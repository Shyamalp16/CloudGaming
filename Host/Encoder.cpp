#include "Encoder.h"
#include "GlobalTime.h"
#include "pion_webrtc.h"
#include <Windows.h>
#include <functional>
#include <mutex>
#include <condition_variable>
#include <fstream>
#include <chrono>
#include "PacketQueue.h"
#include "D3DHelpers.h" // For GetGpuVendorId
#include <libavutil/hwcontext_d3d11va.h>
#include <libavutil/avutil.h>  // AV_NOPTS_VALUE
#include <d3d11.h>
#include <wrl.h>
#include <unordered_map>
#include "EtwMarkers.h"
#include "VideoMetrics.h"
#include <thread>
#include <deque>
#include <cstring>
#include <list>
#include <cmath>  // For std::abs in UpdatePacingFromTimestamp
#include <avrt.h>
#pragma comment(lib, "Avrt.lib")
#include "AdaptiveQualityControl.h"
#include "ThreadPriorityManager.h"

// Removed unused software conversion components
int Encoder::currentWidth = 0;
int Encoder::currentHeight = 0;

std::mutex Encoder::g_encoderMutex;

// Static variables for encoder state
AVFormatContext* Encoder::formatCtx = nullptr;
AVCodecContext* Encoder::codecCtx = nullptr;
AVStream* Encoder::videoStream = nullptr;
AVPacket* Encoder::packet = nullptr;
AVBufferRef* Encoder::hwDeviceCtx = nullptr;
AVBufferRef* Encoder::hwFramesCtx = nullptr;
int Encoder::frameCounter = 0;
int64_t Encoder::last_dts = 0;

Encoder::EncodedFrameCallback Encoder::g_onEncodedFrameCallback = nullptr;

std::mutex Encoder::g_frameMutex;
std::condition_variable Encoder::g_frameAvailable;
std::vector<uint8_t> Encoder::g_latestFrameData;
int64_t Encoder::g_latestPTS = 0;
bool Encoder::g_frameReady = false;

static bool g_shutdown = false;

static std::chrono::steady_clock::time_point encoderStartTime;
static bool isFirstFrame = true;

// D3D11 VideoProcessor resources for GPU BGRA -> NV12 conversion
static Microsoft::WRL::ComPtr<ID3D11VideoDevice> g_videoDevice;
static Microsoft::WRL::ComPtr<ID3D11VideoContext> g_videoContext;
static Microsoft::WRL::ComPtr<ID3D11VideoProcessorEnumerator> g_vpEnumerator;
static Microsoft::WRL::ComPtr<ID3D11VideoProcessor> g_videoProcessor;
static int g_vpWidth = 0;
static int g_vpHeight = 0;
// LRU caches for D3D11 views to avoid per-frame allocations and prevent wholesale clears
template<typename K, typename V>
class LruCacheD3D {
public:
    explicit LruCacheD3D(size_t cap = 0) : capacity_(cap) {}
    void setCapacity(size_t cap) {
        capacity_ = cap;
        evictIfNeeded();
    }
    void clear() {
        items_.clear();
        map_.clear();
    }
    bool get(const K& key, V& out) {
        auto it = map_.find(key);
        if (it == map_.end()) return false;
        items_.splice(items_.begin(), items_, it->second);
        out = it->second->second;
        return true;
    }
    void put(const K& key, const V& val) {
        auto it = map_.find(key);
        if (it != map_.end()) {
            it->second->second = val;
            items_.splice(items_.begin(), items_, it->second);
            return;
        }
        items_.emplace_front(key, val);
        map_[key] = items_.begin();
        evictIfNeeded();
    }
    size_t size() const { return map_.size(); }
private:
    void evictIfNeeded() {
        if (capacity_ == 0) return;
        while (map_.size() > capacity_) {
            auto last = items_.end();
            --last;
            map_.erase(last->first);
            items_.pop_back();
        }
    }
    size_t capacity_;
    std::list<std::pair<K, V>> items_;
    std::unordered_map<K, typename std::list<std::pair<K, V>>::iterator> map_;
};

// framePoolBuffers=2 means at most 2-3 live BGRA source textures from WGC.
// A capacity-8 LRU fits them all in L1/L2 cache (vs 64-entry unordered_map
// that pointer-chases into random heap locations on every frame).
static LruCacheD3D<ID3D11Texture2D*, Microsoft::WRL::ComPtr<ID3D11VideoProcessorInputView>> g_inputViewLru(8);
static LruCacheD3D<ID3D11Texture2D*, Microsoft::WRL::ComPtr<ID3D11VideoProcessorOutputView>> g_outputViewLru(8);
// Optional GPU timestamp queries for VideoProcessorBlt
// WARNING: Keep disabled in production - adds GPU overhead and can cause stalls
static bool g_gpuTimingEnabled = false;
static Microsoft::WRL::ComPtr<ID3D11Query> g_tsDisjoint;
static Microsoft::WRL::ComPtr<ID3D11Query> g_tsStart;
static Microsoft::WRL::ComPtr<ID3D11Query> g_tsEnd;

// HDR tone mapping configuration
static bool g_hdrToneMappingEnabled = false;
static std::string g_hdrToneMappingMethod = "reinhard";
static float g_hdrExposure = 0.0f;
static float g_hdrGamma = 2.2f;
static float g_hdrSaturation = 1.0f;
static bool g_deferredContextEnabled = false;
static Microsoft::WRL::ComPtr<ID3D11DeviceContext> g_deferredContext;
static Microsoft::WRL::ComPtr<ID3D11CommandList> g_commandList;

// VideoProcessor format validation cache
static bool g_skipFormatChecks = false; // Skip format checks after initial validation
static std::unordered_map<DXGI_FORMAT, bool> g_inputFormatCache;
static std::unordered_map<DXGI_FORMAT, bool> g_outputFormatCache;

// Hardware frame ring
static std::vector<AVFrame*> g_hwFrames;
static int g_hwFrameIndex = 0;
static int g_hwFramePoolSize = 4; // default to NVENC surfaces; >= async_depth

// Encoder runtime configuration (overridable from host config)
static int g_startBitrateBps = 20000000; // 20 Mbps default
static int g_minBitrateBps = 10000000;   // 10 Mbps default
static int g_maxBitrateBps = 50000000;   // 50 Mbps default
static int g_minBitrateController = 10000000;
static int g_maxBitrateController = 50000000;
static int g_increaseStep = 5000000;         // +5 Mbps
static int g_decreaseCooldownMs = 5000;      // ms — 5 s minimum between reductions to prevent oscillation
static int g_cleanSamplesRequired = 3;
static int g_increaseIntervalMs = 1000;      // ms
static int g_currentBitrate = 25000000;      // start ~25 Mbps
static int g_cleanSamples = 0;
static int g_congestionCeiling = 0;          // remembers bitrate that caused loss, 0 = no ceiling
static std::chrono::steady_clock::time_point g_lastChange = std::chrono::steady_clock::now();
static std::chrono::steady_clock::time_point g_ceilingSetTime = std::chrono::steady_clock::now();
static std::atomic<int> g_eagainCount{0};
static std::chrono::steady_clock::time_point g_lastEagain = std::chrono::steady_clock::now();
static bool g_fullRangeColor = false; // default to limited (TV) range
// NVENC configurable options
// Pacing config
static std::atomic<int> g_pacingFps{0};
static std::atomic<int> g_pacingFixedUs{0};
// PLI policy
static std::atomic<bool> g_ignorePli{false};
static std::atomic<int> g_minPliIntervalMs{500};
static std::atomic<double> g_minPliLossThreshold{0.03};
static std::chrono::steady_clock::time_point g_lastPliTime = std::chrono::steady_clock::now() - std::chrono::seconds(10);
static std::string g_nvPreset = "p5"; // faster low-latency default
static std::string g_nvRc = "cbr";
static int g_nvBf = 0;
static int g_nvRcLookahead = 0;
static int g_nvAsyncDepth = 2;
static int g_nvSurfaces = 3; // Optimized: async_depth + 1 for minimal buffering
// Pacing from capture timestamps (EWMA of inter-frame delta)
static std::atomic<long long> g_lastCaptureTsUs{0};
static std::atomic<long long> g_smoothedDurUs{0};

// Encoded sample sender queue to avoid holding encoder mutex during FFI send
struct QueuedSample {
    std::vector<uint8_t> data;
    int64_t durationUs;
};
static std::mutex g_sendMutex;
static std::condition_variable g_sendCV;
static std::deque<QueuedSample> g_sendQueue;
static std::atomic<bool> g_senderRunning{false};
static std::thread g_senderThread;
static constexpr size_t kMaxSendQueue = 2; // 2 frames max = ~22ms at 90fps; oldest dropped on overflow

static void SenderLoop() {
    // Elevate sender thread to MMCSS 'Playback' to avoid starving the game
    DWORD taskIndex = 0;
    HANDLE mmcssTask = AvSetMmThreadCharacteristicsW(L"Playback", &taskIndex);
    if (mmcssTask) {
        AvSetMmThreadPriority(mmcssTask, AVRT_PRIORITY_HIGH);
    }
    // Simple 1Hz logging
    int sentCount = 0;
    auto lastLog = std::chrono::steady_clock::now();
    while (g_senderRunning.load()) {
        QueuedSample sample;
        {
            std::unique_lock<std::mutex> lk(g_sendMutex);
            g_sendCV.wait(lk, []{ return !g_sendQueue.empty() || !g_senderRunning.load(); });
            if (!g_senderRunning.load() && g_sendQueue.empty()) break;
            if (g_sendQueue.empty()) continue;
            sample = std::move(g_sendQueue.front());
            g_sendQueue.pop_front();
        }
        if (!sample.data.empty()) {
            auto ffiStart = std::chrono::steady_clock::now();
            int result = sendVideoSample(sample.data.data(), static_cast<int>(sample.data.size()), sample.durationUs);
            auto ffiEnd = std::chrono::steady_clock::now();
            double ffiMs = std::chrono::duration<double, std::milli>(ffiEnd - ffiStart).count();
            VideoMetrics::ewmaUpdate(VideoMetrics::ffiSendLatencyMsAvg(), ffiMs);
            (void)result; // errors are logged inside sendVideoSample caller historically
            sentCount++;
        }
        auto now = std::chrono::steady_clock::now();
        if (std::chrono::duration_cast<std::chrono::seconds>(now - lastLog).count() >= 1) {
            // Keep stdout fairly quiet; rely on existing logs elsewhere
            sentCount = 0;
            lastLog = now;
        }
    }
}

