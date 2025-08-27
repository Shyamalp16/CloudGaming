#include "CaptureHelpers.h"
#include "GlobalTime.h"
#include <chrono>
#include <iostream>
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

// Constants to replace magic numbers
static constexpr int kDefaultFramePoolBuffers = 3;
static constexpr int kMaxFramePoolBuffers = 16;
static constexpr int kDefaultTargetFps = 120;
static constexpr int kMaxQueuedFramesDefault = 8;
static constexpr int kPacingMaxBurstFramesDefault = 8;
static constexpr int kOneSecondUs = 1000000;

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
    int sequenceNumber;
    winrt::com_ptr<ID3D11Texture2D> texture; // private copy for lifetime safety
    int64_t timestamp;
    winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DSurface surface{ nullptr }; // deferred copy source
};

struct FrameComparator {
    bool operator()(const FrameData& a, const FrameData& b) const {
        return a.sequenceNumber > b.sequenceNumber;
    };
};

// Single-producer single-consumer ring buffer: capture callback -> encoder thread
static size_t g_maxQueuedFrames = kMaxQueuedFramesDefault;
std::atomic<int> frameSequenceCounter{ 0 };

// Ring storage and indices
static std::vector<FrameData> g_ring;
static std::atomic<size_t> g_ringHead{ 0 }; // next write position (producer moves)
static std::atomic<size_t> g_ringTail{ 0 }; // next read position (consumer moves)
static size_t g_ringCapacity = 0;           // equals g_maxQueuedFrames (number of slots)
static std::mutex g_ringMutex;              // only for consumer wait
static std::condition_variable g_ringCV;    // wake consumer when producer pushes

// Metrics
static std::atomic<uint64_t> g_overwriteDrops{ 0 }; // times we overwrote oldest due to full ring
static std::atomic<uint64_t> g_backpressureSkips{ 0 }; // frames skipped by consumer due to encoder backpressure

static std::atomic<int> g_targetFps{kDefaultTargetFps};
void SetCaptureTargetFps(int fps) { if (fps > 0) g_targetFps.store(fps); }

void SetMaxQueuedFrames(int maxDepth) {
    if (maxDepth < 1) maxDepth = 1;
    g_maxQueuedFrames = static_cast<size_t>(maxDepth);
}

// Backpressure drop policy defaults
static std::atomic<int> g_dropWindowMs{200};
static std::atomic<int> g_dropMinEvents{2};
void SetBackpressureDropPolicy(int windowMs, int minEvents) {
    if (windowMs < 1) windowMs = 1;
    if (minEvents < 1) minEvents = 1;
    g_dropWindowMs.store(windowMs);
    g_dropMinEvents.store(minEvents);
}

