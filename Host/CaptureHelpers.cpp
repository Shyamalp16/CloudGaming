#include "CaptureHelpers.h"
#include "GlobalTime.h"
#include "AudioCapturer.h"
#include <chrono>
#include <iostream>
#include <iomanip>
#include <wincodec.h>
#include <string>
#include <filesystem>
#include <windows.h>
#include <queue>
#include <algorithm>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.DirectX.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>
#include <windows.graphics.directx.direct3d11.interop.h>
#include <d3d11.h>
#include <winrt/base.h>
#include "Encoder.h"
#include "AdaptiveQualityControl.h"
#include "VideoMetrics.h"
#include "EtwMarkers.h"
#include <avrt.h>
#include <deque>
#pragma comment(lib, "Avrt.lib")

// Constants to replace magic numbers
static constexpr int kDefaultFramePoolBuffers = 3;
static constexpr int kMaxFramePoolBuffers = 16;
static constexpr int kDefaultTargetFps = 120;
static constexpr int kMaxQueuedFramesDefault = 2;

using namespace std::chrono;
using namespace winrt;
using namespace winrt::Windows::Graphics::Capture;
using namespace winrt::Windows::Graphics::DirectX;
using namespace winrt::Windows::Graphics::DirectX::Direct3D11;
using namespace winrt::Windows::Foundation;

//Global Variables
std::vector<std::thread> workerThreads;
std::atomic<bool> isCapturing{ false };
// (Removed) advanced WGC pool tracking

struct FrameData {
    int sequenceNumber = 0;
    winrt::com_ptr<ID3D11Texture2D> texture;
    int64_t captureTimestampUs = 0;
    int64_t enqueueSteadyUs = 0;
    winrt::Windows::Graphics::SizeInt32 contentSize{ 0, 0 };
};

// (Removed) FrameComparator used by priority_queue in older design

// Single-producer single-consumer ring buffer: capture callback -> encoder thread
static size_t g_maxQueuedFrames = kMaxQueuedFramesDefault;
std::atomic<int> frameSequenceCounter{ 0 };

// The queue and texture pool share one short-lived lock. This is intentionally
// not a lock-free ring: dropping the oldest item makes the producer a second
// consumer, which invalidates SPSC assumptions and races COM pointer writes.
static std::mutex g_queueMutex;
static std::condition_variable g_queueCV;
static std::deque<FrameData> g_frameQueue;
static std::vector<winrt::com_ptr<ID3D11Texture2D>> g_freeTextures;
static size_t g_copyPoolSize = 4;
static size_t g_allocatedTextures = 0;

// Metrics
static std::atomic<uint64_t> g_overwriteDrops{ 0 }; // times we overwrote oldest due to full ring
static std::atomic<uint64_t> g_backpressureSkips{ 0 }; // frames skipped by consumer due to encoder backpressure
static std::atomic<int> g_lastProcessedSeq{ -1 }; // monotonicity tracking

// Timestamp source tracking for A/V sync debugging
static std::atomic<uint64_t> g_systemRelativeTimeFrames{ 0 }; // frames using WGC SystemRelativeTime
static std::atomic<uint64_t> g_fallbackTimeFrames{ 0 }; // frames using audio reference clock fallback
static std::atomic<uint64_t> g_outOfOrder{ 0 }; // frames observed out of order

static std::atomic<int> g_targetFps{kDefaultTargetFps};
void SetCaptureTargetFps(int fps) { if (fps > 0) g_targetFps.store(fps); }

void SetMaxQueuedFrames(int maxDepth) {
    if (maxDepth < 1) maxDepth = 1;
    g_maxQueuedFrames = static_cast<size_t>(maxDepth);
}

void SetCopyPoolSize(int poolSize) {
    g_copyPoolSize = static_cast<size_t>(std::clamp(poolSize, 2, 32));
}

// MMCSS config
static std::atomic<bool> g_enableMmcss{true};
static std::atomic<int>  g_mmcssPriority{2}; // 2 ~ HIGH
void SetMmcssConfig(bool enable, int priority) {
    g_enableMmcss.store(enable);
    // CRITICAL can starve the captured game. Capture work is bounded and should
    // use at most AVRT_PRIORITY_HIGH.
    g_mmcssPriority.store(std::clamp(priority, 0, 2));
}