static void StartSenderThreadIfNeeded() {
    bool expected = false;
    if (g_senderRunning.compare_exchange_strong(expected, true)) {
        g_senderThread = std::thread(SenderLoop);
    }
}

static void StopSenderThread() {
    bool expected = true;
    if (g_senderRunning.compare_exchange_strong(expected, false)) {
        {
            std::lock_guard<std::mutex> lk(g_sendMutex);
        }
        g_sendCV.notify_all();
        if (g_senderThread.joinable()) g_senderThread.join();
        // Drain any remaining queued samples
        {
            std::lock_guard<std::mutex> lk(g_sendMutex);
            g_sendQueue.clear();
        }
    }
}

static inline int64_t ComputeFrameDurationUsLocked() {
    int fpsNum = Encoder::codecCtx ? Encoder::codecCtx->framerate.num : 60;
    int fpsDen = Encoder::codecCtx ? Encoder::codecCtx->framerate.den : 1;
    int fixedUs = g_pacingFixedUs.load();
    int cfgFps = g_pacingFps.load();
    if (fixedUs > 0) return fixedUs;
    if (cfgFps > 0) return static_cast<int64_t>(1000000.0 / static_cast<double>(cfgFps));
    if (fpsNum > 0) return static_cast<int64_t>((1000000.0 * (fpsDen > 0 ? fpsDen : 1)) / static_cast<double>(fpsNum));
    return 8333; // ~120fps fallback
}

static inline int64_t UpdatePacingFromTimestamp(int64_t currentTsUs) {
    // Honor explicit fixed pacing if configured
    if (g_pacingFixedUs.load(std::memory_order_relaxed) > 0) {
        return g_pacingFixedUs.load(std::memory_order_relaxed);
    }
    if (currentTsUs <= 0) {
        long long sm = g_smoothedDurUs.load(std::memory_order_relaxed);
        return sm > 0 ? sm : ComputeFrameDurationUsLocked();
    }
    long long prevTs = g_lastCaptureTsUs.exchange(currentTsUs, std::memory_order_relaxed);
    if (prevTs > 0) {
        long long delta = currentTsUs - prevTs;
        // FIX: Stricter bounds checking (500us = 2000fps max, 100ms = 10fps min)
        // This prevents invalid deltas from corrupting the smoothed duration
        if (delta > 500 && delta < 100000) {
            long long prevSm = g_smoothedDurUs.load(std::memory_order_relaxed);
            // FIX: Adaptive alpha - use more of new value if delta differs significantly from smoothed
            // This allows faster adaptation to FPS changes while maintaining stability
            double alpha = 0.2; // Default smoothing factor
            if (prevSm > 0) {
                double diffRatio = std::abs(static_cast<double>(delta - prevSm)) / static_cast<double>(prevSm);
                if (diffRatio > 0.1) {
                    // Delta differs by more than 10% from smoothed - use more aggressive smoothing
                    alpha = 0.3;
                }
            }
            long long newSm = (prevSm <= 0) ? delta : 
                static_cast<long long>((1.0 - alpha) * static_cast<double>(prevSm) + alpha * static_cast<double>(delta));
            g_smoothedDurUs.store(newSm, std::memory_order_relaxed);
            return newSm;
        }
    }
    long long sm = g_smoothedDurUs.load(std::memory_order_relaxed);
    return sm > 0 ? sm : ComputeFrameDurationUsLocked();
}

static inline void EnqueueEncodedSample(std::vector<uint8_t>&& bytes, int64_t durationUs) {
    if (bytes.empty()) return;

    // Check with adaptive quality controller before queuing
    auto qualityDecision = AdaptiveQualityControl::checkFrameDropping();

    if (qualityDecision.shouldDropFrame) {
        // Log the dropping decision for monitoring (temporarily more verbose for debugging)
        static int dropCounter = 0;
        if (++dropCounter % 10 == 0) {  // Log every 10th drop for debugging
            std::cout << "[AdaptiveQC] Frame dropped: " << qualityDecision.reason
                      << " (condition: " << static_cast<int>(qualityDecision.condition)
                      << ", ratio: " << qualityDecision.dropRatio
                      << ", total dropped: " << dropCounter << ")" << std::endl;
        }
        VideoMetrics::inc(VideoMetrics::sendQueueDrops());
        return; // Drop the frame without queuing
    }

    {
        std::lock_guard<std::mutex> lk(g_sendMutex);
        if (g_sendQueue.size() >= kMaxSendQueue) {
            // drop oldest to prevent growth and large latency spikes
            g_sendQueue.pop_front();
            VideoMetrics::inc(VideoMetrics::sendQueueDrops());
        }
        g_sendQueue.push_back(QueuedSample{ std::move(bytes), durationUs });
        VideoMetrics::sendQueueDepth().store(static_cast<uint64_t>(g_sendQueue.size()), std::memory_order_relaxed);
    }
    g_sendCV.notify_one();
}

// Per-packet sample: used to enqueue each AVPacket as its own WebRTC sample (avoids merging
// multiple frames into one sample, which causes decode corruption and jitter-buffer confusion).
struct PendingSample {
    std::vector<uint8_t> data;
    int64_t durationUs;
};

static PendingSample MakePendingSampleFromPacket(AVPacket* pkt, AVCodecContext* ctx, int64_t fallbackTsUs) {
    PendingSample out;
    if (!pkt || !pkt->data || pkt->size <= 0) return out;
    out.data.assign(pkt->data, pkt->data + pkt->size);
    int64_t pktTsUs = fallbackTsUs;
    if (pkt->pts != AV_NOPTS_VALUE && pkt->pts >= 0 && ctx) {
        pktTsUs = av_rescale_q(pkt->pts, ctx->time_base, AVRational{ 1, 1000000 });
    }
    out.durationUs = UpdatePacingFromTimestamp(pktTsUs);
    if (out.durationUs <= 0) out.durationUs = ComputeFrameDurationUsLocked();
    if (out.durationUs <= 0) out.durationUs = 8333;
    return out;
}

// Pre-validate common texture formats to cache results and avoid per-frame checks
static void PreValidateFormats() {
    if (!g_vpEnumerator) return;

    // Common input formats for desktop capture
    std::vector<DXGI_FORMAT> inputFormats = {
        DXGI_FORMAT_B8G8R8A8_UNORM,  // BGRA - most common for desktop capture
        DXGI_FORMAT_B8G8R8X8_UNORM,  // BGRX
        DXGI_FORMAT_R8G8B8A8_UNORM   // RGBA
    };

    // Common output formats
    std::vector<DXGI_FORMAT> outputFormats = {
        DXGI_FORMAT_NV12,            // NV12 - most common for encoding
        DXGI_FORMAT_P010             // P010 for HDR (if supported)
    };

    // Validate and cache input formats
    for (DXGI_FORMAT format : inputFormats) {
        UINT support = 0;
        HRESULT hr = g_vpEnumerator->CheckVideoProcessorFormat(format, &support);
        bool isSupported = SUCCEEDED(hr) && (support & D3D11_VIDEO_PROCESSOR_FORMAT_SUPPORT_INPUT);
        g_inputFormatCache[format] = isSupported;
    }

    // Validate and cache output formats
    for (DXGI_FORMAT format : outputFormats) {
        UINT support = 0;
        HRESULT hr = g_vpEnumerator->CheckVideoProcessorFormat(format, &support);
        bool isSupported = SUCCEEDED(hr) && (support & D3D11_VIDEO_PROCESSOR_FORMAT_SUPPORT_OUTPUT);
        g_outputFormatCache[format] = isSupported;
    }

    // Enable skipping format checks after successful validation
    g_skipFormatChecks = true;

    std::wcout << L"[Encoder][VP] Pre-validated " << inputFormats.size() << L" input and "
               << outputFormats.size() << L" output formats for optimized performance" << std::endl;
}

// Prime common texture views to avoid creation overhead on first frames
static void PrimeCommonViews(ID3D11Device* device, int width, int height) {
    if (!g_videoDevice || !g_videoContext || !g_vpEnumerator) return;

    // Create a dummy BGRA texture for input view priming
    D3D11_TEXTURE2D_DESC texDesc = {};
    texDesc.Width = width;
    texDesc.Height = height;
    texDesc.MipLevels = 1;
    texDesc.ArraySize = 1;
    texDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    texDesc.SampleDesc.Count = 1;
    texDesc.Usage = D3D11_USAGE_DEFAULT;
    texDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

    Microsoft::WRL::ComPtr<ID3D11Texture2D> dummyInputTex;
    if (SUCCEEDED(device->CreateTexture2D(&texDesc, nullptr, dummyInputTex.GetAddressOf()))) {
        // Prime input view
        D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC inDesc{};
        inDesc.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
        inDesc.Texture2D.MipSlice = 0;
        inDesc.Texture2D.ArraySlice = 0;

        Microsoft::WRL::ComPtr<ID3D11VideoProcessorInputView> inView;
        if (SUCCEEDED(g_videoDevice->CreateVideoProcessorInputView(dummyInputTex.Get(), g_vpEnumerator.Get(), &inDesc, inView.GetAddressOf()))) {
            g_inputViewLru.put(dummyInputTex.Get(), inView);
        }
    }

    // Create a dummy NV12 texture for output view priming
    texDesc.Format = DXGI_FORMAT_NV12;
    texDesc.BindFlags = D3D11_BIND_RENDER_TARGET;

    Microsoft::WRL::ComPtr<ID3D11Texture2D> dummyOutputTex;
    if (SUCCEEDED(device->CreateTexture2D(&texDesc, nullptr, dummyOutputTex.GetAddressOf()))) {
        // Prime output view
        D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC outDesc{};
        outDesc.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
        outDesc.Texture2D.MipSlice = 0;

        Microsoft::WRL::ComPtr<ID3D11VideoProcessorOutputView> outView;
        if (SUCCEEDED(g_videoDevice->CreateVideoProcessorOutputView(dummyOutputTex.Get(), g_vpEnumerator.Get(), &outDesc, outView.GetAddressOf()))) {
            g_outputViewLru.put(dummyOutputTex.Get(), outView);
        }
    }

    std::wcout << L"[Encoder][VP] Primed common texture views for reduced initialization overhead" << std::endl;
}

