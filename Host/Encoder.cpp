#include "Encoder.h"
#include "GlobalTime.h"
#include "pion_webrtc.h"
#include <functional>
#include <mutex>
#include <condition_variable>
#include <fstream>
#include <chrono>

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

#define ENCODER "h264_nvenc"

Encoder::EncodedFrameCallback Encoder::g_onEncodedFrameCallback = nullptr;

std::mutex Encoder::g_frameMutex;
std::condition_variable Encoder::g_frameAvailable;
std::vector<uint8_t> Encoder::g_latestFrameData;
int64_t Encoder::g_latestPTS = 0;
bool Encoder::g_frameReady = false;

std::ofstream debugH264File;

static std::chrono::steady_clock::time_point encoderStartTime;
static bool isFirstFrame = true;

namespace Encoder {
    void setEncodedFrameCallback(EncodedFrameCallback callback) {
        g_onEncodedFrameCallback = callback;
    }

    bool getEncodedFrame(std::vector<uint8_t>& frameData, int64_t& pts) {
        std::unique_lock<std::mutex> lock(g_frameMutex);
        if (!g_frameReady) {
            g_frameAvailable.wait(lock, [] { return g_frameReady; });
        }
        if (g_latestFrameData.empty()) {
            return false;
        }
        frameData = g_latestFrameData;
        pts = g_latestPTS;
        g_frameReady = false;
        lock.unlock();
        g_frameAvailable.notify_one();
        return true;
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
            g_latestPTS = packet->pts;
            g_frameReady = true;
        }
        g_frameAvailable.notify_one();

        //std::wcout << L"[WebRTC] Packet Size: " << packet->size << L", First 20 Bytes: ";
        for (int i = 0; i < std::min(20, packet->size); i++) {
            //std::wcout << std::hex << (int)packet->data[i] << L" ";
        }
        //std::wcout << std::dec << L"\n";

        logNALUnits(packet->data, packet->size);

        /*if (debugH264File.is_open()) {
            debugH264File.write(reinterpret_cast<const char*>(packet->data), packet->size);
        }*/