// Session options
static std::atomic<bool> g_cursorCaptureEnabled{true};
static std::atomic<bool> g_borderRequired{true};
void SetCursorCaptureEnabled(bool enable) { g_cursorCaptureEnabled.store(enable); }
void SetBorderRequired(bool required) { g_borderRequired.store(required); }
// MinUpdateInterval (100ns units). 0 means not set
static std::atomic<long long> g_minUpdateInterval100ns{0};
void SetMinUpdateInterval100ns(long long interval100ns) {
    if (interval100ns < 0) interval100ns = 0;
    g_minUpdateInterval100ns.store(interval100ns);
}
static int64_t SteadyNowUs() {
    return duration_cast<microseconds>(steady_clock::now().time_since_epoch()).count();
}

static bool TextureMatches(ID3D11Texture2D* texture, const D3D11_TEXTURE2D_DESC& sourceDesc) {
    if (!texture) return false;
    D3D11_TEXTURE2D_DESC desc{};
    texture->GetDesc(&desc);
    return desc.Width == sourceDesc.Width && desc.Height == sourceDesc.Height &&
           desc.Format == sourceDesc.Format;
}

static void ReturnTextureLocked(winrt::com_ptr<ID3D11Texture2D>&& texture) {
    if (texture) g_freeTextures.push_back(std::move(texture));
}

static winrt::com_ptr<ID3D11Texture2D> AcquireCopyTextureLocked(
    ID3D11Device* device,
    const D3D11_TEXTURE2D_DESC& sourceDesc)
{
    auto matching = std::find_if(g_freeTextures.begin(), g_freeTextures.end(),
        [&](const auto& texture) { return TextureMatches(texture.get(), sourceDesc); });
    if (matching != g_freeTextures.end()) {
        auto texture = std::move(*matching);
        g_freeTextures.erase(matching);
        return texture;
    }

    // If the pool is exhausted, reclaim the oldest queued frame. The texture is
    // safe to reuse because an item already removed by the consumer is not in
    // this queue. This gives cloud-gaming traffic latest-frame semantics.
    if (g_allocatedTextures >= g_copyPoolSize && !g_frameQueue.empty()) {
        auto texture = std::move(g_frameQueue.front().texture);
        g_frameQueue.pop_front();
        g_overwriteDrops.fetch_add(1, std::memory_order_relaxed);
        VideoMetrics::inc(VideoMetrics::overwriteDrops());
        if (TextureMatches(texture.get(), sourceDesc)) return texture;
        texture = nullptr;
        --g_allocatedTextures;
    }

    if (g_allocatedTextures >= g_copyPoolSize) return nullptr;

    D3D11_TEXTURE2D_DESC copyDesc = sourceDesc;
    copyDesc.MipLevels = 1;
    copyDesc.ArraySize = 1;
    copyDesc.SampleDesc.Count = 1;
    copyDesc.SampleDesc.Quality = 0;
    copyDesc.Usage = D3D11_USAGE_DEFAULT;
    copyDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    copyDesc.CPUAccessFlags = 0;
    copyDesc.MiscFlags = 0;

    winrt::com_ptr<ID3D11Texture2D> texture;
    if (FAILED(device->CreateTexture2D(&copyDesc, nullptr, texture.put()))) return nullptr;
    ++g_allocatedTextures;
    return texture;
}

class CallbackMmcssRegistration {
public:
    void ensureRegistered() {
        if (attempted_ || !g_enableMmcss.load(std::memory_order_relaxed)) return;
        attempted_ = true;
        DWORD taskIndex = 0;
        handle_ = AvSetMmThreadCharacteristicsW(L"Capture", &taskIndex);
        if (handle_) {
            AVRT_PRIORITY priority = g_mmcssPriority.load(std::memory_order_relaxed) >= 2
                ? AVRT_PRIORITY_HIGH : AVRT_PRIORITY_NORMAL;
            AvSetMmThreadPriority(handle_, priority);
        } else {
            SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
        }
    }

