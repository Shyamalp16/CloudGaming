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
    void ConvertFrame(const uint8_t* bgraData, int bgraPitch, int width, int height);

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
                std::wcout << L"[DEBUG] NAL Unit Type: " << (int)nalUnitType << L"\n";
                int nextPos = pos + 1;
                while (nextPos + 3 < size) {
                    if (data[nextPos] == 0x00 && data[nextPos + 1] == 0x00 && data[nextPos + 2] == 0x00 && data[nextPos + 3] == 0x01) {
                        break;
                    }
                    nextPos++;
                }
                int nalSize = (nextPos + 3 < size) ? (nextPos - pos + 3) : (size - pos);
                std::wcout << L"[DEBUG] NAL Unit Size: " << nalSize << L"\n";
                pos = nextPos;
            }
            else {
                pos++;
            }
        }
    }

    void pushPacketToWebRTC(AVPacket* packet) {
        std::wcout << L"[WebRTC] Pushing encoded packets (PTS: " << packet->pts << L") to WebRTC module.\n";
        {
            std::lock_guard<std::mutex> lock(g_frameMutex);
            g_latestFrameData.assign(packet->data, packet->data + packet->size);
            g_latestPTS = packet->pts;
            g_frameReady = true;
        }
        g_frameAvailable.notify_one();

        std::wcout << L"[WebRTC] Packet Size: " << packet->size << L", First 20 Bytes: ";
        for (int i = 0; i < std::min(20, packet->size); i++) {
            //std::wcout << std::hex << (int)packet->data[i] << L" ";
        }
        //std::wcout << std::dec << L"\n";

        logNALUnits(packet->data, packet->size);

        int result = sendVideoPacket(packet->data, packet->size, packet->pts);
        if (result != 0) {
            std::wcerr << L"[WebRTC] Failed to send video packet to WebRTC module. Error code: " << result << L"\n";
        }
    }

    void InitializeEncoder(const std::string& fileName, int width, int height, int fps) {
    std::lock_guard<std::mutex> lock(g_encoderMutex);

    UINT vendorId = GetGpuVendorId();
    std::string encoderName;
    bool isHardware = true;
    AVHWDeviceType hwDeviceType;
    AVPixelFormat hwPixFmt;

    switch (vendorId) {
    case 0x10DE: // NVIDIA
        encoderName = "h264_nvenc";
        hwDeviceType = AV_HWDEVICE_TYPE_CUDA;
        hwPixFmt = AV_PIX_FMT_CUDA;
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
    codecCtx->time_base = AVRational{ 1, fps };
    codecCtx->framerate = { fps, 1 };
    codecCtx->gop_size = 10;
    codecCtx->max_b_frames = 0;
    codecCtx->bit_rate = 30000000;

    if (isHardware) {
        codecCtx->pix_fmt = hwPixFmt;

        if (av_hwdevice_ctx_create(&hwDeviceCtx, hwDeviceType, nullptr, nullptr, 0) < 0) {
            std::cerr << "[Encoder] Failed to create HW device context." << std::endl;
            return;
        }

        if (hwDeviceType == AV_HWDEVICE_TYPE_D3D11VA) {
            AVHWDeviceContext* deviceCtx = (AVHWDeviceContext*)hwDeviceCtx->data;
            AVD3D11VADeviceContext* d3d11vaDeviceCtx = (AVD3D11VADeviceContext*)deviceCtx->hwctx;
            d3d11vaDeviceCtx->device = (ID3D11Device*)GetD3DDevice().get();
            std::wcout << L"[Encoder] Passed existing D3D11 device to FFmpeg." << std::endl;
        }

        codecCtx->hw_device_ctx = av_buffer_ref(hwDeviceCtx);

        hwFramesCtx = av_hwframe_ctx_alloc(hwDeviceCtx);
        if (!hwFramesCtx) {
            std::cerr << "[Encoder] Failed to allocate hardware frames context." << std::endl;
            return;
        }

        AVHWFramesContext* framesCtx = (AVHWFramesContext*)hwFramesCtx->data;
        framesCtx->format = hwPixFmt;
        framesCtx->sw_format = AV_PIX_FMT_YUV420P;
        framesCtx->width = width;
        framesCtx->height = height;
        framesCtx->initial_pool_size = 20;

        if (av_hwframe_ctx_init(hwFramesCtx) < 0) {
            std::cerr << "[Encoder] Failed to initialize hardware frames context." << std::endl;
            return;
        }

        codecCtx->hw_frames_ctx = av_buffer_ref(hwFramesCtx);

        hwFrame = av_frame_alloc();
        hwFrame->format = hwPixFmt;
        hwFrame->width = width;
        hwFrame->height = height;
        if (av_hwframe_get_buffer(codecCtx->hw_frames_ctx, hwFrame, 0) < 0) {
            std::cerr << "[Encoder] Failed to allocate hardware frame buffer." << std::endl;
            return;
        }
    }
    else { // Software encoder
        codecCtx->pix_fmt = AV_PIX_FMT_YUV420P;
    }

    AVDictionary* opts = nullptr;
    if (encoderName == "h264_nvenc") {
        av_dict_set(&opts, "preset", "p7", 0);
        av_dict_set(&opts, "rc", "vbr", 0);
    }
    else if (encoderName == "libx264") {
        av_dict_set(&opts, "preset", "ultrafast", 0);
        av_dict_set(&opts, "tune", "zerolatency", 0);
    }
    av_dict_set(&opts, "zerolatency", "1", 0);


    if (avcodec_open2(codecCtx, codec, &opts) < 0) {
        std::cerr << "[Encoder] Failed to open codec." << std::endl;
        av_dict_free(&opts);
        return;
    }
    av_dict_free(&opts);

    videoStream = avformat_new_stream(formatCtx, codec);
    avcodec_parameters_from_context(videoStream->codecpar, codecCtx);

    packet = av_packet_alloc();

    std::wcout << L"[Encoder] " << std::wstring(encoderName.begin(), encoderName.end()) << " encoder initialized successfully." << std::endl;
}

void EncodeAndPushFrame() {
    try {
        if (!nv12Frame) {
            std::cerr << "[Encoder] Invalid frame (nv12Frame is NULL).\n";
            return;
        }

        if (!codecCtx) {
            std::cerr << "[Encoder] Codec context not initialized.\n";
            return;
        }

        if (isFirstFrame) {
            encoderStartTime = std::chrono::steady_clock::now();
            isFirstFrame = false;
        }

        auto currentTime = std::chrono::steady_clock::now();
        auto elapsedTimeUS = std::chrono::duration_cast<std::chrono::microseconds>(currentTime - encoderStartTime).count();
        int64_t frameDurationUS = 1000000 / codecCtx->framerate.num;
        nv12Frame->pts = (elapsedTimeUS / frameDurationUS);

        AVFrame* frameToSend = nv12Frame;

        // If using hardware encoding, upload the frame to the GPU
        if (hwDeviceCtx) {
            if (av_hwframe_transfer_data(hwFrame, nv12Frame, 0) < 0) {
                std::cerr << "[Encoder] Failed to transfer frame to GPU." << std::endl;
                return;
            }
            hwFrame->pts = nv12Frame->pts;
            frameToSend = hwFrame;
        }

        int ret = avcodec_send_frame(codecCtx, frameToSend);
        if (ret < 0) {
            char errBuf[128];
            av_make_error_string(errBuf, 128, ret);
            std::cerr << "[Encoder] Failed to send frame to encoder: " << errBuf << "\n";
            return;
        }

        while (avcodec_receive_packet(codecCtx, packet) == 0) {
            packet->stream_index = videoStream->index;
            av_packet_rescale_ts(packet, codecCtx->time_base, videoStream->time_base);

            if (packet->dts < last_dts) {
                packet->dts = last_dts + 1;
            }
            last_dts = packet->dts;

            Packet p;
            p.data.assign(packet->data, packet->data + packet->size);
            p.pts = packet->pts;
            g_packetQueue.push(p);

            av_packet_unref(packet);
        }
        frameCounter++;
    }
    catch (const std::exception& e) {
        std::cerr << "[EXCEPTION] EncodeAndPushFrame() - Exception caught: " << e.what() << "\n";
    }
    catch (...) {
        std::cerr << "[EXCEPTION] EncodeAndPushFrame() - Unknown exception caught!\n";
    }
}

void EncodeFrame(ID3D11Texture2D* texture, ID3D11DeviceContext* context, int width, int height) {
    D3D11_TEXTURE2D_DESC desc;
    texture->GetDesc(&desc);

    winrt::com_ptr<ID3D11Texture2D> stagingTexture;
    D3D11_TEXTURE2D_DESC stagingDesc = desc;
    stagingDesc.Usage = D3D11_USAGE_STAGING;
    stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    stagingDesc.BindFlags = 0;
    stagingDesc.MiscFlags = 0;

    winrt::com_ptr<ID3D11Device> device;
    context->GetDevice(device.put());
    device->CreateTexture2D(&stagingDesc, nullptr, stagingTexture.put());

    context->CopyResource(stagingTexture.get(), texture);

    D3D11_MAPPED_SUBRESOURCE mappedResource;
    context->Map(stagingTexture.get(), 0, D3D11_MAP_READ, 0, &mappedResource);

    ConvertFrame(static_cast<const uint8_t*>(mappedResource.pData), mappedResource.RowPitch, width, height);

    context->Unmap(stagingTexture.get(), 0);
}

    void FlushEncoder() {
        std::wcout << L"[DEBUG] Flushing encoder\n";
        int ret = avcodec_send_frame(codecCtx, nullptr);
        if (ret < 0) {
            char errBuf[128];
            av_make_error_string(errBuf, 128, ret);
            std::cerr << "[Encoder] Failed to flush encoder: " << errBuf << "\n";
            return;
        }

        while (avcodec_receive_packet(codecCtx, packet) == 0) {
            packet->stream_index = videoStream->index;
            av_packet_rescale_ts(packet, codecCtx->time_base, videoStream->time_base);

            if (packet->dts < last_dts) {
                packet->dts = last_dts + 1;
            }
            last_dts = packet->dts;

            /*std::wcout << L"[DEBUG] Flushing packet: size=" << packet->size << L", PTS=" << packet->pts
                << L", DTS=" << packet->dts << L", flags=" << packet->flags << L"\n";*/

            if (packet->size > 0) {
                int offset = 0;
                while (offset < packet->size) {
                    if (offset + 2 < packet->size && packet->data[offset] == 0 && packet->data[offset + 1] == 0) {
                        if (offset + 3 < packet->size && packet->data[offset + 2] == 0 && packet->data[offset + 3] == 1) {
                            offset += 4;
                        }
                        else if (packet->data[offset + 2] == 1) {
                            offset += 3;
                        }
                        else {
                            offset++;
                            continue;
                        }

                        if (offset < packet->size) {
                            uint8_t nal_unit_type = packet->data[offset] & 0x1F;
                            int nal_size = 0;
                            int next_start = offset + 1;
                            while (next_start + 2 < packet->size) {
                                if (packet->data[next_start] == 0 && packet->data[next_start + 1] == 0 &&
                                    (next_start + 3 < packet->size && packet->data[next_start + 2] == 0 && packet->data[next_start + 3] == 1)) {
                                    break;
                                }
                                next_start++;
                            }
                            nal_size = next_start - offset;
                            if (next_start == packet->size) {
                                nal_size = packet->size - offset;
                            }

                            /*std::wcout << L"[DEBUG] Flushing NAL unit type: " << static_cast<int>(nal_unit_type)
                                << L", size: " << nal_size << L" bytes\n";*/
                            offset += nal_size;
                        }
                    }
                    else {
                        offset++;
                    }
                }
            }

            static std::ofstream h264File("debug_stream.h264", std::ios::binary | std::ios::app);
            if (h264File.is_open()) {
                h264File.write((char*)packet->data, packet->size);
                h264File.flush();
            }

            av_packet_unref(packet);
        }
        //std::wcout << L"[DEBUG] NVENC encoder flush complete\n";
    }

    void ConvertFrame(const uint8_t* bgraData, int bgraPitch, int width, int height) {
        std::lock_guard<std::mutex> lock(g_encoderMutex);
        std::wcout << L"[CONV_DEBUG] width=" << width << L", height=" << height
            << L", bgraPitch=" << bgraPitch << L", expected stride=" << (width * 4) << L"\n";

        if (!bgraData) {
            std::wcerr << L"[CONV_DEBUG] bgraData is NULL\n";
            return;
        }

        if (!swsCtx || width != currentWidth || height != currentHeight) {
            if (swsCtx) sws_freeContext(swsCtx);
            if (nv12Frame) av_frame_free(&nv12Frame);

            swsCtx = sws_getContext(width, height, AV_PIX_FMT_BGRA, width, height, AV_PIX_FMT_YUV420P,
                SWS_BICUBIC, nullptr, nullptr, nullptr);
            if (!swsCtx) {
                std::cerr << "[Encoder] Failed to create swsCtx\n";
                return;
            }

            nv12Frame = av_frame_alloc();
            if (!nv12Frame) {
                std::cerr << "[Encoder] Failed to allocate nv12Frame\n";
                return;
            }

            nv12Frame->format = AV_PIX_FMT_YUV420P;
            nv12Frame->width = width;
            nv12Frame->height = height;
            if (av_frame_get_buffer(nv12Frame, 32) < 0) {
                std::cerr << "[Encoder] Failed to allocate nv12Frame buffer\n";
                return;
            }

            nv12Frame->linesize[0] = width;
            nv12Frame->linesize[1] = width / 2;
            nv12Frame->linesize[2] = width / 2;

            currentWidth = width;
            currentHeight = height;
            /*std::wcout << L"[CONV_DEBUG] SWS context created: input=" << width << L"x" << height
                << L", output=" << width << L"x" << height << L"\n";*/
        }

        uint8_t* inData[4] = { const_cast<uint8_t*>(bgraData), nullptr, nullptr, nullptr };
        int inLineSize[4] = { bgraPitch, 0, 0, 0 };
        if (av_frame_make_writable(nv12Frame) < 0) {
            std::cerr << "[Encoder] Failed to make nv12Frame writable\n";
            return;
        }

        sws_scale(swsCtx, inData, inLineSize, 0, height, nv12Frame->data, nv12Frame->linesize);
        EncodeAndPushFrame();
    }

    void FinalizeEncoder() {
        std::lock_guard<std::mutex> lock(g_encoderMutex);
        std::wcout << L"[DEBUG] Finalizing encoder...\n";

        avcodec_send_frame(codecCtx, nullptr);
        while (avcodec_receive_packet(codecCtx, packet) == 0) {
            packet->stream_index = videoStream->index;
            av_packet_rescale_ts(packet, codecCtx->time_base, videoStream->time_base);

            if (packet->dts < packet->pts) {
                packet->dts = packet->pts;
            }

            av_interleaved_write_frame(formatCtx, packet);
            av_packet_unref(packet);
        }

        av_frame_free(&hwFrame);
        avcodec_free_context(&codecCtx);
        avformat_free_context(formatCtx);
        av_packet_free(&packet);
        av_buffer_unref(&hwFramesCtx);
        av_buffer_unref(&hwDeviceCtx);
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