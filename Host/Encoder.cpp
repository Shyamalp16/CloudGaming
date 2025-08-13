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
        codecCtx->gop_size = 10;
        codecCtx->max_b_frames = 0;
        codecCtx->bit_rate = 30000000;

        // Use NV12 software frames and let NVENC upload; avoids BGRA->NV12 on GPU issues
        codecCtx->pix_fmt = AV_PIX_FMT_NV12;

        AVDictionary* opts = nullptr;
        if (encoderName == "h264_nvenc") {
            av_dict_set(&opts, "preset", "p7", 0);
            av_dict_set(&opts, "rc", "vbr", 0);
            av_dict_set(&opts, "zerolatency", "1", 0);
            av_dict_set(&opts, "repeat-headers", "1", 0);
            av_dict_set(&opts, "profile", "baseline", 0);
            // Force IDR more frequently to ensure decoder gets keyframes quickly
            av_dict_set(&opts, "forced-idr", "1", 0);
            av_dict_set(&opts, "gops-per-idr", "1", 0);
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

        // Allocate NV12 frame buffer for encoder input
        if (nv12Frame) {
            av_frame_free(&nv12Frame);
        }
        nv12Frame = av_frame_alloc();
        nv12Frame->format = AV_PIX_FMT_NV12;
        nv12Frame->width = width;
        nv12Frame->height = height;
        if (av_frame_get_buffer(nv12Frame, 32) < 0) {
            std::cerr << "[Encoder] Failed to allocate NV12 frame buffer." << std::endl;
            return;
        }
        // Initialize SWS converter from BGRA -> NV12
        if (swsCtx) {
            sws_freeContext(swsCtx);
            swsCtx = nullptr;
        }
        swsCtx = sws_getContext(width, height, AV_PIX_FMT_BGRA, width, height, AV_PIX_FMT_NV12, SWS_BILINEAR, nullptr, nullptr, nullptr);
        if (!swsCtx) {
            std::cerr << "[Encoder] Failed to create sws context." << std::endl;
            return;
        }

        std::wcout << L"[Encoder] " << std::wstring(encoderName.begin(), encoderName.end()) << " encoder initialized successfully." << std::endl;
    }

    void EncodeFrame(ID3D11Texture2D* texture, ID3D11DeviceContext* context, int width, int height, int64_t pts) {
        std::lock_guard<std::mutex> lock(g_encoderMutex);
        if (!codecCtx || !swsCtx || !nv12Frame) {
            std::cerr << "[Encoder] Encoder not initialized or frames not allocated." << std::endl;
            return;
        }

        // Ensure even dimensions and re-init encoder if size changed
        D3D11_TEXTURE2D_DESC srcDesc{};
        texture->GetDesc(&srcDesc);
        int srcW = (int)(srcDesc.Width & ~1U);
        int srcH = (int)(srcDesc.Height & ~1U);
        if (srcW != currentWidth || srcH != currentHeight) {
            FinalizeEncoder();
            InitializeEncoder("output.mp4", srcW, srcH, codecCtx->framerate.num);
            if (!codecCtx || !swsCtx || !nv12Frame) {
                std::cerr << "[Encoder] Re-init failed on size change." << std::endl;
                return;
            }
        }

        // Create a staging texture for CPU readback
        D3D11_TEXTURE2D_DESC stagingDesc = srcDesc;
        stagingDesc.BindFlags = 0;
        stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        stagingDesc.Usage = D3D11_USAGE_STAGING;
        stagingDesc.MiscFlags = 0;
        Microsoft::WRL::ComPtr<ID3D11Texture2D> staging;
        ID3D11Device* device = (ID3D11Device*)GetD3DDevice().get();
        if (FAILED(device->CreateTexture2D(&stagingDesc, nullptr, staging.GetAddressOf()))) {
            std::cerr << "[Encoder] Failed to create staging texture." << std::endl;
            return;
        }
        context->CopyResource(staging.Get(), texture);

        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (FAILED(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped))) {
            std::cerr << "[Encoder] Failed to map staging texture." << std::endl;
            return;
        }

        // Convert BGRA (with row pitch) -> NV12 using swscale
        const uint8_t* srcData[4] = { (const uint8_t*)mapped.pData, nullptr, nullptr, nullptr };
        int srcLinesize[4] = { (int)mapped.RowPitch, 0, 0, 0 };
        sws_scale(swsCtx, srcData, srcLinesize, 0, currentHeight, nv12Frame->data, nv12Frame->linesize);
        context->Unmap(staging.Get(), 0);

        // Set PTS and send to encoder
        nv12Frame->pts = av_rescale_q(pts, { 1, 1000000 }, codecCtx->time_base);

        int ret = avcodec_send_frame(codecCtx, nv12Frame);
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