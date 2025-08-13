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

SwsContext* Encoder::swsCtx = nullptr;
AVFrame* Encoder::nv12Frame = nullptr;
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
        int result = sendVideoPacket(packet->data, packet->size, g_latestPTS);
        if (result != 0) {
            std::wcerr << L"[WebRTC] Failed to send video packet to WebRTC module. Error code: " << result << L"\n";
        }
    }

    void InitializeEncoder(const std::string& fileName, int width, int height, int fps) {
        std::lock_guard<std::mutex> lock(g_encoderMutex);

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
        codecCtx->gop_size = fps * 2; // Relax IDR cadence to ~2 seconds
        codecCtx->max_b_frames = 0; // low-latency
        codecCtx->bit_rate = 50000000; // 50 Mbps target bitrate

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
            av_dict_set(&opts, "preset", "p7", 0);
            av_dict_set(&opts, "rc", "cbr", 0);
            av_dict_set(&opts, "repeat-headers", "1", 0);
            av_dict_set(&opts, "profile", "main", 0); // use Main profile
            av_dict_set(&opts, "rc-lookahead", "0", 0);
            av_dict_set(&opts, "bf", "0", 0);
            // Spatial AQ for detail preservation
            av_dict_set(&opts, "spatial_aq", "1", 0);
            av_dict_set(&opts, "aq-strength", "10", 0);
            // Looser VBV to reduce quality pulsing
            char rateBuf[32];
            char buf2[32];
            snprintf(rateBuf, sizeof(rateBuf), "%d", codecCtx->bit_rate);
            snprintf(buf2, sizeof(buf2), "%d", codecCtx->bit_rate * 2);
            av_dict_set(&opts, "maxrate", rateBuf, 0);
            av_dict_set(&opts, "bufsize", buf2, 0);
            // Relax forced IDR (handled by gop_size)
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
        Microsoft::WRL::ComPtr<ID3D11VideoProcessorInputView> inView;
        Microsoft::WRL::ComPtr<ID3D11VideoProcessorOutputView> outView;
        D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC inDesc{};
        inDesc.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
        inDesc.Texture2D.MipSlice = 0; inDesc.Texture2D.ArraySlice = 0;
        ID3D11Device* device = (ID3D11Device*)GetD3DDevice().get();
    if (FAILED(g_videoDevice->CreateVideoProcessorInputView(texture, g_vpEnumerator.Get(), &inDesc, inView.GetAddressOf()))) {
            std::cerr << "[Encoder][VP] CreateVideoProcessorInputView failed." << std::endl; return; }
        D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC outDesc{};
        outDesc.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
        outDesc.Texture2D.MipSlice = 0;
        // Create output view directly on FFmpeg's NV12 texture (hwFrame->data[0]) to avoid extra copy
        ID3D11Texture2D* ffmpegNV12 = (ID3D11Texture2D*)hwFrame->data[0];
        if (FAILED(g_videoDevice->CreateVideoProcessorOutputView(ffmpegNV12, g_vpEnumerator.Get(), &outDesc, outView.GetAddressOf()))) {
            std::cerr << "[Encoder][VP] CreateVideoProcessorOutputView failed." << std::endl; return; }
        D3D11_VIDEO_PROCESSOR_STREAM stream{}; stream.Enable = TRUE; stream.pInputSurface = inView.Get();
    if (FAILED(g_videoContext->VideoProcessorBlt(g_videoProcessor.Get(), outView.Get(), 0, 1, &stream))) {
            std::cerr << "[Encoder][VP] VideoProcessorBlt failed." << std::endl; return; }

        // No extra copy needed; VP wrote directly into ffmpegNV12 via output view

        hwFrame->pts = av_rescale_q(pts, { 1, 1000000 }, codecCtx->time_base);

        int ret = avcodec_send_frame(codecCtx, hwFrame);
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
        }
    }
}