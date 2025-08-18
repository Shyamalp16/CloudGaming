#include "Encoder.h"
#include "GlobalTime.h"
#include "pion_webrtc.h"
#include <functional>
#include <mutex>
#include <condition_variable>
#include <fstream>
#include <chrono>
#include "PacketQueue.h"
#include "D3DHelpers.h" // For GetGpuVendorId
#include <libavutil/hwcontext_d3d11va.h>
#include <d3d11.h>
#include <wrl.h>
#include <unordered_map>

// Removed unused software conversion components
int Encoder::currentWidth = 0;
int Encoder::currentHeight = 0;

std::mutex Encoder::g_encoderMutex;

// Static variables for encoder state
AVFormatContext* Encoder::formatCtx = nullptr;
AVCodecContext* Encoder::codecCtx = nullptr;
AVStream* Encoder::videoStream = nullptr;
AVPacket* Encoder::packet = nullptr;
AVFrame* Encoder::hwFrame = nullptr;
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
// Cached views to avoid per-frame allocations
static std::unordered_map<ID3D11Texture2D*, Microsoft::WRL::ComPtr<ID3D11VideoProcessorInputView>> g_inputViewCache;
static std::unordered_map<ID3D11Texture2D*, Microsoft::WRL::ComPtr<ID3D11VideoProcessorOutputView>> g_outputViewCache;

// Encoder runtime configuration (overridable from host config)
static int g_startBitrateBps = 20000000; // 20 Mbps default
static int g_minBitrateBps = 10000000;   // 10 Mbps default
static int g_maxBitrateBps = 50000000;   // 50 Mbps default
static int g_minBitrateController = 10000000;
static int g_maxBitrateController = 50000000;
static int g_increaseStep = 5000000;         // +5 Mbps
static int g_decreaseCooldownMs = 300;       // ms
static int g_cleanSamplesRequired = 3;
static int g_increaseIntervalMs = 1000;      // ms
static int g_currentBitrate = 25000000;      // start ~25 Mbps
static int g_cleanSamples = 0;
static std::chrono::steady_clock::time_point g_lastChange = std::chrono::steady_clock::now();
static std::atomic<bool> g_pendingReopen{false};
static std::atomic<int> g_reopenTargetBitrate{0};
static std::atomic<int> g_eagainCount{0};
static std::chrono::steady_clock::time_point g_lastEagain = std::chrono::steady_clock::now();

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

    std::wcout << L"[Encoder][VP] Initialized VideoProcessor for " << width << L"x" << height << std::endl;
    return true;
}

namespace Encoder {
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

    void OnRtcpFeedback(double packetLoss, double /*rtt*/, double /*jitter*/) {
        auto now = std::chrono::steady_clock::now();
        auto since = std::chrono::duration_cast<std::chrono::milliseconds>(now - g_lastChange).count();

        if (packetLoss >= 0.03) { // >= 3% loss
            if (since >= g_decreaseCooldownMs) {
                double factor = (packetLoss >= 0.10) ? 0.6 : 0.8;
                int target = static_cast<int>(g_currentBitrate * factor);
                g_currentBitrate = std::max(g_minBitrateController, target);
                AdjustBitrate(g_currentBitrate);
                g_lastChange = now;
            }
            g_cleanSamples = 0;
            return;
        }

        g_cleanSamples++;
        if (since >= g_increaseIntervalMs && g_cleanSamples >= g_cleanSamplesRequired) {
            int target = g_currentBitrate + g_increaseStep;
            if (target <= g_maxBitrateController) {
                g_currentBitrate = target;
                AdjustBitrate(g_currentBitrate);
                g_lastChange = now;
            }
            g_cleanSamples = 0;
        }
    }
    extern "C" void OnPLI() {
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
        //std::wcout << L"[WebRTC] Pushing encoded packets (PTS: " << packet->pts << L") to WebRTC module.\n";
        {
            std::lock_guard<std::mutex> lock(g_frameMutex);
            g_latestFrameData.assign(packet->data, packet->data + packet->size);
            // Convert packet PTS from codec time_base to microseconds for downstream RTP timestamping
            int64_t pts_us = av_rescale_q(packet->pts, codecCtx->time_base, AVRational{1, 1000000});
            g_latestPTS = pts_us;
            g_frameReady = true;
        }
        g_frameAvailable.notify_one();

        logNALUnits(packet->data, packet->size);

        // Pass PTS in microseconds to Go layer
        // Provide frame duration from current encoder FPS for paced sending
        static bool loggedFps = false;
        int fpsNum = codecCtx ? codecCtx->framerate.num : 60;
        int fpsDen = codecCtx ? codecCtx->framerate.den : 1;
        double fpsVal = (fpsDen != 0) ? static_cast<double>(fpsNum) / static_cast<double>(fpsDen) : 60.0;
        if (!loggedFps) {
            std::wcout << L"[Encoder] framerate num/den: " << fpsNum << L"/" << fpsDen << L" (~" << fpsVal << L" fps)\n";
            loggedFps = true;
        }
        // Fallback to measured delta between successive frames if needed
        static int64_t lastPtsUs = -1;
        int64_t frameDurationUs;
        if (lastPtsUs > 0 && g_latestPTS > lastPtsUs) {
            frameDurationUs = g_latestPTS - lastPtsUs;
        } else {
            frameDurationUs = static_cast<int64_t>(1000000.0 / (fpsVal > 1.0 ? fpsVal : 60.0));
        }
        lastPtsUs = g_latestPTS;
        if (frameDurationUs <= 0) frameDurationUs = 8333; // ~120fps fallback
        int result = sendVideoSample(packet->data, packet->size, frameDurationUs);
        if (result != 0) {
            std::wcerr << L"[WebRTC] Failed to send video packet to WebRTC module. Error code: " << result << L"\n";
        }
    }