// MMCSS config
static std::atomic<bool> g_enableMmcss{true};
static std::atomic<int>  g_mmcssPriority{2}; // 2 ~ HIGH
void SetMmcssConfig(bool enable, int priority) {
    g_enableMmcss.store(enable);
    g_mmcssPriority.store(std::clamp(priority, 0, 3));
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

// (Removed) Texture copy pool (unused)
// Allow temporary bursts before dropping oldest frames when encoder is not backlogged
static std::atomic<int> g_pacingMaxBurstFrames{kPacingMaxBurstFramesDefault};
void SetPacingMaxBurstFrames(int frames) {
    if (frames < 1) frames = 1;
    if (frames > 128) frames = 128;
    g_pacingMaxBurstFrames.store(frames);
}

static DXGI_FORMAT CoerceFormatForVideoProcessor(DXGI_FORMAT fmt) {
    switch (fmt) {
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: return DXGI_FORMAT_B8G8R8A8_UNORM;
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: return DXGI_FORMAT_R8G8B8A8_UNORM;
    default: return fmt;
    }
}

// (Removed) RecreateCopyPool (unused)

// (Removed) AcquirePoolSlot (unused)

// (Removed) ReleasePoolSlot (unused)

// (Removed) SetCopyPoolSize (unused)

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

static std::atomic<int> g_framePoolBuffers{3};
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
        // Configure MinUpdateInterval if requested
        try {
            auto val = g_minUpdateInterval100ns.load();
            if (val > 0) {
                session.MinUpdateInterval(winrt::Windows::Foundation::TimeSpan{ val });
                std::wcout << L"[WGC] MinUpdateInterval set to " << val << L" (100ns units)" << std::endl;
            }
        } catch (...) {}
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
                for (;;) {
                    auto frame = sender.TryGetNextFrame();
                    if (!frame) break;

                    // (Removed) pool Recreate; revert to previous behavior

                    auto surface = frame.Surface();
                    if (!surface) continue;

                    int sequenceNumber = frameSequenceCounter++;
                    // Timestamp in microseconds for encoder/Go layer
                    // Prefer WGC SystemRelativeTime (100ns units) for stable, system-aligned timestamps
                    int64_t timestamp = 0;
                    try {
                        auto srt = frame.SystemRelativeTime();
                        // Convert 100ns units to microseconds
                        timestamp = static_cast<int64_t>(srt.count() / 10);
                    } catch (...) {}
                    if (timestamp <= 0) {
                        // Fallback to steady_clock if SRT unavailable
                        timestamp = duration_cast<microseconds>(steady_clock::now().time_since_epoch()).count();
                    }
                    // --- Debug: capture fps ---
                    {
                        static int capCount = 0;
                        static int64_t lastTs = 0;
                        capCount++;
                        if (lastTs == 0) lastTs = timestamp;
                        if (timestamp - lastTs >= kOneSecondUs) { // 1s
                            std::wcout << L"[WGC] frames/s: " << capCount << std::endl;
                            capCount = 0;
                            lastTs = timestamp;
                        }
                    }
                    // Log ContentSize when it changes
                    try {
                        auto cs = frame.ContentSize();
                        static std::atomic<int> lastLoggedW{ 0 };
                        static std::atomic<int> lastLoggedH{ 0 };
                        if (cs.Width != lastLoggedW.load() || cs.Height != lastLoggedH.load()) {
                            std::wcout << L"[WGC] ContentSize changed: " << cs.Width << L"x" << cs.Height << std::endl;
                            lastLoggedW.store(cs.Width);
                            lastLoggedH.store(cs.Height);
                        }
                    } catch (...) {}
                    // Ultra-light: push surface + timestamp into SPSC ring (latest-frame-wins)
                    if (g_ringCapacity > 0) {
                        // Compute next head and detect full
                        size_t head = g_ringHead.load(std::memory_order_relaxed);
                        size_t nextHead = (head + 1) % g_ringCapacity;
                        size_t tail = g_ringTail.load(std::memory_order_acquire);
                        if (nextHead == tail) {
                            // Full: drop oldest (advance tail) to keep latest
                            g_ringTail.store((tail + 1) % g_ringCapacity, std::memory_order_release);
                            g_overwriteDrops.fetch_add(1, std::memory_order_relaxed);
                        }
                        g_ring[head].sequenceNumber = sequenceNumber;
                        g_ring[head].texture = nullptr;
                        g_ring[head].timestamp = timestamp;
                        g_ring[head].surface = surface;
                        g_ringHead.store(nextHead, std::memory_order_release);
                        // Notify consumer
                        g_ringCV.notify_one();
                    }
                }
            }
            catch (const std::exception& e) {
                std::wcerr << L"[FrameArrived] Exception: " << e.what() << L"\n";
            }
        });
    return framePool.FrameArrived(handler);
}

// Helper: ring size (approximate, safe for SPSC)
static inline size_t RingSize() {
    size_t head = g_ringHead.load(std::memory_order_acquire);
    size_t tail = g_ringTail.load(std::memory_order_acquire);
    if (head >= tail) return head - tail;
    return g_ringCapacity - (tail - head);
}

