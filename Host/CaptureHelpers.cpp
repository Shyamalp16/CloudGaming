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
#include "VideoMetrics.h"
#include "EtwMarkers.h"
#include <avrt.h>
#pragma comment(lib, "Avrt.lib")

// Constants to replace magic numbers
static constexpr int kDefaultFramePoolBuffers = 3;
static constexpr int kMaxFramePoolBuffers = 16;
static constexpr int kDefaultTargetFps = 120;
static constexpr int kMaxQueuedFramesDefault = 2;
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

// Deferred frame metadata for consumer thread processing
struct DeferredFrameMetadata {
    int64_t systemRelativeTimeUs;
    winrt::Windows::Graphics::SizeInt32 contentSize;
    bool shouldSkipUnchanged;
};

// (Removed) FrameComparator used by priority_queue in older design

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

// Deferred metadata ring for consumer thread (same capacity as frame ring)
static std::vector<DeferredFrameMetadata> g_metadataRing;
static std::atomic<size_t> g_metadataHead{ 0 };
static std::atomic<size_t> g_metadataTail{ 0 };
static size_t g_metadataCapacity = 0;

// Metrics
static std::atomic<uint64_t> g_overwriteDrops{ 0 }; // times we overwrote oldest due to full ring
static std::atomic<uint64_t> g_backpressureSkips{ 0 }; // frames skipped by consumer due to encoder backpressure
static std::atomic<int> g_lastProcessedSeq{ -1 }; // monotonicity tracking