    void InitializeEncoder(const std::string& fileName, int width, int height, int fps) {
        std::lock_guard<std::mutex> lock(g_encoderMutex);

        // Clear caches when (re)initializing encoder
        g_inputViewCache.clear();
        g_outputViewCache.clear();

        if (codecCtx) avcodec_free_context(&codecCtx);
        if (hwFrame) av_frame_free(&hwFrame);
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
        codecCtx->gop_size = fps * 2; // IDR every ~2 seconds to reduce keyframe overhead
        codecCtx->max_b_frames = 0; // low-latency
        codecCtx->bit_rate = g_startBitrateBps; // configurable start bitrate
        // Initialize VBV to track target bitrate and avoid pulsing
        codecCtx->rc_max_rate = codecCtx->bit_rate;
        codecCtx->rc_buffer_size = codecCtx->bit_rate * 2;

        // Signal SDR BT.709 full range for desktop capture; match encoder opts below
        codecCtx->color_range     = AVCOL_RANGE_JPEG;   // full (PC range)
        codecCtx->color_primaries = AVCOL_PRI_BT709;
        codecCtx->color_trc       = AVCOL_TRC_BT709;
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
            framesCtx->initial_pool_size = 8;
            // Ensure D3D11 textures are created with render target and SRV so we can CopyResource into them and the encoder can read
            AVD3D11VAFramesContext* framesHw = (AVD3D11VAFramesContext*)framesCtx->hwctx;
            if (framesHw) {
                framesHw->BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
                framesHw->MiscFlags = 0;
            }
            if (av_hwframe_ctx_init(hwFramesCtx) < 0) {
                std::cerr << "[Encoder] Failed to init hwFramesCtx." << std::endl;
                return;
            }
            codecCtx->hw_frames_ctx = av_buffer_ref(hwFramesCtx);

            hwFrame = av_frame_alloc();
            if (av_hwframe_get_buffer(codecCtx->hw_frames_ctx, hwFrame, 0) < 0) {
                // Retry with larger pool and explicit bind flags
                AVHWFramesContext* retryCtx = (AVHWFramesContext*)hwFramesCtx->data;
                retryCtx->initial_pool_size = 16;
                AVD3D11VAFramesContext* hw = (AVD3D11VAFramesContext*)retryCtx->hwctx;
                if (hw) hw->BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
                if (av_hwframe_ctx_init(hwFramesCtx) < 0 || av_hwframe_get_buffer(codecCtx->hw_frames_ctx, hwFrame, 0) < 0) {
                    std::cerr << "[Encoder] Failed to alloc hwFrame after retry." << std::endl;
                    return;
                }
            }

            InitializeVideoProcessor((ID3D11Device*)GetD3DDevice().get(), width, height);
        } else {
            codecCtx->pix_fmt = AV_PIX_FMT_NV12;
        }