    ~CallbackMmcssRegistration() {
        if (handle_) AvRevertMmThreadCharacteristics(handle_);
    }

private:
    bool attempted_ = false;
    HANDLE handle_ = nullptr;
};

winrt::com_ptr<ID3D11Texture2D> GetTextureFromSurface(
    winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DSurface surface)
{
    auto dxgiInterfaceAccess = surface.as<::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();
    winrt::com_ptr<ID3D11Texture2D> texture;
    winrt::check_hresult(dxgiInterfaceAccess->GetInterface(
        __uuidof(ID3D11Texture2D),
        texture.put_void()));
    return texture;
}

static std::atomic<int> g_framePoolBuffers{kDefaultFramePoolBuffers};
void SetFramePoolBuffers(int bufferCount) {
    if (bufferCount < 1) bufferCount = 1;
    if (bufferCount > 16) bufferCount = 16;
    g_framePoolBuffers.store(bufferCount);
}

Direct3D11CaptureFramePool createFreeThreadedFramePool(
    Direct3D11::IDirect3DDevice d3dDevice,
    winrt::Windows::Graphics::SizeInt32 size)
{
    int numberOfBuffers = g_framePoolBuffers.load();
    if (numberOfBuffers < 1) numberOfBuffers = 1;
    if (numberOfBuffers > kMaxFramePoolBuffers) numberOfBuffers = kMaxFramePoolBuffers;
    auto pixelFormat = DirectXPixelFormat::B8G8R8A8UIntNormalized;
    Direct3D11CaptureFramePool framePool = nullptr;
    try
    {
        framePool = Direct3D11CaptureFramePool::CreateFreeThreaded(
            d3dDevice,
            pixelFormat,
            numberOfBuffers,
            size
        );
    }
    catch (const winrt::hresult_error& e)
    {
        std::wcerr << L"[createFreeThreadedFramePool] Failed. HRESULT=0x"
            << std::hex << e.code() << std::endl;
    }
    return framePool;
}

GraphicsCaptureSession createCaptureSession(
    GraphicsCaptureItem item,
    Direct3D11CaptureFramePool framePool)
{
    GraphicsCaptureSession session = nullptr;
    try
    {
        session = framePool.CreateCaptureSession(item);
        session.IsCursorCaptureEnabled(g_cursorCaptureEnabled.load());
        // Best-effort: some OS versions expose IsBorderRequired
        try { session.IsBorderRequired(g_borderRequired.load()); } catch (...) {}
        // Configure MinUpdateInterval from the single host.video.fps source.
        try {
            auto val = g_minUpdateInterval100ns.load();
            if (val > 0) {
                session.MinUpdateInterval(winrt::Windows::Foundation::TimeSpan{ val });
                std::wcout << L"[WGC] MinUpdateInterval set to " << val << L" (100ns units)" << std::endl;
            }
        } catch (...) {
            // Older Windows builds may not support the property. Leaving it
            // unset is preferable to the old 10 fps fallback.
            std::wcout << L"[WGC] MinUpdateInterval unsupported; using compositor cadence" << std::endl;
        }
    }
    catch (const winrt::hresult_error& e)
    {
        std::wcerr << L"[createCaptureSession] Failed. HRESULT=0x"
            << std::hex << e.code() << std::endl;
    }
    return session;
}