// Timestamp source tracking for A/V sync debugging
static std::atomic<uint64_t> g_systemRelativeTimeFrames{ 0 }; // frames using WGC SystemRelativeTime
static std::atomic<uint64_t> g_fallbackTimeFrames{ 0 }; // frames using audio reference clock fallback
static std::chrono::steady_clock::time_point g_lastTimestampReport = std::chrono::steady_clock::now();
static std::atomic<uint64_t> g_outOfOrder{ 0 }; // frames observed out of order

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
// Skip unchanged frames heuristic
static std::atomic<bool> g_skipUnchanged{false};
void SetSkipUnchanged(bool enable) { g_skipUnchanged.store(enable); }

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
        // Configure MinUpdateInterval if requested - ensure it's always set for low latency
        try {
            auto val = g_minUpdateInterval100ns.load();
            if (val > 0) {
                session.MinUpdateInterval(winrt::Windows::Foundation::TimeSpan{ val });
                std::wcout << L"[WGC] MinUpdateInterval set to " << val << L" (100ns units)" << std::endl;
            } else {
                // Fallback: set high-performance default (8.33ms for 120fps) for debugging
                auto fallbackVal = 83333LL; // 8.333ms in 100ns units (120fps)
                session.MinUpdateInterval(winrt::Windows::Foundation::TimeSpan{ fallbackVal });
                std::wcout << L"[WGC] MinUpdateInterval not configured, using high-performance fallback: " << fallbackVal << L" (100ns units, ~120fps)" << std::endl;
            }
        } catch (...) {
            // Exception fallback: set very conservative default to prevent high latency
            try {
                auto exceptionFallback = 1000000LL; // 100ms in 100ns units (10fps)
                session.MinUpdateInterval(winrt::Windows::Foundation::TimeSpan{ exceptionFallback });
                std::wcout << L"[WGC] Exception in MinUpdateInterval, using exception fallback: " << exceptionFallback << L" (100ns units)" << std::endl;
            } catch (...) {}
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
                for (;;) {
                    auto frame = sender.TryGetNextFrame();
                    if (!frame) break;

                    auto surface = frame.Surface();
                    if (!surface) continue;

                    // MINIMAL CALLBACK: Only essential work here to reduce WGC backpressure
                    int sequenceNumber = frameSequenceCounter++;

                    // Extract timestamp with robust validation and fallback logging
                    int64_t timestamp = 0;
                    int64_t systemRelativeTimeUs = 0;
                    bool usedSystemRelativeTime = true;

                    try {
                        auto srt = frame.SystemRelativeTime();
                        systemRelativeTimeUs = static_cast<int64_t>(srt.count() / 10);

                        // Validate the timestamp is reasonable (not negative, not too far in future)
                        auto nowUs = std::chrono::duration_cast<std::chrono::microseconds>(
                            std::chrono::steady_clock::now().time_since_epoch()).count();

                        if (systemRelativeTimeUs > 0 && systemRelativeTimeUs <= (nowUs + 1000000)) { // Within 1 second of now
                            timestamp = systemRelativeTimeUs;
                            g_systemRelativeTimeFrames.fetch_add(1, std::memory_order_relaxed);
                        } else {
                            // Invalid SystemRelativeTime, fall back
                            usedSystemRelativeTime = false;
                            static bool loggedInvalidSrt = false;
                            if (!loggedInvalidSrt) {
                                std::wcout << L"[VideoCapture] WARNING: Invalid SystemRelativeTime (" << systemRelativeTimeUs
                                          << L" μs), falling back to audio reference clock. This may affect A/V sync." << std::endl;
                                loggedInvalidSrt = true;
                            }
                        }
                    } catch (const std::exception& e) {
                        // Exception getting SystemRelativeTime, fall back
                        usedSystemRelativeTime = false;
                        static bool loggedSrtException = false;
                        if (!loggedSrtException) {
                            std::wcout << L"[VideoCapture] WARNING: Exception getting SystemRelativeTime: " << e.what()
                                      << L". Falling back to audio reference clock. This may affect A/V sync." << std::endl;
                            loggedSrtException = true;
                        }
                    } catch (...) {
                        // Generic exception, fall back
                        usedSystemRelativeTime = false;
                        static bool loggedSrtGenericException = false;
                        if (!loggedSrtGenericException) {
                            std::wcout << L"[VideoCapture] WARNING: Unknown exception getting SystemRelativeTime. "
                                      << L"Falling back to audio reference clock. This may affect A/V sync." << std::endl;
                            loggedSrtGenericException = true;
                        }
                    }

                    if (!usedSystemRelativeTime || timestamp <= 0) {
                        timestamp = AudioCapturer::GetSharedReferenceTimeUs();
                        systemRelativeTimeUs = timestamp;
                        g_fallbackTimeFrames.fetch_add(1, std::memory_order_relaxed);

                        // Log fallback usage (one-time warning)
                        static bool loggedFallbackUsage = false;
                        if (!loggedFallbackUsage) {
                            std::wcout << L"[VideoCapture] INFO: Using audio reference clock fallback for video timestamps. "
                                      << L"This ensures A/V sync but may introduce slight timing variations." << std::endl;
                            loggedFallbackUsage = true;
                        }
                    }

                    // Periodic timestamp source reporting for A/V sync debugging
                    {
                        auto now = std::chrono::steady_clock::now();
                        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - g_lastTimestampReport);
                        if (elapsed.count() >= 60) { // Report every minute
                            uint64_t srtFrames = g_systemRelativeTimeFrames.load(std::memory_order_relaxed);
                            uint64_t fallbackFrames = g_fallbackTimeFrames.load(std::memory_order_relaxed);
                            uint64_t totalFrames = srtFrames + fallbackFrames;

                            if (totalFrames > 0) {
                                double srtPercentage = (static_cast<double>(srtFrames) / totalFrames) * 100.0;
                                std::wcout << L"[VideoCapture] Timestamp source stats (last " << elapsed.count()
                                          << L"s): " << srtFrames << L" SRT frames (" << std::fixed << std::setprecision(1)
                                          << srtPercentage << L"%), " << fallbackFrames << L" fallback frames ("
                                          << (100.0 - srtPercentage) << L"%)" << std::endl;
                            }

                            // Reset counters for next period
                            g_systemRelativeTimeFrames.store(0, std::memory_order_relaxed);
                            g_fallbackTimeFrames.store(0, std::memory_order_relaxed);
                            g_lastTimestampReport = now;
                        }
                    }

                    // Extract content size for deferred processing
                    winrt::Windows::Graphics::SizeInt32 contentSize{ 0, 0 };
                    try {
                        contentSize = frame.ContentSize();
                    } catch (...) {}

                    // Ultra-light enqueue: minimal work in callback
                    if (g_ringCapacity > 0) {
                        // Compute next positions
                        size_t head = g_ringHead.load(std::memory_order_relaxed);
                        size_t nextHead = (head + 1) % g_ringCapacity;
                        size_t tail = g_ringTail.load(std::memory_order_acquire);

                        if (nextHead == tail) {
                            // Full: drop oldest (advance tail) to keep latest
                            g_ringTail.store((tail + 1) % g_ringCapacity, std::memory_order_release);
                            g_overwriteDrops.fetch_add(1, std::memory_order_relaxed);
                        }

                        // Store frame data (minimal)
                        g_ring[head].sequenceNumber = sequenceNumber;
                        g_ring[head].texture = nullptr;
                        g_ring[head].timestamp = timestamp;
                        g_ring[head].surface = surface;

                        // Store metadata for deferred processing (consumer thread)
                        size_t metadataHead = g_metadataHead.load(std::memory_order_relaxed);
                        g_metadataRing[metadataHead].systemRelativeTimeUs = systemRelativeTimeUs;
                        g_metadataRing[metadataHead].contentSize = contentSize;
                        g_metadataRing[metadataHead].shouldSkipUnchanged = g_skipUnchanged.load();

                        // Update both ring heads
                        g_ringHead.store(nextHead, std::memory_order_release);
                        g_metadataHead.store((metadataHead + 1) % g_metadataCapacity, std::memory_order_release);

                        // Notify consumer (minimal)
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
    // Initialize shared reference clock for AV synchronization
    AudioCapturer::InitializeSharedReferenceClock();

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

        // Initialize metadata ring with same capacity
        g_metadataCapacity = g_ringCapacity;
        g_metadataRing.assign(g_metadataCapacity, DeferredFrameMetadata{});
        g_metadataHead.store(0, std::memory_order_relaxed);
        g_metadataTail.store(0, std::memory_order_relaxed);
    }

    // Single encode/transmit consumer thread
    workerThreads.emplace_back([](){
        // Elevate this consumer thread to MMCSS 'Games' if enabled
        HANDLE mmcssHandle = nullptr;
        if (g_enableMmcss.load()) {
            DWORD taskIndex = 0;
            mmcssHandle = AvSetMmThreadCharacteristicsW(L"Games", &taskIndex);
            if (mmcssHandle) {
                int prio = g_mmcssPriority.load();
                AVRT_PRIORITY mapped = AVRT_PRIORITY_NORMAL;
                if (prio <= 0) mapped = AVRT_PRIORITY_LOW;
                else if (prio == 1) mapped = AVRT_PRIORITY_NORMAL;
                else if (prio == 2) mapped = AVRT_PRIORITY_HIGH;
                else mapped = AVRT_PRIORITY_CRITICAL;
                AvSetMmThreadPriority(mmcssHandle, mapped);
            }
        }
        auto device = GetD3DDevice();
        winrt::com_ptr<ID3D11DeviceContext> context;
        device->GetImmediateContext(context.put());

        static int lastInitW = 0;
        static int lastInitH = 0;
        static auto lastLog = std::chrono::steady_clock::now();
        static int submitCount = 0;
        static uint64_t lastOverwriteDrops = 0;
        static uint64_t lastBpSkips = 0;
        static uint64_t lastOutOfOrder = 0;

        while (isCapturing.load()) {
            FrameData job{};
            // Wait for available frame or shutdown
            {
                std::unique_lock<std::mutex> lk(g_ringMutex);
                g_ringCV.wait(lk, []{ return (RingSize() > 0) || !isCapturing.load(); });
                if (!isCapturing.load() && RingSize() == 0) break;
            }

            // Enhanced backpressure handling with severity-based response
            {
                Encoder::BackpressureLevel bpLevel = Encoder::GetBackpressureLevel();
                size_t head = g_ringHead.load(std::memory_order_acquire);
                size_t tail = g_ringTail.load(std::memory_order_acquire);
                size_t sz = (head >= tail) ? (head - tail) : (g_ringCapacity - (tail - head));

                // Determine how aggressively to drop based on backpressure severity
                size_t framesToKeep = 1; // Default: keep only latest frame
                bool shouldDrop = false;

                if (bpLevel == Encoder::SEVERE) {
                    // Severe backpressure: drop everything except the absolute latest
                    framesToKeep = 1;
                    shouldDrop = (sz > 1);
                } else if (bpLevel == Encoder::MODERATE) {
                    // Moderate backpressure: keep last 2 frames
                    framesToKeep = 2;
                    shouldDrop = (sz > framesToKeep);
                } else if (bpLevel == Encoder::MILD) {
                    // Mild backpressure: keep last 3 frames
                    framesToKeep = 3;
                    shouldDrop = (sz > framesToKeep);
                } else if (sz >= g_maxQueuedFrames - 1 && sz > 2) {
                    // Fallback: queue is critically full regardless of EAGAIN status
                    framesToKeep = 1;
                    shouldDrop = true;
                }

                if (shouldDrop) {
                    // Calculate new tail position to keep only the desired number of frames
                    size_t newTail;
                    if (sz <= framesToKeep) {
                        newTail = tail; // No change needed
                    } else {
                        newTail = (head + g_ringCapacity - framesToKeep) % g_ringCapacity;
                    }

                    // Count how many we skipped
                    size_t skipped = 0;
                    if (newTail != tail) {
                        if (newTail > tail) {
                            skipped = newTail - tail;
                        } else {
                            skipped = (g_ringCapacity - tail) + newTail;
                        }
                        g_ringTail.store(newTail, std::memory_order_release);
                        g_backpressureSkips.fetch_add(skipped, std::memory_order_relaxed);
                    }

                    // Log backpressure events with severity information (throttled)
                    static auto lastBackpressureLog = std::chrono::steady_clock::now();
                    auto now = std::chrono::steady_clock::now();
                    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - lastBackpressureLog);
                    if (elapsed.count() >= 1000) { // Log at most once per second
                        const wchar_t* severityStr = L"UNKNOWN";
                        switch (bpLevel) {
                            case Encoder::NONE: severityStr = L"NONE"; break;
                            case Encoder::MILD: severityStr = L"MILD"; break;
                            case Encoder::MODERATE: severityStr = L"MODERATE"; break;
                            case Encoder::SEVERE: severityStr = L"SEVERE"; break;
                        }
                        std::wcout << L"[Capture] Backpressure (" << severityStr << L"): dropped " << skipped
                                  << L" frames, kept " << framesToKeep << L", queue was " << sz << L"/" << g_maxQueuedFrames << std::endl;
                        lastBackpressureLog = now;
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
            // DEFERRED PROCESSING: Handle work moved from callback to reduce WGC backpressure
            {
                // Pop corresponding metadata from metadata ring
                size_t metadataTail = g_metadataTail.load(std::memory_order_acquire);
                DeferredFrameMetadata metadata = g_metadataRing[metadataTail];
                g_metadataTail.store((metadataTail + 1) % g_metadataCapacity, std::memory_order_release);

                // 1. FPS calculation (moved from callback)
                {
                    static int capCount = 0;
                    static int64_t windowStartUs = 0;
                    capCount++;
                    if (windowStartUs == 0) windowStartUs = metadata.systemRelativeTimeUs;
                    int64_t elapsed = metadata.systemRelativeTimeUs - windowStartUs;
                    if (elapsed >= kOneSecondUs) { // ~1s window
                        double fps = static_cast<double>(capCount) * 1'000'000.0 / static_cast<double>(elapsed);
                        VideoMetrics::ewmaUpdate(VideoMetrics::captureFps(), fps);
                        capCount = 0;
                        windowStartUs = metadata.systemRelativeTimeUs;
                    }
                }

                // 2. Content size change detection (moved from callback)
                try {
                    static std::atomic<int> lastLoggedW{ 0 };
                    static std::atomic<int> lastLoggedH{ 0 };
                    if (metadata.contentSize.Width != lastLoggedW.load() || metadata.contentSize.Height != lastLoggedH.load()) {
                        std::wcout << L"[WGC] ContentSize changed: " << metadata.contentSize.Width << L"x" << metadata.contentSize.Height << std::endl;
                        lastLoggedW.store(metadata.contentSize.Width);
                        lastLoggedH.store(metadata.contentSize.Height);
                    }
                } catch (...) {}

                // 3. Unchanged frame skipping (moved from callback)
                if (metadata.shouldSkipUnchanged) {
                    try {
                        static int64_t lastSrtUs = 0;
                        if (lastSrtUs != 0) {
                            // If extremely small delta (< 1000us), likely no new content; skip aggressively
                            if (metadata.systemRelativeTimeUs - lastSrtUs <= 1000) {
                                lastSrtUs = metadata.systemRelativeTimeUs;
                                continue; // Skip this frame entirely
                            }
                        }
                        lastSrtUs = metadata.systemRelativeTimeUs;
                    } catch (...) {}
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
                VideoMetrics::queueDepth().store(static_cast<uint64_t>(qsz), std::memory_order_relaxed);
                uint64_t oo = g_outOfOrder.load(std::memory_order_relaxed);
                // std::wcout << L"[Stats] Encode=" << submitCount
                //            << L"/s, Target=" << g_targetFps.load()
                //            << L" fps, QueueDepth=" << qsz
                //            << L", OverwriteDrops/s=" << (od - lastOverwriteDrops)
                //            << L", BPSkips/s=" << (bp - lastBpSkips)
                //            << L", OutOfOrder/s=" << (oo - lastOutOfOrder)
                //            << std::endl;
                lastOverwriteDrops = od;
                lastBpSkips = bp;
                lastOutOfOrder = oo;
                submitCount = 0;
                lastLog = nowDbg;
            }
        }
        if (mmcssHandle) {
            AvRevertMmThreadCharacteristics(mmcssHandle);
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

        // Clear metadata ring
        g_metadataRing.clear();
        g_metadataCapacity = 0;
        g_metadataHead.store(0, std::memory_order_relaxed);
        g_metadataTail.store(0, std::memory_order_relaxed);
    }
    // Removed unused encode queue cleanup

    framePool.FrameArrived(token);
    framePool.Close();
    Encoder::FinalizeEncoder();
}