        AVDictionary* opts = nullptr;
        if (encoderName == "h264_nvenc") {
            // Use high-quality preset per your preference, but keep low-latency flags
            av_dict_set(&opts, "preset", "p7", 0);            // highest quality
            av_dict_set(&opts, "tune", "ull", 0);             // ultra low latency
            av_dict_set(&opts, "rc", "cbr", 0);
            av_dict_set(&opts, "repeat-headers", "1", 0);
            av_dict_set(&opts, "profile", "baseline", 0);
            av_dict_set(&opts, "rc-lookahead", "0", 0);
            av_dict_set(&opts, "bf", "0", 0);
            // Keep AQ for visual quality while letting NVENC handle perf
            av_dict_set(&opts, "spatial_aq", "1", 0);
            av_dict_set(&opts, "aq-strength", "10", 0);
            // Reduce queueing depth to help frame pacing
            av_dict_set(&opts, "async_depth", "1", 0);
            av_dict_set(&opts, "surfaces", "4", 0);
            // Looser VBV to reduce quality pulsing
            char rateBuf[32];
            char buf2[32];
            snprintf(rateBuf, sizeof(rateBuf), "%d", codecCtx->bit_rate);
            snprintf(buf2, sizeof(buf2), "%d", codecCtx->bit_rate * 2);
            av_dict_set(&opts, "maxrate", rateBuf, 0);
            av_dict_set(&opts, "bufsize", buf2, 0);
            // Ensure color metadata is set on stream
            av_dict_set(&opts, "colorspace", "bt709", 0);
            av_dict_set(&opts, "color_primaries", "bt709", 0);
            av_dict_set(&opts, "color_trc", "bt709", 0);
            av_dict_set(&opts, "color_range", "pc", 0); // full range
            // Let NVENC pick appropriate level automatically
            // Relax forced IDR (handled by gop_size)
        } else if (encoderName == "libx264") {
            // x264 names differ; set BT.709 SDR limited
            av_dict_set(&opts, "colorprim", "bt709", 0);
            av_dict_set(&opts, "transfer",  "bt709", 0);
            av_dict_set(&opts, "colormatrix","bt709", 0);
            av_dict_set(&opts, "fullrange", "1", 0); // full range
            av_dict_set(&opts, "profile", "baseline", 0); // match SDP baseline
        }
        else if (encoderName == "h264_qsv") {
            av_dict_set(&opts, "preset", "veryfast", 0);
            av_dict_set(&opts, "zerolatency", "1", 0);
            av_dict_set(&opts, "repeat-headers", "1", 0);
            av_dict_set(&opts, "profile", "baseline", 0);
        }
        else if (encoderName == "h264_amf") {
            av_dict_set(&opts, "usage", "lowlatency_high_quality", 0);
            av_dict_set(&opts, "repeat-headers", "1", 0);
            av_dict_set(&opts, "profile", "baseline", 0);
        }
        else if (encoderName == "libx264") {
            av_dict_set(&opts, "preset", "ultrafast", 0);
            av_dict_set(&opts, "tune", "zerolatency", 0);
            av_dict_set(&opts, "x264-params", "repeat-headers=1", 0);
        }

        // Ensure Annex B where supported (NVENC/libx264 use AVCodecContext flags instead of option)
        av_dict_set(&opts, "annexb", "1", 0);
        codecCtx->flags &= ~AV_CODEC_FLAG_GLOBAL_HEADER; // carry SPS/PPS in-band

        if (avcodec_open2(codecCtx, codec, &opts) < 0) {
            std::cerr << "[Encoder] Failed to open codec." << std::endl;
            av_dict_free(&opts);
            return;
        }
        av_dict_free(&opts);

        if (avformat_alloc_output_context2(&formatCtx, nullptr, "null", nullptr) < 0) {
            std::cerr << "[Encoder] Failed to allocate format context." << std::endl;
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

        std::wcout << L"[Encoder] " << std::wstring(encoderName.begin(), encoderName.end()) << " encoder initialized successfully." << std::endl;
    }