static bool InitializeVideoProcessor(ID3D11Device* device, int width, int height)
{
    g_videoDevice.Reset();
    g_videoContext.Reset();
    g_vpEnumerator.Reset();
    g_videoProcessor.Reset();
    g_vpWidth = width;
    g_vpHeight = height;

    Microsoft::WRL::ComPtr<ID3D11DeviceContext> dc;
    device->GetImmediateContext(dc.GetAddressOf());
    if (FAILED(device->QueryInterface(__uuidof(ID3D11VideoDevice), (void**)g_videoDevice.GetAddressOf()))) {
        std::cerr << "[Encoder][VP] ID3D11VideoDevice QI failed" << std::endl;
        return false;
    }
    if (FAILED(dc->QueryInterface(__uuidof(ID3D11VideoContext), (void**)g_videoContext.GetAddressOf()))) {
        std::cerr << "[Encoder][VP] ID3D11VideoContext QI failed" << std::endl;
        return false;
    }

    D3D11_VIDEO_PROCESSOR_CONTENT_DESC desc{};
    desc.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
    desc.InputWidth = width;
    desc.InputHeight = height;
    desc.OutputWidth = width;
    desc.OutputHeight = height;
    desc.Usage = D3D11_VIDEO_USAGE_OPTIMAL_SPEED;
    if (FAILED(g_videoDevice->CreateVideoProcessorEnumerator(&desc, g_vpEnumerator.GetAddressOf()))) {
        std::cerr << "[Encoder][VP] CreateVideoProcessorEnumerator failed" << std::endl;
        return false;
    }
    if (FAILED(g_videoDevice->CreateVideoProcessor(g_vpEnumerator.Get(), 0, g_videoProcessor.GetAddressOf()))) {
        std::cerr << "[Encoder][VP] CreateVideoProcessor failed" << std::endl;
        return false;
    }

    // Explicitly set BT.709 color space so the VideoProcessor uses the same
    // matrix we signal in the H.264 SPS. Without this, drivers default to BT.601
    // (SD matrix), which skews red/luma and produces a warmer, slightly off color.
    //
    // Input: full-range sRGB BGRA from WGC (RGB_Range=0, Nominal_Range=0-255)
    // Output: BT.709 YCbCr in limited or full range matching g_fullRangeColor
    {
        D3D11_VIDEO_PROCESSOR_COLOR_SPACE inputCS{};
        inputCS.Usage        = 0;  // playback
        inputCS.RGB_Range    = 0;  // full-range RGB (0-255); WGC delivers uncompressed BGRA
        inputCS.YCbCr_Matrix = 1;  // BT.709 (1=HD), not BT.601 (0=SD)
        inputCS.YCbCr_xvYCC  = 0;
        inputCS.Nominal_Range = 2; // D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_0_255 (full-range input)
        g_videoContext->VideoProcessorSetStreamColorSpace(g_videoProcessor.Get(), 0, &inputCS);

        D3D11_VIDEO_PROCESSOR_COLOR_SPACE outputCS{};
        outputCS.Usage        = 0;
        outputCS.RGB_Range    = 0;
        outputCS.YCbCr_Matrix = 1;  // BT.709
        outputCS.YCbCr_xvYCC  = 0;
        // Nominal_Range: 1 = 16-235 limited (TV), 2 = 0-255 full (PC)
        outputCS.Nominal_Range = g_fullRangeColor ? 2 : 1;
        g_videoContext->VideoProcessorSetOutputColorSpace(g_videoProcessor.Get(), &outputCS);

        std::wcout << L"[Encoder][VP] Color space set: BT.709, "
                   << (g_fullRangeColor ? L"full range (0-255)" : L"limited range (16-235)") << std::endl;
    }

    // Pre-validate common formats to avoid per-frame checks
    PreValidateFormats();

    // Prime common texture views to avoid creation overhead on first frames
    PrimeCommonViews(device, width, height);

    std::wcout << L"[Encoder][VP] Initialized VideoProcessor for " << width << L"x" << height << std::endl;
    return true;
}



// Check if a format is supported using cached results
static bool IsInputFormatSupported(DXGI_FORMAT format) {
    if (g_skipFormatChecks) {
        auto it = g_inputFormatCache.find(format);
        if (it != g_inputFormatCache.end()) {
            return it->second;
        }
    }

    // Fallback to live check if not cached
    UINT support = 0;
    HRESULT hr = g_vpEnumerator->CheckVideoProcessorFormat(format, &support);
    bool isSupported = SUCCEEDED(hr) && (support & D3D11_VIDEO_PROCESSOR_FORMAT_SUPPORT_INPUT);
    g_inputFormatCache[format] = isSupported; // Cache for future use
    return isSupported;
}

// Check if a format is supported using cached results
static bool IsOutputFormatSupported(DXGI_FORMAT format) {
    if (g_skipFormatChecks) {
        auto it = g_outputFormatCache.find(format);
        if (it != g_outputFormatCache.end()) {
            return it->second;
        }
    }

    // Fallback to live check if not cached
    UINT support = 0;
    HRESULT hr = g_vpEnumerator->CheckVideoProcessorFormat(format, &support);
    bool isSupported = SUCCEEDED(hr) && (support & D3D11_VIDEO_PROCESSOR_FORMAT_SUPPORT_OUTPUT);
    g_outputFormatCache[format] = isSupported; // Cache for future use
    return isSupported;
}

namespace Encoder {
    void SetGpuTimingEnabled(bool enable) {
        std::lock_guard<std::mutex> lock(g_encoderMutex);
        g_gpuTimingEnabled = enable;
    }
    void SetDeferredContextEnabled(bool enable) {
        std::lock_guard<std::mutex> lock(g_encoderMutex);
        g_deferredContextEnabled = enable;
        if (!enable) {
            g_deferredContext.Reset();
            g_commandList.Reset();
        }
    }

    void SetHdrToneMappingConfig(bool enabled, const std::string& method, float exposure, float gamma, float saturation) {
        g_hdrToneMappingEnabled = enabled;
        g_hdrToneMappingMethod = method;
        g_hdrExposure = exposure;
        g_hdrGamma = gamma;
        g_hdrSaturation = saturation;
        
        if (enabled) {
            std::wcout << L"[Encoder] HDR tone mapping enabled - Method: " << method.c_str() 
                      << L", Exposure: " << exposure << L", Gamma: " << gamma 
                      << L", Saturation: " << saturation << std::endl;
        }
    }

    // Helper function for linear interpolation
    float lerp(float a, float b, float t) {
        return a + t * (b - a);
    }

    // HDR tone mapping function using Reinhard tone mapping
    // WARNING: CPU-side pixel processing. At 1920×1080 this loops over 2,073,600
    // pixels per call. At 120 fps that is ~248 million pixel ops/sec on a single
    // CPU core — will saturate the core and cause massive frame drops. This path
    // is only safe for offline/single-shot use. For real-time tone mapping use a
    // GPU compute shader or the VideoProcessor's built-in tonemapping pipeline.
    void ApplyHdrToneMapping(ID3D11Texture2D* texture, ID3D11DeviceContext* context) {
        if (!g_hdrToneMappingEnabled || !texture || !context) return;

        // Map the texture for CPU access
        D3D11_MAPPED_SUBRESOURCE mapped;
        HRESULT hr = context->Map(texture, 0, D3D11_MAP_READ_WRITE, 0, &mapped);
        if (FAILED(hr)) return;

        // Get texture description
        D3D11_TEXTURE2D_DESC desc;
        texture->GetDesc(&desc);
        
        // Process BGRA pixels (assuming 8-bit per channel)
        if (desc.Format == DXGI_FORMAT_B8G8R8A8_UNORM) {
            uint8_t* pixels = static_cast<uint8_t*>(mapped.pData);
            int pixelCount = desc.Width * desc.Height;
            
            for (int i = 0; i < pixelCount; ++i) {
                int pixelOffset = i * 4; // BGRA format
                
                // Convert to float [0,1]
                float b = pixels[pixelOffset + 0] / 255.0f;
                float g = pixels[pixelOffset + 1] / 255.0f;
                float r = pixels[pixelOffset + 2] / 255.0f;
                float a = pixels[pixelOffset + 3] / 255.0f;
                
                // Apply exposure adjustment
                float exposureMultiplier = powf(2.0f, g_hdrExposure);
                r *= exposureMultiplier;
                g *= exposureMultiplier;
                b *= exposureMultiplier;
                
                        // FIX: Only apply Reinhard tone mapping if values exceed SDR range (> 1.0)
                // This prevents crushing blacks on non-HDR content
                // Reinhard tone mapping: x / (1 + x) - only for values > 1.0
                if (r > 1.0f) r = r / (1.0f + r);
                if (g > 1.0f) g = g / (1.0f + g);
                if (b > 1.0f) b = b / (1.0f + b);
                
                // Apply gamma correction
                r = powf(r, 1.0f / g_hdrGamma);
                g = powf(g, 1.0f / g_hdrGamma);
                b = powf(b, 1.0f / g_hdrGamma);
                
                // Apply saturation adjustment
                float luminance = 0.299f * r + 0.587f * g + 0.114f * b;
                r = lerp(luminance, r, g_hdrSaturation);
                g = lerp(luminance, g, g_hdrSaturation);
                b = lerp(luminance, b, g_hdrSaturation);
                
                // Clamp to [0,1] and convert back to uint8
                r = std::max(0.0f, std::min(1.0f, r));
                g = std::max(0.0f, std::min(1.0f, g));
                b = std::max(0.0f, std::min(1.0f, b));
                
                pixels[pixelOffset + 0] = static_cast<uint8_t>(b * 255.0f);
                pixels[pixelOffset + 1] = static_cast<uint8_t>(g * 255.0f);
                pixels[pixelOffset + 2] = static_cast<uint8_t>(r * 255.0f);
                // Alpha channel unchanged
            }
        }
        
        context->Unmap(texture, 0);
    }