winrt::event_token FrameArrivedEventRegistration(Direct3D11CaptureFramePool const& framePool) {
    auto handler = TypedEventHandler<Direct3D11CaptureFramePool, winrt::Windows::Foundation::IInspectable>(
        [](Direct3D11CaptureFramePool sender, winrt::Windows::Foundation::IInspectable) {
            try {
                thread_local CallbackMmcssRegistration callbackPriority;
                callbackPriority.ensureRegistered();

                auto device = GetD3DDevice();
                if (!device) return;
                winrt::com_ptr<ID3D11DeviceContext> context;
                device->GetImmediateContext(context.put());

                for (;;) {
                    auto frame = sender.TryGetNextFrame();
                    if (!frame) break;
                    VideoMetrics::inc(VideoMetrics::wgcFramesArrived());

                    auto surface = frame.Surface();
                    if (!surface) continue;
                    int sequenceNumber = frameSequenceCounter++;

                    int64_t timestamp = 0;
                    try {
                        auto srt = frame.SystemRelativeTime();
                        timestamp = static_cast<int64_t>(srt.count() / 10);
                    } catch (...) {
                        timestamp = 0;
                    }

                    if (timestamp > 0) {
                        g_systemRelativeTimeFrames.fetch_add(1, std::memory_order_relaxed);
                    } else {
                        timestamp = AudioCapturer::GetSharedReferenceTimeUs();
                        g_fallbackTimeFrames.fetch_add(1, std::memory_order_relaxed);
                    }

                    winrt::Windows::Graphics::SizeInt32 contentSize{ 0, 0 };
                    try { contentSize = frame.ContentSize(); } catch (...) {}

                    // Copy while Direct3D11CaptureFrame is alive. Once this loop
                    // iteration ends WGC may immediately recycle its surface.
                    auto source = GetTextureFromSurface(surface);
                    if (!source) continue;
                    D3D11_TEXTURE2D_DESC sourceDesc{};
                    source->GetDesc(&sourceDesc);

                    winrt::com_ptr<ID3D11Texture2D> ownedTexture;
                    {
                        std::lock_guard<std::mutex> lock(g_queueMutex);
                        ownedTexture = AcquireCopyTextureLocked(device.get(), sourceDesc);
                    }
                    if (!ownedTexture) {
                        g_overwriteDrops.fetch_add(1, std::memory_order_relaxed);
                        VideoMetrics::inc(VideoMetrics::overwriteDrops());
                        continue;
                    }

                    context->CopyResource(ownedTexture.get(), source.get());
                    VideoMetrics::inc(VideoMetrics::captureCopies());

                    {
                        std::lock_guard<std::mutex> lock(g_queueMutex);
                        while (g_frameQueue.size() >= g_maxQueuedFrames) {
                            auto dropped = std::move(g_frameQueue.front());
                            g_frameQueue.pop_front();
                            ReturnTextureLocked(std::move(dropped.texture));
                            g_overwriteDrops.fetch_add(1, std::memory_order_relaxed);
                            VideoMetrics::inc(VideoMetrics::overwriteDrops());
                        }
                        g_frameQueue.push_back(FrameData{
                            sequenceNumber,
                            std::move(ownedTexture),
                            timestamp,
                            SteadyNowUs(),
                            contentSize
                        });
                        VideoMetrics::queueDepth().store(
                            static_cast<uint64_t>(g_frameQueue.size()), std::memory_order_relaxed);
                    }
                    g_queueCV.notify_one();
                }
            }
            catch (const std::exception& e) {
                std::wcerr << L"[FrameArrived] Exception: " << e.what() << L"\n";
            }
        });
    return framePool.FrameArrived(handler);
}

static size_t QueueSize() {
    std::lock_guard<std::mutex> lock(g_queueMutex);
    return g_frameQueue.size();
}

static void RecycleFrameTexture(FrameData& frame) {
    std::lock_guard<std::mutex> lock(g_queueMutex);
    ReturnTextureLocked(std::move(frame.texture));
}