    void EncodeFrame(ID3D11Texture2D* texture, ID3D11DeviceContext* context, int width, int height, int64_t pts) {
        std::lock_guard<std::mutex> lock(g_encoderMutex);
        // If a bitrate reopen was requested and we have a valid context, perform it now
        if (g_pendingReopen.load() && codecCtx) {
            int fpsNum = codecCtx->framerate.num > 0 ? codecCtx->framerate.num : 60;
            int fpsDen = codecCtx->framerate.den > 0 ? codecCtx->framerate.den : 1;
            int fps = fpsDen != 0 ? fpsNum / fpsDen : 60;
            if (fps <= 0) fps = 60;
            int target = g_reopenTargetBitrate.load();
            std::wcout << L"[Encoder] Reopening encoder to apply bitrate=" << target << L" bps" << std::endl;
            FlushEncoder();
            FinalizeEncoder();
            InitializeEncoder("output.mp4", currentWidth, currentHeight, fps);
            if (target > 0) {
                AdjustBitrate(target);
            }
            g_pendingReopen.store(false);
        }
        if (!codecCtx || !hwFrame || !g_videoProcessor) {
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
            if (hwFrame) { av_frame_free(&hwFrame); hwFrame = nullptr; }
            if (hwFramesCtx) { av_buffer_unref(&hwFramesCtx); hwFramesCtx = nullptr; }
            currentWidth = srcW; currentHeight = srcH;
            hwFramesCtx = av_hwframe_ctx_alloc(hwDeviceCtx);
            if (!hwFramesCtx) return;
            AVHWFramesContext* framesCtx = (AVHWFramesContext*)hwFramesCtx->data;
            framesCtx->format = AV_PIX_FMT_D3D11;
            framesCtx->sw_format = AV_PIX_FMT_NV12;
            framesCtx->width = currentWidth;
            framesCtx->height = currentHeight;
            framesCtx->initial_pool_size = 8;
            AVD3D11VAFramesContext* framesHw = (AVD3D11VAFramesContext*)framesCtx->hwctx;
            if (framesHw) {
                framesHw->BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
                framesHw->MiscFlags = 0;
            }
            if (av_hwframe_ctx_init(hwFramesCtx) < 0) return;
            if (codecCtx->hw_frames_ctx) av_buffer_unref(&codecCtx->hw_frames_ctx);
            codecCtx->hw_frames_ctx = av_buffer_ref(hwFramesCtx);
            hwFrame = av_frame_alloc();
            if (av_hwframe_get_buffer(codecCtx->hw_frames_ctx, hwFrame, 0) < 0) return;
            InitializeVideoProcessor((ID3D11Device*)GetD3DDevice().get(), currentWidth, currentHeight);
        }

        // GPU VideoProcessor BGRA->NV12
        // Cached input view for the source texture
        Microsoft::WRL::ComPtr<ID3D11VideoProcessorInputView> inView;
        auto itIn = g_inputViewCache.find(texture);
        if (itIn != g_inputViewCache.end()) {
            inView = itIn->second;
        } else {
            // Validate format support for input
            D3D11_TEXTURE2D_DESC inTexDesc{};
            texture->GetDesc(&inTexDesc);
            UINT support = 0;
            HRESULT chk = g_vpEnumerator->CheckVideoProcessorFormat(inTexDesc.Format, &support);
            if (FAILED(chk) || (support & D3D11_VIDEO_PROCESSOR_FORMAT_SUPPORT_INPUT) == 0) {
                std::wcerr << L"[Encoder][VP] Input format not supported by VideoProcessor. HRESULT=0x"
                           << std::hex << chk << L" format=" << inTexDesc.Format << std::endl;
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
            g_inputViewCache[texture] = inView;
        }

        // Cached output view for FFmpeg's NV12 texture
        ID3D11Texture2D* ffmpegNV12 = (ID3D11Texture2D*)hwFrame->data[0];
        Microsoft::WRL::ComPtr<ID3D11VideoProcessorOutputView> outView;
        auto itOut = g_outputViewCache.find(ffmpegNV12);
        if (itOut != g_outputViewCache.end()) {
            outView = itOut->second;
        } else {
            // Validate output format support (NV12)
            D3D11_TEXTURE2D_DESC outTexDesc{};
            ffmpegNV12->GetDesc(&outTexDesc);
            UINT supportOut = 0;
            HRESULT chkOut = g_vpEnumerator->CheckVideoProcessorFormat(outTexDesc.Format, &supportOut);
            if (FAILED(chkOut) || (supportOut & D3D11_VIDEO_PROCESSOR_FORMAT_SUPPORT_OUTPUT) == 0) {
                std::wcerr << L"[Encoder][VP] Output format not supported by VideoProcessor. HRESULT=0x"
                           << std::hex << chkOut << L" format=" << outTexDesc.Format << std::endl;
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
            g_outputViewCache[ffmpegNV12] = outView;
        }
        D3D11_VIDEO_PROCESSOR_STREAM stream{}; stream.Enable = TRUE; stream.pInputSurface = inView.Get();
        if (FAILED(g_videoContext->VideoProcessorBlt(g_videoProcessor.Get(), outView.Get(), 0, 1, &stream))) {
            std::cerr << "[Encoder][VP] VideoProcessorBlt failed." << std::endl; return; }

        // No extra copy needed; VP wrote directly into ffmpegNV12 via output view

        hwFrame->pts = av_rescale_q(pts, { 1, 1000000 }, codecCtx->time_base);

        int ret = avcodec_send_frame(codecCtx, hwFrame);
        if (ret == AVERROR(EAGAIN)) {
            // Encoder output queue full; drain packets and retry once
            g_eagainCount.fetch_add(1);
            g_lastEagain = std::chrono::steady_clock::now();
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
                packet->stream_index = videoStream->index;
                pushPacketToWebRTC(packet);
                av_packet_unref(packet);
            }
            // Retry once after draining
            ret = avcodec_send_frame(codecCtx, hwFrame);
        }

        if (ret < 0) {
            char errBuf[128];
            av_make_error_string(errBuf, 128, ret);
            std::cerr << "[Encoder] Failed to send frame to encoder: " << errBuf << "\n";
            return;
        }

        while (ret >= 0) {
            ret = avcodec_receive_packet(codecCtx, packet);
            if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
                return; // Need more input or end of stream
            }
            else if (ret < 0) {
                char errBuf[128];
                av_make_error_string(errBuf, 128, ret);
                std::cerr << "[Encoder] Failed to receive packet from encoder: " << errBuf << "\n";
                return;
            }

            packet->stream_index = videoStream->index;
            pushPacketToWebRTC(packet);
            av_packet_unref(packet);
        }
    }