    bool AcquireHwInputSurface(int &slotIndexOut, ID3D11Texture2D** nv12TextureOut) {
        std::lock_guard<std::mutex> lock(g_encoderMutex);
        if (!codecCtx || g_hwFrames.empty()) return false;
        slotIndexOut = g_hwFrameIndex;
        g_hwFrameIndex = (g_hwFrameIndex + 1) % static_cast<int>(g_hwFrames.size());
        AVFrame* hw = g_hwFrames[slotIndexOut];
        *nv12TextureOut = (ID3D11Texture2D*)hw->data[0];
        return (*nv12TextureOut != nullptr);
    }

    bool VideoProcessorBltToSlot(ID3D11Texture2D* bgraSrcTexture, int slotIndex) {
        // Apply HDR tone mapping if enabled (before video processing)
        if (g_hdrToneMappingEnabled) {
            Microsoft::WRL::ComPtr<ID3D11DeviceContext> context;
            ((ID3D11Device*)GetD3DDevice().get())->GetImmediateContext(context.GetAddressOf());
            ApplyHdrToneMapping(bgraSrcTexture, context.Get());
        }

        // Core GPU operation under mutex (keep this minimal)
        HRESULT bltHr = E_FAIL;
        {
            std::lock_guard<std::mutex> lock(g_encoderMutex);
            if (!codecCtx || !g_videoProcessor || slotIndex < 0 || slotIndex >= (int)g_hwFrames.size()) return false;
            AVFrame* hw = g_hwFrames[slotIndex];
            ID3D11Texture2D* nv12 = (ID3D11Texture2D*)hw->data[0];

            // Create/reuse views (fast LRU operations)
            Microsoft::WRL::ComPtr<ID3D11VideoProcessorInputView> inView;
            if (!g_inputViewLru.get(bgraSrcTexture, inView)) {
                D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC inDesc{};
                inDesc.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
                inDesc.Texture2D.MipSlice = 0; inDesc.Texture2D.ArraySlice = 0;
                if (FAILED(g_videoDevice->CreateVideoProcessorInputView(bgraSrcTexture, g_vpEnumerator.Get(), &inDesc, inView.GetAddressOf()))) return false;
                g_inputViewLru.put(bgraSrcTexture, inView);
            }
            Microsoft::WRL::ComPtr<ID3D11VideoProcessorOutputView> outView;
            if (!g_outputViewLru.get(nv12, outView)) {
                D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC outDesc{};
                outDesc.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D; outDesc.Texture2D.MipSlice = 0;
                if (FAILED(g_videoDevice->CreateVideoProcessorOutputView(nv12, g_vpEnumerator.Get(), &outDesc, outView.GetAddressOf()))) return false;
                g_outputViewLru.put(nv12, outView);
            }
            D3D11_VIDEO_PROCESSOR_STREAM stream{}; stream.Enable = TRUE; stream.pInputSurface = inView.Get();

            bltHr = g_videoContext->VideoProcessorBlt(g_videoProcessor.Get(), outView.Get(), 0, 1, &stream);
        }

        // GPU timing queries OUTSIDE mutex (potentially blocking operations)
        if (g_gpuTimingEnabled && SUCCEEDED(bltHr)) {
            // Lazy-create timestamp queries on first use (outside mutex)
            if (!g_tsDisjoint) {
                std::lock_guard<std::mutex> lock(g_encoderMutex); // Brief lock just for query creation
                D3D11_QUERY_DESC qd{}; qd.Query = D3D11_QUERY_TIMESTAMP_DISJOINT; qd.MiscFlags = 0;
                ((ID3D11Device*)GetD3DDevice().get())->CreateQuery(&qd, g_tsDisjoint.GetAddressOf());
            }
            if (!g_tsStart) {
                std::lock_guard<std::mutex> lock(g_encoderMutex); // Brief lock just for query creation
                D3D11_QUERY_DESC q{}; q.Query = D3D11_QUERY_TIMESTAMP; q.MiscFlags = 0;
                ((ID3D11Device*)GetD3DDevice().get())->CreateQuery(&q, g_tsStart.GetAddressOf());
            }
            if (!g_tsEnd) {
                std::lock_guard<std::mutex> lock(g_encoderMutex); // Brief lock just for query creation
                D3D11_QUERY_DESC q{}; q.Query = D3D11_QUERY_TIMESTAMP; q.MiscFlags = 0;
                ((ID3D11Device*)GetD3DDevice().get())->CreateQuery(&q, g_tsEnd.GetAddressOf());
            }

            // GPU timing operations (outside main mutex)
            Microsoft::WRL::ComPtr<ID3D11DeviceContext> immediate;
            ((ID3D11Device*)GetD3DDevice().get())->GetImmediateContext(immediate.GetAddressOf());

            immediate->Begin(g_tsDisjoint.Get());
            immediate->End(g_tsStart.Get());
            // BLT already done above
            immediate->End(g_tsEnd.Get());
            immediate->End(g_tsDisjoint.Get());

            // Readback timing (non-blocking try; if not ready, skip logging) - OUTSIDE MUTEX
            D3D11_QUERY_DATA_TIMESTAMP_DISJOINT disjoint{};
            if (SUCCEEDED(immediate->GetData(g_tsDisjoint.Get(), &disjoint, sizeof(disjoint), 0)) && !disjoint.Disjoint) {
                UINT64 startTs = 0, endTs = 0;
                if (SUCCEEDED(immediate->GetData(g_tsStart.Get(), &startTs, sizeof(startTs), 0)) &&
                    SUCCEEDED(immediate->GetData(g_tsEnd.Get(), &endTs, sizeof(endTs), 0)) && endTs > startTs) {
                    double gpuMs = (double)(endTs - startTs) / (double)disjoint.Frequency * 1000.0;
                    VideoMetrics::vpGpuMs().store(gpuMs, std::memory_order_relaxed);
                    // Persist last observed value for external export
                    // Avoid an include cycle by not depending on VideoMetrics here
                    static auto lastLog = std::chrono::steady_clock::now();
                    auto now = std::chrono::steady_clock::now();
                    if (std::chrono::duration_cast<std::chrono::seconds>(now - lastLog).count() >= 1) {
                        // std::wcout << L"[VP] VideoProcessorBlt GPU time ~" << gpuMs << L" ms" << std::endl;
                        lastLog = now;
                    }
                }
            }
        }

        return SUCCEEDED(bltHr);
    }