void StartCapture() {
    // Initialize shared reference clock for AV synchronization
    AudioCapturer::InitializeSharedReferenceClock();

    for (auto& thread : workerThreads) {
        if (thread.joinable()) thread.join();
    }
    workerThreads.clear();

    isCapturing.store(true);
    frameSequenceCounter.store(0);

    // Initialize the owned-texture queue. Keep at least one texture outside the
    // queue for the callback's in-progress copy.
    {
        std::lock_guard<std::mutex> lock(g_queueMutex);
        g_copyPoolSize = std::max(g_copyPoolSize, g_maxQueuedFrames + 1);
        g_frameQueue.clear();
        g_freeTextures.clear();
        g_allocatedTextures = 0;
        g_overwriteDrops.store(0, std::memory_order_relaxed);
        g_backpressureSkips.store(0, std::memory_order_relaxed);
        g_outOfOrder.store(0, std::memory_order_relaxed);
        g_lastProcessedSeq.store(-1, std::memory_order_relaxed);
    }

    // Single encode/transmit consumer thread
    workerThreads.emplace_back([](){
        // Elevate this consumer thread to MMCSS 'Capture' to avoid starving the game
        HANDLE mmcssHandle = nullptr;
        if (g_enableMmcss.load()) {
            DWORD taskIndex = 0;
            mmcssHandle = AvSetMmThreadCharacteristicsW(L"Capture", &taskIndex);
            if (mmcssHandle) {
                int prio = g_mmcssPriority.load();
                AVRT_PRIORITY mapped = AVRT_PRIORITY_NORMAL;
                if (prio <= 0) mapped = AVRT_PRIORITY_LOW;
                else if (prio == 1) mapped = AVRT_PRIORITY_NORMAL;
                else mapped = AVRT_PRIORITY_HIGH;
                AvSetMmThreadPriority(mmcssHandle, mapped);
            }
        }
        auto device = GetD3DDevice();
        winrt::com_ptr<ID3D11DeviceContext> context;
        device->GetImmediateContext(context.put());

        int lastInitW = 0;
        int lastInitH = 0;
        auto lastLog = std::chrono::steady_clock::now();
        int submitCount = 0;
        uint64_t lastOverwriteDrops = 0;
        uint64_t lastBpSkips = 0;
        uint64_t lastOutOfOrder = 0;

        uint64_t lastArrived = VideoMetrics::wgcFramesArrived().load(std::memory_order_relaxed);
        auto fpsWindowStart = std::chrono::steady_clock::now();

        for (;;) {
            FrameData job{};

            // Wake immediately when a new frame lands; also wakes on shutdown.
            {
                std::unique_lock<std::mutex> lock(g_queueMutex);
                g_queueCV.wait(lock, [] { return !g_frameQueue.empty() || !isCapturing.load(); });
                if (!isCapturing.load() && g_frameQueue.empty()) break;

                // Encode the freshest queued frame. Older queued frames cannot
                // increase delivered FPS once the encoder has fallen behind.
                while (g_frameQueue.size() > 1) {
                    auto dropped = std::move(g_frameQueue.front());
                    g_frameQueue.pop_front();
                    ReturnTextureLocked(std::move(dropped.texture));
                    g_backpressureSkips.fetch_add(1, std::memory_order_relaxed);
                    VideoMetrics::inc(VideoMetrics::backpressureSkips());
                }
                job = std::move(g_frameQueue.front());
                g_frameQueue.pop_front();
                VideoMetrics::queueDepth().store(0, std::memory_order_relaxed);
            }

            if (SteadyNowUs() - job.enqueueSteadyUs > 100000) {
                g_backpressureSkips.fetch_add(1, std::memory_order_relaxed);
                VideoMetrics::inc(VideoMetrics::backpressureSkips());
                RecycleFrameTexture(job);
                continue;
            }

            // Content-size change detection
            {
                static std::atomic<int> lastLoggedW{ 0 };
                static std::atomic<int> lastLoggedH{ 0 };
                if (job.contentSize.Width  != lastLoggedW.load() ||
                    job.contentSize.Height != lastLoggedH.load()) {
                    std::wcout << L"[WGC] ContentSize: "
                               << job.contentSize.Width << L"x" << job.contentSize.Height << std::endl;
                    lastLoggedW.store(job.contentSize.Width);
                    lastLoggedH.store(job.contentSize.Height);
                }
            }

            // Enforce monotonic sequence; count any out-of-order occurrence
            {
                int prev = g_lastProcessedSeq.load(std::memory_order_relaxed);
                if (prev >= 0 && job.sequenceNumber <= prev) {
                    g_outOfOrder.fetch_add(1, std::memory_order_relaxed);
                } else {
                    g_lastProcessedSeq.store(job.sequenceNumber, std::memory_order_relaxed);
                }
            }
            if (!job.texture) continue;

            D3D11_TEXTURE2D_DESC desc{};
            job.texture->GetDesc(&desc);
            int encW = static_cast<int>(desc.Width & ~1U);
            int encH = static_cast<int>(desc.Height & ~1U);
            if (lastInitW != encW || lastInitH != encH) {
                if (lastInitW != 0 && lastInitH != 0) Encoder::FinalizeEncoder();
                Encoder::InitializeEncoder("output.mp4", encW, encH, g_targetFps.load());
                lastInitW = encW; lastInitH = encH;
            }

            auto qualityDecision = AdaptiveQualityControl::checkFrameDropping();
            if (qualityDecision.shouldDropFrame) {
                g_backpressureSkips.fetch_add(1, std::memory_order_relaxed);
                VideoMetrics::inc(VideoMetrics::backpressureSkips());
                RecycleFrameTexture(job);
                continue;
            }

            int slot = -1; ID3D11Texture2D* nv12 = nullptr;
            if (Encoder::AcquireHwInputSurface(slot, &nv12) &&
                Encoder::VideoProcessorBltToSlot(job.texture.get(), slot)) {
                VideoMetrics::inc(VideoMetrics::vpSubmissions());
                if (Encoder::SubmitHwFrame(slot, job.captureTimestampUs)) submitCount++;
            }
            RecycleFrameTexture(job);

            auto nowDbg = std::chrono::steady_clock::now();
            if (std::chrono::duration_cast<std::chrono::seconds>(nowDbg - lastLog).count() >= 1) {
                uint64_t od = g_overwriteDrops.load(std::memory_order_relaxed);
                uint64_t bp = g_backpressureSkips.load(std::memory_order_relaxed);
                size_t qsz = QueueSize();
                VideoMetrics::queueDepth().store(static_cast<uint64_t>(qsz), std::memory_order_relaxed);
                uint64_t oo = g_outOfOrder.load(std::memory_order_relaxed);
                uint64_t arrived = VideoMetrics::wgcFramesArrived().load(std::memory_order_relaxed);
                auto fpsElapsedUs = duration_cast<microseconds>(nowDbg - fpsWindowStart).count();
                double captureFps = fpsElapsedUs > 0
                    ? static_cast<double>(arrived - lastArrived) * 1000000.0 / static_cast<double>(fpsElapsedUs)
                    : 0.0;
                VideoMetrics::ewmaUpdate(VideoMetrics::captureFps(), captureFps);
                std::wcout << L"[Stats] WGC=" << std::fixed << std::setprecision(1) << captureFps
                           << L" fps, Encode=" << submitCount
                           << L"/s, Target=" << g_targetFps.load()
                           << L" fps, QueueDepth=" << qsz
                           << L", OverwriteDrops/s=" << (od - lastOverwriteDrops)
                           << L", BPSkips/s=" << (bp - lastBpSkips)
                           << L", OutOfOrder/s=" << (oo - lastOutOfOrder)
                           << std::endl;
                lastOverwriteDrops = od;
                lastBpSkips = bp;
                lastOutOfOrder = oo;
                submitCount = 0;
                lastLog = nowDbg;
                lastArrived = arrived;
                fpsWindowStart = nowDbg;
            }
        }
        if (mmcssHandle) {
            AvRevertMmThreadCharacteristics(mmcssHandle);
        }
    });
}

void StopCapture(winrt::event_token& token, winrt::Windows::Graphics::Capture::Direct3D11CaptureFramePool const& framePool) {
    // Stop callbacks before waiting for the consumer; otherwise the producer can
    // keep refilling the queue while shutdown waits for it to become empty.
    framePool.FrameArrived(token);
    isCapturing.store(false);
    g_queueCV.notify_all();

    for (auto& thread : workerThreads) {
        if (thread.joinable()) thread.join();
    }
    workerThreads.clear();

    // Release all application-owned capture textures after the consumer exits.
    {
        std::lock_guard<std::mutex> lock(g_queueMutex);
        g_frameQueue.clear();
        g_freeTextures.clear();
        g_allocatedTextures = 0;
    }

    framePool.Close();
    Encoder::FinalizeEncoder();
}