    void FlushEncoder() {
        std::lock_guard<std::mutex> lock(g_encoderMutex);
        if (!codecCtx) return;
        std::wcout << L"[DEBUG] Flushing encoder\n";
        int ret = avcodec_send_frame(codecCtx, nullptr); // Send flush frame
        if (ret < 0) {
            std::cerr << "[Encoder] Failed to send flush frame to encoder.\n";
            return;
        }
        while (ret >= 0) {
            ret = avcodec_receive_packet(codecCtx, packet);
            if (ret == AVERROR_EOF) {
                break;
            }
            else if (ret < 0) {
                std::cerr << "[Encoder] Error while flushing encoder.\n";
                break;
            }
            pushPacketToWebRTC(packet);
            av_packet_unref(packet);
        }
        std::wcout << L"[DEBUG] Encoder flush complete\n";
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
        std::lock_guard<std::mutex> lock(g_encoderMutex);
        std::wcout << L"[DEBUG] Finalizing encoder...\n";

        if (codecCtx) {
            avcodec_free_context(&codecCtx);
            codecCtx = nullptr;
        }
        if (formatCtx) {
            avformat_free_context(formatCtx);
            formatCtx = nullptr;
        }
        if (packet) {
            av_packet_free(&packet);
            packet = nullptr;
        }
        if (hwFrame) {
            av_frame_free(&hwFrame);
            hwFrame = nullptr;
        }
        if (hwFramesCtx) {
            av_buffer_unref(&hwFramesCtx);
            hwFramesCtx = nullptr;
        }
        if (hwDeviceCtx) {
            av_buffer_unref(&hwDeviceCtx);
            hwDeviceCtx = nullptr;
        }
        std::wcout << L"[Encoder] Encoder finalized.\n";
    }

    void AdjustBitrate(int new_bitrate) {
        std::lock_guard<std::mutex> lock(g_encoderMutex);
        if (codecCtx) {
            std::wcout << L"[Encoder] Adjusting bitrate to " << new_bitrate << L" bps\n";
            codecCtx->bit_rate = new_bitrate;
            codecCtx->rc_max_rate = new_bitrate;
            codecCtx->rc_buffer_size = new_bitrate * 2;
            // Apply encoder-specific runtime controls
            if (codecCtx->priv_data) {
                int r1 = av_opt_set_int(codecCtx->priv_data, "bitrate", new_bitrate, 0);
                int r2 = av_opt_set_int(codecCtx->priv_data, "maxrate", new_bitrate, 0);
                int r3 = av_opt_set_int(codecCtx->priv_data, "bufsize", new_bitrate * 2, 0);
                if (r1 < 0 || r2 < 0 || r3 < 0) {
                    std::wcout << L"[Encoder] Runtime bitrate update not fully supported by this codec. Scheduling reopen...\n";
                    g_pendingReopen.store(true);
                    g_reopenTargetBitrate.store(new_bitrate);
                }
            }
            // Force an IDR soon so downstream adapts to new rate quickly
            av_opt_set(codecCtx->priv_data, "force_key_frames", "expr:gte(t,n_forced*1)", 0);
        }
    }

    bool IsBacklogged(int recent_window_ms, int min_events) {
        auto now = std::chrono::steady_clock::now();
        auto since = std::chrono::duration_cast<std::chrono::milliseconds>(now - g_lastEagain).count();
        int cnt = g_eagainCount.load();
        return (since <= recent_window_ms) && (cnt >= min_events);
    }

    void GetAndResetBackpressureStats(int &eagainEvents) {
        eagainEvents = g_eagainCount.exchange(0);
    }
}