void StartCapture() {
    for (auto& thread : workerThreads) {
        if (thread.joinable()) thread.join();
    }
    workerThreads.clear();

    isCapturing.store(true);
    frameSequenceCounter.store(0);

    // Initialize ring buffer storage
    {
        std::lock_guard<std::mutex> lock(g_ringMutex);
        g_ringCapacity = std::max<size_t>(g_maxQueuedFrames, 2);
        g_ring.assign(g_ringCapacity, FrameData{});
        g_ringHead.store(0, std::memory_order_relaxed);
        g_ringTail.store(0, std::memory_order_relaxed);
        g_overwriteDrops.store(0, std::memory_order_relaxed);
        g_backpressureSkips.store(0, std::memory_order_relaxed);
    }

    // Single encode/transmit consumer thread
    workerThreads.emplace_back([](){
        auto device = GetD3DDevice();
        winrt::com_ptr<ID3D11DeviceContext> context;
        device->GetImmediateContext(context.put());

        static int lastInitW = 0;
        static int lastInitH = 0;
        static auto lastLog = std::chrono::steady_clock::now();
        static int submitCount = 0;
        static uint64_t lastOverwriteDrops = 0;
        static uint64_t lastBpSkips = 0;

        while (isCapturing.load()) {
            FrameData job{};
            // Wait for available frame or shutdown
            {
                std::unique_lock<std::mutex> lk(g_ringMutex);
                g_ringCV.wait(lk, []{ return (RingSize() > 0) || !isCapturing.load(); });
                if (!isCapturing.load() && RingSize() == 0) break;
            }

            // If encoder is backlogged, skip to latest frame (drop all but last)
            {
                bool backlogged = Encoder::IsBacklogged(g_dropWindowMs.load(), g_dropMinEvents.load());
                if (backlogged) {
                    size_t head = g_ringHead.load(std::memory_order_acquire);
                    size_t tail = g_ringTail.load(std::memory_order_acquire);
                    size_t sz = (head >= tail) ? (head - tail) : (g_ringCapacity - (tail - head));
                    if (sz > 1) {
                        // keep only the newest element
                        size_t newTail = (head + g_ringCapacity - 1) % g_ringCapacity;
                        // Count how many we skipped
                        size_t skipped = (sz - 1);
                        g_ringTail.store(newTail, std::memory_order_release);
                        g_backpressureSkips.fetch_add(skipped, std::memory_order_relaxed);
                    }
                }
            }

            // Pop next available frame
            {
                size_t tail = g_ringTail.load(std::memory_order_acquire);
                size_t head = g_ringHead.load(std::memory_order_acquire);
                if (tail == head) continue; // spurious wake
                job = g_ring[tail];
                g_ringTail.store((tail + 1) % g_ringCapacity, std::memory_order_release);
            }
            if (!job.texture && !job.surface) continue;

            winrt::com_ptr<ID3D11Texture2D> tex = job.texture;
            if (!tex && job.surface) {
                tex = GetTextureFromSurface(job.surface);
            }
            if (!tex) continue;

            D3D11_TEXTURE2D_DESC desc{};
            tex->GetDesc(&desc);
            int encW = static_cast<int>(desc.Width & ~1U);
            int encH = static_cast<int>(desc.Height & ~1U);
            if (lastInitW == 0 || lastInitH == 0) {
                Encoder::InitializeEncoder("output.mp4", encW, encH, g_targetFps.load());
                lastInitW = encW; lastInitH = encH;
            }

            // Always attempt submit; encoder internally drains on EAGAIN
            int slot = -1; ID3D11Texture2D* nv12 = nullptr;
            if (Encoder::AcquireHwInputSurface(slot, &nv12) && Encoder::VideoProcessorBltToSlot(tex.get(), slot)) {
                Encoder::SubmitHwFrame(slot, job.timestamp);
                submitCount++;
            }

            auto nowDbg = std::chrono::steady_clock::now();
            if (std::chrono::duration_cast<std::chrono::seconds>(nowDbg - lastLog).count() >= 1) {
                uint64_t od = g_overwriteDrops.load(std::memory_order_relaxed);
                uint64_t bp = g_backpressureSkips.load(std::memory_order_relaxed);
                size_t qsz = RingSize();
                std::wcout << L"[Stats] Encode=" << submitCount
                           << L"/s, Target=" << g_targetFps.load()
                           << L" fps, QueueDepth=" << qsz
                           << L", OverwriteDrops/s=" << (od - lastOverwriteDrops)
                           << L", BPSkips/s=" << (bp - lastBpSkips)
                           << std::endl;
                lastOverwriteDrops = od;
                lastBpSkips = bp;
                submitCount = 0;
                lastLog = nowDbg;
            }
        }
    });
}

void StopCapture(winrt::event_token& token, winrt::Windows::Graphics::Capture::Direct3D11CaptureFramePool const& framePool) {
    isCapturing.store(false);
    g_ringCV.notify_all();

    for (auto& thread : workerThreads) {
        if (thread.joinable()) thread.join();
    }
    workerThreads.clear();

    // Clear ring storage
    {
        std::lock_guard<std::mutex> lock(g_ringMutex);
        g_ring.clear();
        g_ringCapacity = 0;
        g_ringHead.store(0, std::memory_order_relaxed);
        g_ringTail.store(0, std::memory_order_relaxed);
    }
    // Removed unused encode queue cleanup

    framePool.FrameArrived(token);
    framePool.Close();
    Encoder::FinalizeEncoder();
}