    bool SubmitHwFrame(int slotIndex, int64_t timestampUs) {
        // Encode and drain under mutex; collect per-packet samples (do NOT merge multiple
        // frames into one WebRTC sample - causes decode corruption and jitter-buffer confusion).
        std::vector<PendingSample> pendingSamples;

        {
            std::lock_guard<std::mutex> lock(g_encoderMutex);
            if (!codecCtx || slotIndex < 0 || slotIndex >= (int)g_hwFrames.size()) return false;
            AVFrame* hw = g_hwFrames[slotIndex];
            hw->pts = av_rescale_q(timestampUs, {1, 1000000}, codecCtx->time_base);
            auto tSendStart = std::chrono::steady_clock::now();
            int ret = avcodec_send_frame(codecCtx, hw);
            if (ret == AVERROR(EAGAIN)) {
                g_eagainCount.fetch_add(1);
                g_lastEagain = std::chrono::steady_clock::now();
                VideoMetrics::inc(VideoMetrics::eagainEvents());

                // Log EAGAIN events for debugging (throttled)
                static auto lastEagainLog = std::chrono::steady_clock::now();
                auto now = std::chrono::steady_clock::now();
                auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - lastEagainLog);
                if (elapsed.count() >= 5000) { // Log at most every 5 seconds
                    int currentCount = g_eagainCount.load();
                    std::wcout << L"[Encoder] EAGAIN detected: encoder queue full, frame dropped. "
                              << L"Recent EAGAIN count: " << currentCount << std::endl;
                    lastEagainLog = now;
                }
                for (;;) {
                    int r = avcodec_receive_packet(codecCtx, packet);
                    if (r == AVERROR(EAGAIN) || r == AVERROR_EOF) break;
                    else if (r < 0) break;
                    auto sample = MakePendingSampleFromPacket(packet, codecCtx, timestampUs);
                    if (!sample.data.empty()) pendingSamples.push_back(std::move(sample));
                    av_packet_unref(packet);
                }
                ret = avcodec_send_frame(codecCtx, hw);
            }
            if (ret < 0) return false;
            for (;;) {
                int r = avcodec_receive_packet(codecCtx, packet);
                if (r == AVERROR(EAGAIN) || r == AVERROR_EOF) break;
                else if (r < 0) return false;
                auto sample = MakePendingSampleFromPacket(packet, codecCtx, timestampUs);
                if (!sample.data.empty()) pendingSamples.push_back(std::move(sample));
                av_packet_unref(packet);
            }
            auto tSendEnd = std::chrono::steady_clock::now();
            double ms = std::chrono::duration<double, std::milli>(tSendEnd - tSendStart).count();
            VideoMetrics::ewmaUpdate(VideoMetrics::avSendMsAvg(), ms);
        }
        if (!pendingSamples.empty()) {
            VideoMetrics::ewmaUpdate(VideoMetrics::packetsPerFrameAvg(), static_cast<double>(pendingSamples.size()));
        }
        for (auto& s : pendingSamples) {
            EnqueueEncodedSample(std::move(s.data), s.durationUs);
        }
        return true;
    }
    void SetPacingFps(int fps) {
        g_pacingFps.store(fps);
        if (fps > 0) g_pacingFixedUs.store(0);
    }
    void SetPacingFixedUs(int duration_us) {
        g_pacingFixedUs.store(duration_us);
        if (duration_us > 0) g_pacingFps.store(0);
    }

    void ConfigurePliPolicy(bool ignorePli, int minIntervalMs, double minLossThreshold) {
        g_ignorePli.store(ignorePli);
        if (minIntervalMs >= 0) g_minPliIntervalMs.store(minIntervalMs);
        if (minLossThreshold >= 0.0) g_minPliLossThreshold.store(minLossThreshold);
    }

    void SetNvencOptions(const char* preset,
                         const char* rc,
                         int bf,
                         int rc_lookahead,
                         int async_depth,
                         int surfaces) {
        if (preset && *preset) g_nvPreset = preset;
        if (rc && *rc) g_nvRc = rc;
        if (bf >= 0) g_nvBf = bf;
        if (rc_lookahead >= 0) g_nvRcLookahead = rc_lookahead;
        if (async_depth >= 0) g_nvAsyncDepth = async_depth;
        if (surfaces >= 1) {
            // FIX: Enforce optimal surfaces = async_depth + 1 (allow +2 for safety margin)
            // Excessive surfaces increase latency unnecessarily
            int optimalMax = g_nvAsyncDepth + 2;
            if (surfaces > optimalMax) {
                // FIX: Ensure decimal output (not hex) for numbers in log messages
                std::wcout << L"[Encoder] WARNING: Surfaces (" << std::dec << surfaces 
                          << L") exceeds optimal (async_depth + 2 = " << optimalMax 
                          << L"). Clamping to " << optimalMax << L" for low latency" << std::endl;
                g_nvSurfaces = optimalMax;
            } else {
                g_nvSurfaces = surfaces;
            }
        }
    }
    void SetFullRangeColor(bool enable_full_range) {
        g_fullRangeColor = enable_full_range;
    }
    void SetHwFramePoolSize(int pool_size) {
        // Keep within sensible bounds
        if (pool_size < 2) pool_size = 2;
        if (pool_size > 32) pool_size = 32;
        g_hwFramePoolSize = pool_size;
    }
    void SetBitrateConfig(int start_bps, int min_bps, int max_bps) {
        if (start_bps > 0) g_startBitrateBps = start_bps;
        if (min_bps > 0) g_minBitrateBps = min_bps;
        if (max_bps > 0) g_maxBitrateBps = max_bps;
        g_currentBitrate = g_startBitrateBps;
    }

    void ConfigureBitrateController(int min_bps,
                                    int max_bps,
                                    int increase_step_bps,
                                    int decrease_cooldown_ms,
                                    int clean_samples_required,
                                    int increase_interval_ms) {
        if (min_bps > 0) g_minBitrateController = min_bps;
        if (max_bps > 0) g_maxBitrateController = max_bps;
        if (increase_step_bps > 0) g_increaseStep = increase_step_bps;
        if (decrease_cooldown_ms > 0) g_decreaseCooldownMs = decrease_cooldown_ms;
        if (clean_samples_required > 0) g_cleanSamplesRequired = clean_samples_required;
        if (increase_interval_ms > 0) g_increaseIntervalMs = increase_interval_ms;
    }

    void OnRtcpFeedback(double packetLoss, double rtt, double /*jitter*/) {
        auto now = std::chrono::steady_clock::now();
        auto since = std::chrono::duration_cast<std::chrono::milliseconds>(now - g_lastChange).count();

        // FIX: Don't reduce bitrate on localhost (RTT < 5ms) or when packet loss is very low
        // This prevents unnecessary bitrate reduction during local testing
        bool isLocalhost = (rtt < 20.0); // WebRTC processing overhead can push RTT to 5-15ms even on localhost
        double effectiveLossThreshold = isLocalhost ? 0.25 : g_minPliLossThreshold.load(); // Higher threshold for localhost

        if (packetLoss >= effectiveLossThreshold) {
            if (since >= g_decreaseCooldownMs) {
                // Remember the bitrate that caused congestion (set ceiling at 90% of current)
                g_congestionCeiling = static_cast<int>(g_currentBitrate * 0.9);
                g_ceilingSetTime = now;

                double factor = isLocalhost ? 0.9 : ((packetLoss >= 0.10) ? 0.6 : 0.8);
                int target = static_cast<int>(g_currentBitrate * factor);
                g_currentBitrate = std::max(g_minBitrateController, target);
                AdjustBitrate(g_currentBitrate);
                g_lastChange = now;
            }
            g_cleanSamples = 0;
            return;
        }

        g_cleanSamples++;
        int requiredSamples = isLocalhost ? 1 : g_cleanSamplesRequired;
        int increaseInterval = isLocalhost ? 500 : g_increaseIntervalMs;

        // Slowly raise the congestion ceiling after 30s of stability
        auto ceilAge = std::chrono::duration_cast<std::chrono::seconds>(now - g_ceilingSetTime).count();
        if (g_congestionCeiling > 0 && ceilAge > 30) {
            g_congestionCeiling = 0;
        }

        if (since >= increaseInterval && g_cleanSamples >= requiredSamples) {
            int step = isLocalhost ? (g_increaseStep * 2) : g_increaseStep;
            int target = g_currentBitrate + step;
            int effectiveMax = g_maxBitrateController;

            // Don't exceed the congestion ceiling if one is set
            if (g_congestionCeiling > 0 && g_congestionCeiling < effectiveMax) {
                effectiveMax = g_congestionCeiling;
            }

            if (target <= effectiveMax) {
                g_currentBitrate = target;
                AdjustBitrate(g_currentBitrate);
                g_lastChange = now;
            }
            g_cleanSamples = 0;
        }
    }
    extern "C" void OnPLI() {
        if (g_ignorePli.load()) return;
        auto now = std::chrono::steady_clock::now();
        auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(now - g_lastPliTime).count();
        if (elapsedMs < g_minPliIntervalMs.load()) return;
        g_lastPliTime = now;
        RequestIDR();
    }

    void setEncodedFrameCallback(EncodedFrameCallback callback) {
        g_onEncodedFrameCallback = callback;
    }

    void SignalEncoderShutdown() {
        std::lock_guard<std::mutex> lock(g_frameMutex);
        g_shutdown = true;
        g_frameAvailable.notify_all(); // Wake up any waiting threads
    }

    bool getEncodedFrame(std::vector<uint8_t>& frameData, int64_t& pts) {
        std::unique_lock<std::mutex> lock(g_frameMutex);
        if (g_frameAvailable.wait_for(lock, std::chrono::milliseconds(100), [] { return g_frameReady || g_shutdown; })) {
            if (g_shutdown) {
                return false; // Exit if shutdown is signaled
            }

            if (g_latestFrameData.empty()) {
                return false;
            }
            frameData = g_latestFrameData;
            pts = g_latestPTS;
            g_frameReady = false;
            return true;
        }
        return false; // Timeout
    }

    void logNALUnits(const uint8_t* data, int size) {
        int pos = 0;
        while (pos < size) {
            if (pos + 3 >= size) break;
            if (data[pos] == 0x00 && data[pos + 1] == 0x00 && data[pos + 2] == 0x00 && data[pos + 3] == 0x01) {
                pos += 4;
                if (pos >= size) break;
                uint8_t nalUnitType = data[pos] & 0x1F;
                //std::wcout << L"[DEBUG] NAL Unit Type: " << (int)nalUnitType << L"\n";
                int nextPos = pos + 1;
                while (nextPos + 3 < size) {
                    if (data[nextPos] == 0x00 && data[nextPos + 1] == 0x00 && data[nextPos + 2] == 0x00 && data[nextPos + 3] == 0x01) {
                        break;
                    }
                    nextPos++;
                }
                int nalSize = (nextPos + 3 < size) ? (nextPos - pos + 3) : (size - pos);
                //std::wcout << L"[DEBUG] NAL Unit Size: " << nalSize << L"\n";
                pos = nextPos;
            }
            else {
                pos++;
            }
        }
    }

    void pushPacketToWebRTC(AVPacket* packet) {
        // Use packet->pts for duration when available (per-packet enqueue, no merging)
        auto sample = MakePendingSampleFromPacket(packet, codecCtx, 0);
        if (!sample.data.empty()) {
            ETW_MARK("Encoder_Send_Start");
            EnqueueEncodedSample(std::move(sample.data), sample.durationUs);
            ETW_MARK("Encoder_Send_End");
        }
    }

    void InitializeEncoder(const std::string& fileName, int width, int height, int fps) {
        std::lock_guard<std::mutex> lock(g_encoderMutex);

        // Clear caches when (re)initializing encoder
        g_inputViewLru.clear();
        g_outputViewLru.clear();
        // Free any existing hardware frame ring
        for (AVFrame* f : g_hwFrames) { if (f) av_frame_free(&f); }
        g_hwFrames.clear();
        g_hwFrameIndex = 0;

        if (codecCtx) avcodec_free_context(&codecCtx);
        if (hwFramesCtx) av_buffer_unref(&hwFramesCtx);
        if (hwDeviceCtx) av_buffer_unref(&hwDeviceCtx);
        if (formatCtx) {
            avformat_free_context(formatCtx);
            formatCtx = nullptr;
        }
        if (packet) av_packet_free(&packet);

        UINT vendorId = GetGpuVendorId();
        std::string encoderName;
        bool isHardware = true;
        AVHWDeviceType hwDeviceType;
        AVPixelFormat hwPixFmt;

        switch (vendorId) {
        case 0x10DE: // NVIDIA
            encoderName = "h264_nvenc";
            hwDeviceType = AV_HWDEVICE_TYPE_D3D11VA;
            hwPixFmt = AV_PIX_FMT_D3D11;
            break;
        case 0x8086: // Intel
            encoderName = "h264_qsv";
            hwDeviceType = AV_HWDEVICE_TYPE_D3D11VA;
            hwPixFmt = AV_PIX_FMT_D3D11;
            break;
        case 0x1002: // AMD
            encoderName = "h264_amf";
            hwDeviceType = AV_HWDEVICE_TYPE_D3D11VA;
            hwPixFmt = AV_PIX_FMT_D3D11;
            break;
        default:
            encoderName = "libx264";
            isHardware = false;
            break;
        }

        std::wcout << L"[Encoder] Using " << (isHardware ? L"Hardware" : L"Software") << L" encoder: " << std::wstring(encoderName.begin(), encoderName.end()) << std::endl;
        std::wcout << L"[Encoder] Initializing with width=" << width << L", height=" << height << L", fps=" << fps << std::endl;

        const AVCodec* codec = avcodec_find_encoder_by_name(encoderName.c_str());
        if (!codec) {
            std::cerr << "[Encoder] Failed to find encoder: " << encoderName << std::endl;
            return;
        }

        codecCtx = avcodec_alloc_context3(codec);
        if (!codecCtx) {
            std::cerr << "[Encoder] Failed to allocate codec context." << std::endl;
            return;
        }

        codecCtx->width = width;
        codecCtx->height = height;
        currentWidth = width;
        currentHeight = height;
        codecCtx->time_base = AVRational{ 1, fps };
        codecCtx->framerate = { fps, 1 };
        codecCtx->gop_size = fps * 2; // IDR every ~2 seconds: balances compression efficiency with low latency
        codecCtx->max_b_frames = 0; // low-latency
        codecCtx->bit_rate = g_startBitrateBps; // configurable start bitrate
        // Initialize VBV for low-latency: use 1x bitrate for stricter latency control
        codecCtx->rc_max_rate = codecCtx->bit_rate;
        codecCtx->rc_buffer_size = codecCtx->bit_rate; // Tighter VBV: 1x bitrate for minimal buffering

        // Desktop capture content uses the sRGB transfer function (IEC 61966-2-1, gamma ~2.2
        // with linear ramp), NOT the BT.709 camera transfer (~2.0 pure power curve).
        // Signalling AVCOL_TRC_IEC61966_2_1 lets the browser decoder pass the values straight
        // through to the display without an incorrect gamma shift.
        // Color primaries and matrix are identical between sRGB and BT.709 (same Rec. 709
        // chromaticities), so AVCOL_PRI_BT709 and AVCOL_SPC_BT709 are correct.
        codecCtx->color_range     = g_fullRangeColor ? AVCOL_RANGE_JPEG : AVCOL_RANGE_MPEG;
        codecCtx->color_primaries = AVCOL_PRI_BT709;
        codecCtx->color_trc       = AVCOL_TRC_IEC61966_2_1; // sRGB transfer (desktop content)
        codecCtx->colorspace      = AVCOL_SPC_BT709;

        // Configure D3D11VA frames with NV12 sw_format (GPU path)
        if (isHardware) {
            codecCtx->pix_fmt = AV_PIX_FMT_D3D11;

            if (av_hwdevice_ctx_create(&hwDeviceCtx, hwDeviceType, nullptr, nullptr, 0) < 0) {
                std::cerr << "[Encoder] Failed to create HW device context." << std::endl;
                return;
            }
            AVHWDeviceContext* deviceCtx = (AVHWDeviceContext*)hwDeviceCtx->data;
            AVD3D11VADeviceContext* d3d11vaDeviceCtx = (AVD3D11VADeviceContext*)deviceCtx->hwctx;
            d3d11vaDeviceCtx->device = (ID3D11Device*)GetD3DDevice().get();
            d3d11vaDeviceCtx->device->AddRef();
            codecCtx->hw_device_ctx = av_buffer_ref(hwDeviceCtx);

            hwFramesCtx = av_hwframe_ctx_alloc(hwDeviceCtx);
            if (!hwFramesCtx) {
                std::cerr << "[Encoder] Failed to allocate hwFramesCtx." << std::endl;
                return;
            }
            AVHWFramesContext* framesCtx = (AVHWFramesContext*)hwFramesCtx->data;
            framesCtx->format = AV_PIX_FMT_D3D11;
            framesCtx->sw_format = AV_PIX_FMT_NV12;
            framesCtx->width = width;
            framesCtx->height = height;
            framesCtx->initial_pool_size = std::max(g_hwFramePoolSize, g_nvAsyncDepth + 2);
            AVD3D11VAFramesContext* framesHw = (AVD3D11VAFramesContext*)framesCtx->hwctx;
            if (framesHw) {
                // Only RENDER_TARGET: VideoProcessor writes, NVENC reads via its
            // dedicated engine. SHADER_RESOURCE forces a shader-visible heap
            // allocation (extra VRAM + non-paged pool) that we never use.
                framesHw->BindFlags = D3D11_BIND_RENDER_TARGET;
                framesHw->MiscFlags = 0;
            }
            if (av_hwframe_ctx_init(hwFramesCtx) < 0) {
                std::cerr << "[Encoder] Failed to init hwFramesCtx." << std::endl;
                return;
            }
            codecCtx->hw_frames_ctx = av_buffer_ref(hwFramesCtx);

            g_hwFrames.clear();
            g_hwFrameIndex = 0;
            int desiredPool = std::max(g_hwFramePoolSize, g_nvAsyncDepth + 2);
            for (int i = 0; i < desiredPool; ++i) {
                AVFrame* f = av_frame_alloc();
                if (!f) { std::cerr << "[Encoder] av_frame_alloc failed for hw frame." << std::endl; return; }
                if (av_hwframe_get_buffer(codecCtx->hw_frames_ctx, f, 0) < 0) {
                    std::cerr << "[Encoder] av_hwframe_get_buffer failed for hw frame " << i << std::endl; return;
                }
                g_hwFrames.push_back(f);
            }

            InitializeVideoProcessor((ID3D11Device*)GetD3DDevice().get(), width, height);
        } else {
            codecCtx->pix_fmt = AV_PIX_FMT_NV12;
        }

        AVDictionary* opts = nullptr;
        if (encoderName == "h264_nvenc") {
            // Low-latency, faster preset configurable
            av_dict_set(&opts, "preset", g_nvPreset.c_str(), 0); // e.g. p4/p5
            av_dict_set(&opts, "tune", "ull", 0);
            av_dict_set(&opts, "rc", g_nvRc.c_str(), 0);         // cbr/cbr_hq
            av_dict_set(&opts, "repeat-headers", "1", 0);
            av_dict_set(&opts, "profile", "high", 0);
            {
                char buf[16];
                snprintf(buf, sizeof(buf), "%d", g_nvRcLookahead);
                av_dict_set(&opts, "rc-lookahead", buf, 0);
            }
            {
                char buf[16];
                snprintf(buf, sizeof(buf), "%d", g_nvBf);
                av_dict_set(&opts, "bf", buf, 0);
            }
            // Spatial AQ redistributes bits toward flat/dark regions. Strength 10 (max)
            // forces NVENC to do per-macroblock complexity analysis every frame —
            // measurably increasing GPU utilisation and encode latency at 120 fps.
            // Strength 4 retains perceptible quality gains at roughly half the analysis cost.
            // To disable entirely, set spatial_aq=0 (max throughput, minimum GPU load).
            av_dict_set(&opts, "spatial_aq", "1", 0);
            av_dict_set(&opts, "aq-strength", "4", 0);
            // Queue/pool sizes for throughput
            {
                char buf[16];
                snprintf(buf, sizeof(buf), "%d", g_nvAsyncDepth);
                av_dict_set(&opts, "async_depth", buf, 0);
            }
            {
                char buf[16];
                snprintf(buf, sizeof(buf), "%d", g_nvSurfaces);
                av_dict_set(&opts, "surfaces", buf, 0);
            }
            // Tighter VBV for low-latency: use 1x bitrate to minimize buffering
            char rateBuf[32];
            char buf2[32];
            snprintf(rateBuf, sizeof(rateBuf), "%d", codecCtx->bit_rate);
            snprintf(buf2, sizeof(buf2), "%d", codecCtx->bit_rate); // Tighter: 1x bitrate for minimal latency
            av_dict_set(&opts, "maxrate", rateBuf, 0);
            av_dict_set(&opts, "bufsize", buf2, 0);
            // Color metadata in H.264 SPS VUI: primaries/matrix = BT.709, transfer = sRGB
            av_dict_set(&opts, "colorspace", "bt709", 0);
            av_dict_set(&opts, "color_primaries", "bt709", 0);
            av_dict_set(&opts, "color_trc", "iec61966-2-1", 0); // sRGB transfer for desktop capture
            av_dict_set(&opts, "color_range", g_fullRangeColor ? "pc" : "tv", 0);
            // Let NVENC pick appropriate level automatically
            // Relax forced IDR (handled by gop_size)
        } else if (encoderName == "libx264") {
            av_dict_set(&opts, "preset", "ultrafast", 0);
            av_dict_set(&opts, "tune", "zerolatency", 0);
            av_dict_set(&opts, "colorprim", "bt709", 0);
            av_dict_set(&opts, "transfer",  "iec61966-2-1", 0); // sRGB transfer for desktop capture
            av_dict_set(&opts, "colormatrix","bt709", 0);
            av_dict_set(&opts, "fullrange", g_fullRangeColor ? "1" : "0", 0);
            av_dict_set(&opts, "profile", "high", 0);
            av_dict_set(&opts, "x264-params", "repeat-headers=1:no-cabac=1:nal-hrd=cbr:force-cfr=1", 0);
        }
        else if (encoderName == "h264_qsv") {
            av_dict_set(&opts, "preset", "veryfast", 0);
            av_dict_set(&opts, "zerolatency", "1", 0);
            av_dict_set(&opts, "repeat-headers", "1", 0);
            av_dict_set(&opts, "profile", "high", 0);
        }
        else if (encoderName == "h264_amf") {
            av_dict_set(&opts, "usage", "lowlatency_high_quality", 0);
            av_dict_set(&opts, "repeat-headers", "1", 0);
            av_dict_set(&opts, "profile", "high", 0);
        }

        // Ensure Annex B where supported (NVENC/libx264 use AVCodecContext flags instead of option)
        av_dict_set(&opts, "annexb", "1", 0);
        codecCtx->flags &= ~AV_CODEC_FLAG_GLOBAL_HEADER; // carry SPS/PPS in-band

        int openResult = avcodec_open2(codecCtx, codec, &opts);
        if (openResult < 0) {
            char errbuf[128];
            av_strerror(openResult, errbuf, sizeof(errbuf));
            std::cerr << "[Encoder] Failed to open codec: " << errbuf << std::endl;
            av_dict_free(&opts);
            return;
        }
        av_dict_free(&opts);

        int fmtRes = avformat_alloc_output_context2(&formatCtx, nullptr, "null", nullptr);
        if (fmtRes < 0) {
            char errbuf[128];
            av_strerror(fmtRes, errbuf, sizeof(errbuf));
            std::cerr << "[Encoder] Failed to allocate format context: " << errbuf << std::endl;
            return;
        }
        videoStream = avformat_new_stream(formatCtx, nullptr);
        if (!videoStream) {
            std::cerr << "[Encoder] Failed to create new video stream." << std::endl;
            return;
        }
        videoStream->id = formatCtx->nb_streams - 1;
        videoStream->time_base = codecCtx->time_base;
        avcodec_parameters_from_context(videoStream->codecpar, codecCtx);

        packet = av_packet_alloc();

        // CPU swscale path not used in GPU mode

        // Size output view LRU to number of hw frames (NV12 surfaces)
        g_outputViewLru.setCapacity(std::max<size_t>(8, g_hwFrames.size()));

        std::wcout << L"[Encoder] " << std::wstring(encoderName.begin(), encoderName.end()) << " encoder initialized successfully." << std::endl;

        // Log low-latency optimizations
        if (encoderName == "h264_nvenc") {
            std::wcout << L"[Encoder] Low-latency NVENC settings applied:" << std::endl;
            std::wcout << L"[Encoder]   - Preset: " << std::wstring(g_nvPreset.begin(), g_nvPreset.end()) << L" (optimized for speed)" << std::endl;
            std::wcout << L"[Encoder]   - Async Depth: " << g_nvAsyncDepth << L" (minimal internal buffering)" << std::endl;
            std::wcout << L"[Encoder]   - Surfaces: " << g_nvSurfaces << L" (async_depth + 1 for optimal throughput)" << std::endl;
            std::wcout << L"[Encoder]   - VBV Buffer: " << (codecCtx->rc_buffer_size / 1000) << L"kb (1x bitrate for strict latency)" << std::endl;
            std::wcout << L"[Encoder]   - B-frames: " << codecCtx->max_b_frames << L" (disabled for low latency)" << std::endl;
            std::wcout << L"[Encoder]   - GOP Size: " << codecCtx->gop_size << L" frames (IDR every ~" << (codecCtx->gop_size / fps) << L"s)" << std::endl;

            // Validate optimal settings for low latency
            if (g_nvSurfaces > g_nvAsyncDepth + 2) {
                std::wcout << L"[Encoder] WARNING: Surfaces (" << g_nvSurfaces << L") significantly exceed async_depth + 1 ("
                          << (g_nvAsyncDepth + 1) << L"). Consider reducing for lower latency." << std::endl;
            }
            if (codecCtx->rc_buffer_size > codecCtx->bit_rate) {
                std::wcout << L"[Encoder] WARNING: VBV buffer (" << (codecCtx->rc_buffer_size / 1000)
                          << L"kb) exceeds bitrate (" << (codecCtx->bit_rate / 1000) << L"kb). Latency may be higher than optimal." << std::endl;
            }
            // Validate preset for latency optimization
            if (g_nvPreset != "p4" && g_nvPreset != "p5") {
                std::wcout << L"[Encoder] INFO: Using preset '" << std::wstring(g_nvPreset.begin(), g_nvPreset.end())
                          << L"'. For optimal low-latency, consider p4 or p5 presets." << std::endl;
            }
            // Validate async_depth is reasonable
            if (g_nvAsyncDepth > 3) {
                std::wcout << L"[Encoder] WARNING: async_depth (" << g_nvAsyncDepth
                          << L") is high. Values > 3 may increase latency. Consider 1-2 for low latency." << std::endl;
            }

            std::wcout << L"[Encoder] EAGAIN handling: Enhanced backpressure detection with severity-based upstream dropping" << std::endl;
            std::wcout << L"[Encoder]   - Send queue depth: " << kMaxSendQueue << L" (drops oldest on overflow)" << std::endl;
            std::wcout << L"[Encoder]   - Backpressure levels: MILD/MODERATE/SEVERE with adaptive dropping" << std::endl;
        }

        // Ensure sender thread is running after encoder is ready
        StartSenderThreadIfNeeded();
    }

    void EncodeFrame(ID3D11Texture2D* texture, ID3D11DeviceContext* context, int width, int height, int64_t pts) {
        // Apply HDR tone mapping if enabled (before video processing)
        if (g_hdrToneMappingEnabled) {
            ApplyHdrToneMapping(texture, context);
        }

        // Collect per-packet samples (do NOT merge multiple frames into one WebRTC sample)
        std::vector<PendingSample> pendingSamples;

        {
        std::lock_guard<std::mutex> lock(g_encoderMutex);
        if (!codecCtx || g_hwFrames.empty() || !g_videoProcessor) {
            std::cerr << "[Encoder] Encoder/VideoProcessor not initialized." << std::endl;
            return;
        }

        // Ensure even dimensions and re-init encoder if size changed
        D3D11_TEXTURE2D_DESC srcDesc{};
        texture->GetDesc(&srcDesc);
        int srcW = (int)(srcDesc.Width & ~1U);
        int srcH = (int)(srcDesc.Height & ~1U);
        if (srcW != currentWidth || srcH != currentHeight) {
            // Reconfigure hw frames and video processor for new size
            // Recreate hardware frame pool for new size
            if (hwFramesCtx) { av_buffer_unref(&hwFramesCtx); hwFramesCtx = nullptr; }
            currentWidth = srcW; currentHeight = srcH;
            hwFramesCtx = av_hwframe_ctx_alloc(hwDeviceCtx);
            if (!hwFramesCtx) return;
            AVHWFramesContext* framesCtx = (AVHWFramesContext*)hwFramesCtx->data;
            framesCtx->format = AV_PIX_FMT_D3D11;
            framesCtx->sw_format = AV_PIX_FMT_NV12;
            framesCtx->width = currentWidth;
            framesCtx->height = currentHeight;
            framesCtx->initial_pool_size = std::max(g_hwFramePoolSize, g_nvAsyncDepth + 2);
            AVD3D11VAFramesContext* framesHw = (AVD3D11VAFramesContext*)framesCtx->hwctx;
            if (framesHw) {
                    framesHw->BindFlags = D3D11_BIND_RENDER_TARGET;
                framesHw->MiscFlags = 0;
            }
            if (av_hwframe_ctx_init(hwFramesCtx) < 0) return;
            if (codecCtx->hw_frames_ctx) av_buffer_unref(&codecCtx->hw_frames_ctx);
            codecCtx->hw_frames_ctx = av_buffer_ref(hwFramesCtx);
            for (AVFrame* f : g_hwFrames) { if (f) av_frame_free(&f); }
            g_hwFrames.clear();
            g_hwFrameIndex = 0;
            int desiredPool = std::max(g_hwFramePoolSize, g_nvAsyncDepth + 2);
            for (int i = 0; i < desiredPool; ++i) {
                AVFrame* f = av_frame_alloc();
                if (!f) return;
                if (av_hwframe_get_buffer(codecCtx->hw_frames_ctx, f, 0) < 0) return;
                g_hwFrames.push_back(f);
            }
            InitializeVideoProcessor((ID3D11Device*)GetD3DDevice().get(), currentWidth, currentHeight);
        }

        // GPU VideoProcessor BGRA->NV12
        // Cached input view for the source texture
        Microsoft::WRL::ComPtr<ID3D11VideoProcessorInputView> inView;
        if (!g_inputViewLru.get(texture, inView)) {
            // Validate format support for input (use cached results to avoid per-frame checks)
            D3D11_TEXTURE2D_DESC inTexDesc{};
            texture->GetDesc(&inTexDesc);
            if (!IsInputFormatSupported(inTexDesc.Format)) {
                std::wcerr << L"[Encoder][VP] Input format not supported by VideoProcessor. Format="
                           << std::hex << inTexDesc.Format << std::endl;
                return;
            }

            D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC inDesc{};
            inDesc.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
            inDesc.Texture2D.MipSlice = 0; inDesc.Texture2D.ArraySlice = 0;
            HRESULT hrIV = g_videoDevice->CreateVideoProcessorInputView(texture, g_vpEnumerator.Get(), &inDesc, inView.GetAddressOf());
            if (FAILED(hrIV)) {
                std::wcerr << L"[Encoder][VP] CreateVideoProcessorInputView failed. HRESULT=0x" << std::hex << hrIV
                           << L" W=" << inTexDesc.Width << L" H=" << inTexDesc.Height << L" Format=" << inTexDesc.Format << std::endl;
                return;
            }
            g_inputViewLru.put(texture, inView);
        }

        // Cached output view for FFmpeg's NV12 texture from ring
        AVFrame* hwFrameLocal = g_hwFrames[g_hwFrameIndex];
        g_hwFrameIndex = (g_hwFrameIndex + 1) % static_cast<int>(g_hwFrames.size());
        ID3D11Texture2D* ffmpegNV12 = (ID3D11Texture2D*)hwFrameLocal->data[0];
        Microsoft::WRL::ComPtr<ID3D11VideoProcessorOutputView> outView;
        if (!g_outputViewLru.get(ffmpegNV12, outView)) {
            // Validate output format support (use cached results to avoid per-frame checks)
            D3D11_TEXTURE2D_DESC outTexDesc{};
            ffmpegNV12->GetDesc(&outTexDesc);
            if (!IsOutputFormatSupported(outTexDesc.Format)) {
                std::wcerr << L"[Encoder][VP] Output format not supported by VideoProcessor. Format="
                           << std::hex << outTexDesc.Format << std::endl;
                return;
            }
            D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC outDesc{};
            outDesc.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
            outDesc.Texture2D.MipSlice = 0;
            HRESULT hrOV = g_videoDevice->CreateVideoProcessorOutputView(ffmpegNV12, g_vpEnumerator.Get(), &outDesc, outView.GetAddressOf());
            if (FAILED(hrOV)) {
                std::wcerr << L"[Encoder][VP] CreateVideoProcessorOutputView failed. HRESULT=0x" << std::hex << hrOV
                           << L" W=" << outTexDesc.Width << L" H=" << outTexDesc.Height << L" Format=" << outTexDesc.Format << std::endl;
                return;
            }
            g_outputViewLru.put(ffmpegNV12, outView);
        }
        D3D11_VIDEO_PROCESSOR_STREAM stream{}; stream.Enable = TRUE; stream.pInputSurface = inView.Get();
        if (FAILED(g_videoContext->VideoProcessorBlt(g_videoProcessor.Get(), outView.Get(), 0, 1, &stream))) {
            std::cerr << "[Encoder][VP] VideoProcessorBlt failed." << std::endl; return; }

        // No extra copy needed; VP wrote directly into ffmpegNV12 via output view

        hwFrameLocal->pts = av_rescale_q(pts, { 1, 1000000 }, codecCtx->time_base);

        int ret = avcodec_send_frame(codecCtx, hwFrameLocal);
        if (ret == AVERROR(EAGAIN)) {
            // Encoder output queue full; drain packets and retry once
            g_eagainCount.fetch_add(1);
            g_lastEagain = std::chrono::steady_clock::now();
            VideoMetrics::inc(VideoMetrics::eagainEvents());

            // Log EAGAIN events for debugging (throttled)
            static auto lastEagainLog2 = std::chrono::steady_clock::now();
            auto now = std::chrono::steady_clock::now();
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - lastEagainLog2);
            if (elapsed.count() >= 5000) { // Log at most every 5 seconds
                int currentCount = g_eagainCount.load();
                std::wcout << L"[Encoder] EAGAIN detected in EncodeFrame: encoder queue full, frame dropped. "
                          << L"Recent EAGAIN count: " << currentCount << std::endl;
                lastEagainLog2 = now;
            }
            for (;;) {
                int rcv = avcodec_receive_packet(codecCtx, packet);
                if (rcv == AVERROR(EAGAIN) || rcv == AVERROR_EOF) {
                    break;
                } else if (rcv < 0) {
                    char errBuf[128];
                    av_make_error_string(errBuf, 128, rcv);
                    std::cerr << "[Encoder] Drain on EAGAIN failed: " << errBuf << "\n";
                    break;
                }
                auto sample = MakePendingSampleFromPacket(packet, codecCtx, pts);
                if (!sample.data.empty()) pendingSamples.push_back(std::move(sample));
                av_packet_unref(packet);
            }
            // Retry once after draining
            ret = avcodec_send_frame(codecCtx, hwFrameLocal);
        }

        if (ret < 0) {
            char errBuf[128];
            av_make_error_string(errBuf, 128, ret);
            std::cerr << "[Encoder] Failed to send frame to encoder: " << errBuf << "\n";
            // fallthrough to not send anything
        }

        while (ret >= 0) {
            ret = avcodec_receive_packet(codecCtx, packet);
            if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
                break; // Need more input or end of stream
            }
            else if (ret < 0) {
                char errBuf[128];
                av_make_error_string(errBuf, 128, ret);
                std::cerr << "[Encoder] Failed to receive packet from encoder: " << errBuf << "\n";
                break;
            }
            auto sample = MakePendingSampleFromPacket(packet, codecCtx, pts);
            if (!sample.data.empty()) pendingSamples.push_back(std::move(sample));
            av_packet_unref(packet);
        }
        }
        for (auto& s : pendingSamples) {
            EnqueueEncodedSample(std::move(s.data), s.durationUs);
        }
    }

    void FlushEncoder() {
        // Core flush operations under mutex (keep this minimal)
        bool flushFailed = false;
        bool receiveError = false;
        {
            std::lock_guard<std::mutex> lock(g_encoderMutex);
            if (!codecCtx) return;

            int ret = avcodec_send_frame(codecCtx, nullptr); // Send flush frame
            if (ret < 0) {
                flushFailed = true;
                return;
            }
            while (ret >= 0) {
                ret = avcodec_receive_packet(codecCtx, packet);
                if (ret == AVERROR_EOF) {
                    break;
                }
                else if (ret < 0) {
                    receiveError = true;
                    break;
                }
                pushPacketToWebRTC(packet);
                av_packet_unref(packet);
            }
        }

        // Logging OUTSIDE mutex (potentially slow operations)
        if (flushFailed) {
            std::cerr << "[Encoder] Failed to send flush frame to encoder.\n";
        } else if (receiveError) {
            std::cerr << "[Encoder] Error while flushing encoder.\n";
        } else {
            std::wcout << L"[Encoder] Encoder flush complete\n";
        }
    }

    void RequestIDR() {
        std::lock_guard<std::mutex> lock(g_encoderMutex);
        if (!codecCtx) return;
        // Best-effort force IDR on next frame for common encoders
        av_opt_set(codecCtx->priv_data, "force_key_frames", "expr:gte(t,n_forced*1)", 0);
        // For NVENC, try forcing IDR if supported
        av_opt_set_int(codecCtx->priv_data, "forced-idr", 1, 0);
    }

    void FinalizeEncoder() {
        // Core cleanup under mutex (keep this minimal)
        {
            std::lock_guard<std::mutex> lock(g_encoderMutex);
            if (codecCtx) {
                avcodec_free_context(&codecCtx);
                codecCtx = nullptr;
            }
            if (formatCtx) {
                avformat_free_context(formatCtx);
                formatCtx = nullptr;
            }
            if (packet) { av_packet_free(&packet); packet = nullptr; }
            // Free hw frame ring
            for (AVFrame* f : g_hwFrames) { if (f) av_frame_free(&f); }
            g_hwFrames.clear();
            if (hwFramesCtx) {
                av_buffer_unref(&hwFramesCtx);
                hwFramesCtx = nullptr;
            }
            if (hwDeviceCtx) {
                av_buffer_unref(&hwDeviceCtx);
                hwDeviceCtx = nullptr;
            }
            // Stop sender thread after encoder teardown
            StopSenderThread();
        }

        // Logging OUTSIDE mutex (potentially slow operation)
        std::wcout << L"[Encoder] Encoder finalized.\n";
    }

    void AdjustBitrate(int new_bitrate) {
        // Update codec context + attempt NVENC runtime reconfigure, but NEVER reopen the
        // encoder. A full reopen (FinalizeEncoder + InitializeEncoder) on the hot encode
        // path blocks the pipeline for 50-200 ms, which is the primary cause of lag spikes.
        // If the runtime API doesn't support mid-stream rate changes (older NVENC drivers),
        // we log a warning and accept the mismatch — the codec context bit_rate update still
        // influences future VBV accounting and the encoder will converge towards the new rate.
        bool runtimeUpdateOk = true;
        {
            std::lock_guard<std::mutex> lock(g_encoderMutex);
            if (codecCtx) {
                codecCtx->bit_rate      = new_bitrate;
                codecCtx->rc_max_rate   = new_bitrate;
                codecCtx->rc_buffer_size = new_bitrate; // 1× bitrate VBV (tight, low-latency)
                if (codecCtx->priv_data) {
                    int r1 = av_opt_set_int(codecCtx->priv_data, "bitrate", new_bitrate, 0);
                    int r2 = av_opt_set_int(codecCtx->priv_data, "maxrate", new_bitrate, 0);
                    int r3 = av_opt_set_int(codecCtx->priv_data, "bufsize", new_bitrate, 0);
                    runtimeUpdateOk = (r1 >= 0 && r2 >= 0 && r3 >= 0);
                }
            }
        }

        std::wcout << L"[Encoder] Bitrate adjusted to " << new_bitrate
                   << L" bps" << (runtimeUpdateOk ? L"" : L" (context-only; runtime API unavailable)") << L"\n";
    }

    bool IsBacklogged(int recent_window_ms, int min_events) {
        auto now = std::chrono::steady_clock::now();
        auto since = std::chrono::duration_cast<std::chrono::milliseconds>(now - g_lastEagain).count();
        int cnt = g_eagainCount.load();
        return (since <= recent_window_ms) && (cnt >= min_events);
    }

    BackpressureLevel GetBackpressureLevel() {
        auto now = std::chrono::steady_clock::now();
        auto sinceLastEagain = std::chrono::duration_cast<std::chrono::milliseconds>(now - g_lastEagain).count();

        // Decay: if no EAGAIN in 2+ seconds, reset counter to avoid permanent over-reactivity
        if (sinceLastEagain > 2000) {
            g_eagainCount.store(0, std::memory_order_relaxed);
            return BackpressureLevel::NONE;
        }

        int eagainCount = g_eagainCount.load();

        if (sinceLastEagain <= 500 && eagainCount >= 5) {
            return BackpressureLevel::SEVERE;
        }
        else if (sinceLastEagain <= 1000 && eagainCount >= 3) {
            return BackpressureLevel::MODERATE;
        }
        else if (sinceLastEagain <= 2000 && eagainCount >= 2) {
            return BackpressureLevel::MILD;
        }

        return BackpressureLevel::NONE;
    }

    void GetAndResetBackpressureStats(int &eagainEvents) {
        eagainEvents = g_eagainCount.exchange(0);
    }
}