        int result = sendVideoPacket(packet->data, packet->size, packet->pts);
        if (result != 0) {
            //std::wcerr << L"[WebRTC] Failed to send video packet to WebRTC module. Error code: " << result << L"\n";
        }
    }

    void InitializeEncoder(const std::string& fileName, int width, int height, int fps) {
        std::lock_guard<std::mutex> lock(g_encoderMutex);
        std::wcout << L"[DEBUG] Initializing NVENC encoder with width=" << width << L", height=" << height << L", fps=" << fps << L"\n";

        /*debugH264File.open("debug_h264_stream.h264", std::ios::binary);
        if (!debugH264File.is_open()) {
            std::wcerr << L"[DEBUG] Failed to open debug_h264_stream.h264 for writing\n";
        }*/

        // Initialize hardware device context for NVENC
        if (av_hwdevice_ctx_create(&hwDeviceCtx, AV_HWDEVICE_TYPE_CUDA, nullptr, nullptr, 0) < 0) {
            std::cerr << "[Encoder] Failed to create CUDA device context for NVENC.\n";
            return;
        }
        std::wcout << L"[DEBUG] CUDA device context created\n";

        avformat_alloc_output_context2(&formatCtx, nullptr, "mp4", fileName.c_str());
        if (!formatCtx) {
            std::cerr << "[Encoder] Failed to create output context.\n";
            av_buffer_unref(&hwDeviceCtx);
            return;
        }

        std::wcout << L"[DEBUG] Allocated output context\n";

        const AVCodec* codec = avcodec_find_encoder_by_name(ENCODER);
        if (!codec) {
            std::cerr << "[Encoder] Failed to find encoder: " << ENCODER << "\n";
            av_buffer_unref(&hwDeviceCtx);
            return;
        }

        std::wcout << L"[DEBUG] Found encoder: " << ENCODER << "\n";

        videoStream = avformat_new_stream(formatCtx, codec);
        if (!videoStream) {
            std::cerr << "[Encoder] Failed to create new stream.\n";
            av_buffer_unref(&hwDeviceCtx);
            return;
        }

        std::wcout << L"[DEBUG] Created video stream\n";

        codecCtx = avcodec_alloc_context3(codec);
        if (!codecCtx) {
            std::cerr << "[Encoder] Failed to allocate codec context.\n";
            av_buffer_unref(&hwDeviceCtx);
            return;
        }

        std::wcout << L"[DEBUG] Allocated codec context\n";

        codecCtx->width = width;
        codecCtx->height = height;
        codecCtx->time_base = AVRational{ 1, fps };
        videoStream->time_base = codecCtx->time_base;
        codecCtx->framerate = { fps, 1 };
        codecCtx->gop_size = 10;
        codecCtx->max_b_frames = 0;
        codecCtx->pix_fmt = AV_PIX_FMT_CUDA;
        codecCtx->bit_rate = 30000000;


		codecCtx->profile = FF_PROFILE_H264_MAIN;
        //codecCtx->profile = FF_PROFILE_H264_BASELINE;
		//codecCtx->profile = FF_PROFILE_H264_HIGH;
        codecCtx->level = 52;

        // Set hardware device context
        codecCtx->hw_device_ctx = av_buffer_ref(hwDeviceCtx);
        if (!codecCtx->hw_device_ctx) {
            std::cerr << "[Encoder] Failed to set hardware device context.\n";
            avcodec_free_context(&codecCtx);
            av_buffer_unref(&hwDeviceCtx);
            return;
        }

        // Initialize hardware frames context
        hwFramesCtx = av_hwframe_ctx_alloc(hwDeviceCtx);
        if (!hwFramesCtx) {
            std::cerr << "[Encoder] Failed to allocate hardware frames context.\n";
            avcodec_free_context(&codecCtx);
            av_buffer_unref(&hwDeviceCtx);
            return;
        }

        AVHWFramesContext* framesCtx = (AVHWFramesContext*)hwFramesCtx->data;
        framesCtx->format = AV_PIX_FMT_CUDA;
        framesCtx->sw_format = AV_PIX_FMT_YUV420P;
        framesCtx->width = width;
        framesCtx->height = height;
        framesCtx->initial_pool_size = 40; // Number of frames to pre-allocate

        if (av_hwframe_ctx_init(hwFramesCtx) < 0) {
            std::cerr << "[Encoder] Failed to initialize hardware frames context.\n";
            av_buffer_unref(&hwFramesCtx);
            avcodec_free_context(&codecCtx);
            av_buffer_unref(&hwDeviceCtx);
            return;
        }
        std::wcout << L"[DEBUG] Hardware frames context initialized\n";

        codecCtx->hw_frames_ctx = av_buffer_ref(hwFramesCtx);
        if (!codecCtx->hw_frames_ctx) {
            std::cerr << "[Encoder] Failed to set hardware frames context.\n";
            av_buffer_unref(&hwFramesCtx);
            avcodec_free_context(&codecCtx);
            av_buffer_unref(&hwDeviceCtx);
            return;
        }

        // NVENC options
        AVDictionary* opts = nullptr;
        av_dict_set(&opts, "preset", "p7", 0);
        av_dict_set(&opts, "rc", "vbr", 0);
        av_dict_set(&opts, "zerolatency", "1", 0);
        av_dict_set(&opts, "level", "52", 0);

        if (avcodec_open2(codecCtx, codec, &opts) < 0) {
            std::cerr << "[Encoder] Failed to open NVENC codec.\n";
            av_dict_free(&opts);
            av_buffer_unref(&hwFramesCtx);
            av_buffer_unref(&hwDeviceCtx);
            return;
        }
        av_dict_free(&opts);

        std::wcout << L"[DEBUG] NVENC codec opened successfully\n";

        avcodec_parameters_from_context(videoStream->codecpar, codecCtx);

        packet = av_packet_alloc();
        if (!packet) {
            std::cerr << "[Encoder] Failed to allocate packet.\n";
            av_buffer_unref(&hwFramesCtx);
            av_buffer_unref(&hwDeviceCtx);
            return;
        }

        // Allocate hardware frame for NVENC
        hwFrame = av_frame_alloc();
        if (!hwFrame) {
            std::cerr << "[Encoder] Failed to allocate hardware frame.\n";
            av_packet_free(&packet);
            av_buffer_unref(&hwFramesCtx);
            av_buffer_unref(&hwDeviceCtx);
            return;
        }
        hwFrame->format = AV_PIX_FMT_CUDA;
        hwFrame->width = width;
        hwFrame->height = height;
        if (av_hwframe_get_buffer(codecCtx->hw_frames_ctx, hwFrame, 0) < 0) {
            std::cerr << "[Encoder] Failed to allocate hardware frame buffer.\n";
            av_frame_free(&hwFrame);
            av_packet_free(&packet);
            av_buffer_unref(&hwFramesCtx);
            av_buffer_unref(&hwDeviceCtx);
            return;
        }

        std::wcout << L"[Encoder] NVENC Encoder Initialized.\n";
    }

    void EncodeFrame() {
        try {
            //std::wcout << L"[DEBUG] EncodeFrame() - Start\n";

            if (!nv12Frame) {
                std::cerr << "[Encoder] Invalid frame (nv12Frame is NULL).\n";
                return;
            }

            if (!codecCtx) {
                std::cerr << "[Encoder] Codec context not initialized.\n";
                return;
            }

            if (nv12Frame->width != codecCtx->width || nv12Frame->height != codecCtx->height) {
                std::cerr << "[Encoder] Frame dimensions mismatch: expected "
                    << codecCtx->width << "x" << codecCtx->height
                    << ", got " << nv12Frame->width << "x" << nv12Frame->height << "\n";
                return;
            }

            std::wcout << L"[DEBUG] EncodeFrame() - Frame: width=" << nv12Frame->width
                << L", height=" << nv12Frame->height << L", format=" << nv12Frame->format << L"\n";

            if (nv12Frame->format != AV_PIX_FMT_YUV420P) {
                //std::wcout << L"[DEBUG] Fixing frame format to AV_PIX_FMT_YUV420P\n";
                nv12Frame->format = AV_PIX_FMT_YUV420P;
            }

            if (av_frame_make_writable(nv12Frame) < 0) {
                std::cerr << "[Encoder] Failed to make nv12Frame writable.\n";
                return;
            }

            if (isFirstFrame) {
                encoderStartTime = std::chrono::steady_clock::now();
                isFirstFrame = false;
            }

            auto currentTime = std::chrono::steady_clock::now();
            auto elapsedTimeUS = std::chrono::duration_cast<std::chrono::microseconds>(currentTime - encoderStartTime).count();
            int64_t frameDurationUS = 1000000 / codecCtx->framerate.num;
            int64_t pts = (elapsedTimeUS / frameDurationUS) * frameDurationUS;
            nv12Frame->pts = pts;

            // Transfer CPU frame (YUV420P) to GPU (CUDA)
            if (av_hwframe_transfer_data(hwFrame, nv12Frame, 0) < 0) {
                std::cerr << "[Encoder] Failed to transfer frame to GPU.\n";
                return;
            }
            hwFrame->pts = nv12Frame->pts;

            if (frameCounter == 0 || frameCounter % codecCtx->gop_size == 0) {
                hwFrame->pict_type = AV_PICTURE_TYPE_I;
                //std::wcout << L"[DEBUG] Forcing KeyFrame at frame " << frameCounter << L"\n";
            }

            //std::wcout << L"[DEBUG] Sending frame to NVENC encoder, PTS=" << hwFrame->pts << L"\n";
            int ret = avcodec_send_frame(codecCtx, hwFrame);
            if (ret < 0) {
                char errBuf[128];
                av_make_error_string(errBuf, 128, ret);
                std::cerr << "[Encoder] Failed to send frame to NVENC encoder: " << errBuf << "\n";
                return;
            }

            while (avcodec_receive_packet(codecCtx, packet) == 0) {
                packet->stream_index = videoStream->index;
                av_packet_rescale_ts(packet, codecCtx->time_base, videoStream->time_base);

                if (packet->dts < last_dts) {
                    //std::wcout << L"[DEBUG] Adjusting DTS: " << packet->dts << L" to " << (last_dts + 1) << L"\n";
                    packet->dts = last_dts + 1;
                }
                last_dts = packet->dts;

                /*std::wcout << L"[DEBUG] Encoded packet: size=" << packet->size << L", PTS=" << packet->pts
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
                                        (packet->data[next_start + 2] == 1 || (next_start + 3 < packet->size && packet->data[next_start + 2] == 0 && packet->data[next_start + 3] == 1))) {
                                        break;
                                    }
                                    next_start++;
                                }
                                nal_size = next_start - offset;
                                if (next_start == packet->size) {
                                    nal_size = packet->size - offset;
                                }
                                /*std::wcout << L"[DEBUG] NAL unit type: " << static_cast<int>(nal_unit_type)
                                    << L", size: " << nal_size << L" bytes\n";*/
                                offset += nal_size;
                            }
                        }
                        else {
                            offset++;
                        }
                    }
                }

               /* static std::ofstream h264File("debug_stream.h264", std::ios::binary | std::ios::app);
                if (h264File.is_open()) {
                    h264File.write((char*)packet->data, packet->size);
                    h264File.flush();
                }*/

                if (g_onEncodedFrameCallback) {
                    g_onEncodedFrameCallback(packet);
                }
                else {
                    pushPacketToWebRTC(packet);
                }

                av_packet_unref(packet);
            }
            frameCounter++;
            /*std::wcout << L"[DEBUG] Frame encoded successfully. Frame Counter: " << frameCounter
                << L", PTS: " << hwFrame->pts << L"\n";*/
        }
        catch (const std::exception& e) {
            std::cerr << "[EXCEPTION] EncodeFrame() - Exception caught: " << e.what() << "\n";
        }
        catch (...) {
            std::cerr << "[EXCEPTION] EncodeFrame() - Unknown exception caught!\n";
        }
    }

    void FlushEncoder() {
        std::wcout << L"[DEBUG] Flushing NVENC encoder\n";
        int ret = avcodec_send_frame(codecCtx, nullptr);
        if (ret < 0) {
            char errBuf[128];
            av_make_error_string(errBuf, 128, ret);
            std::cerr << "[Encoder] Failed to flush NVENC encoder: " << errBuf << "\n";
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

        /*std::ofstream yuvOut("debug_yuv420p.raw", std::ios::binary | std::ios::app);
        for (int i = 0; i < height; i++) {
            yuvOut.write(reinterpret_cast<char*>(nv12Frame->data[0] + i * nv12Frame->linesize[0]), width);
        }
        for (int i = 0; i < height / 2; i++) {
            yuvOut.write(reinterpret_cast<char*>(nv12Frame->data[1] + i * nv12Frame->linesize[1]), width / 2);
        }
        for (int i = 0; i < height / 2; i++) {
            yuvOut.write(reinterpret_cast<char*>(nv12Frame->data[2] + i * nv12Frame->linesize[2]), width / 2);
        }
        yuvOut.close();
        std::wcout << L"[DEBUG] Saved YUV420P frame to debug_yuv420p.raw\n";*/

       /* std::ofstream bgraFile("debug_bgra.raw", std::ios::binary | std::ios::app);
        for (int i = 0; i < height; i++) {
            bgraFile.write(reinterpret_cast<char*>(const_cast<uint8_t*>(bgraData) + i * bgraPitch), width * 4);
        }
        bgraFile.close();
        std::wcout << L"[DEBUG] Saved BGRA input to debug_bgra.raw\n";*/

        EncodeFrame();
    }

    void FinalizeEncoder() {
        std::lock_guard<std::mutex> lock(g_encoderMutex);
        std::wcout << L"[DEBUG] Finalizing NVENC encoder...\n";

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

        if (debugH264File.is_open()) {
            debugH264File.close();
        }

        std::wcout << L"[Encoder] NVENC Encoder finalized.\n";
    }
}