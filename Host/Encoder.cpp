#include "Encoder.h"
#include "GlobalTime.h"
#include "pion_webrtc.h"
#include <functional>
#include <mutex>
#include <condition_variable>

// Define variables in the Encoder namespace
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
int Encoder::frameCounter = 0;
int64_t Encoder::last_dts = 0;

#define ENCODER "h264_nvenc"

// Define callback variable in Encoder namespace
Encoder::EncodedFrameCallback Encoder::g_onEncodedFrameCallback = nullptr;

// New variables for storing the latest encoded frame
std::mutex Encoder::g_frameMutex;
std::condition_variable Encoder::g_frameAvailable;
std::vector<uint8_t> Encoder::g_latestFrameData;
int64_t Encoder::g_latestPTS = 0;
bool Encoder::g_frameReady = false;

namespace Encoder {
    void setEncodedFrameCallback(EncodedFrameCallback callback) {
        g_onEncodedFrameCallback = callback;
    }

    // Function to get the latest encoded frame
    bool getEncodedFrame(std::vector<uint8_t>& frameData, int64_t& pts) {
        std::unique_lock<std::mutex> lock(g_frameMutex);
        if (!g_frameReady) {
            g_frameAvailable.wait(lock, [] { return g_frameReady; });
        }

        if (g_latestFrameData.empty()) {
            return false;
        }

        frameData = g_latestFrameData; // Copy the data
        pts = g_latestPTS;
        g_frameReady = false; // Mark as consumed
        lock.unlock();
        g_frameAvailable.notify_one(); // Allow new frames to overwrite
        return true;
    }

    // Update pushPacketToWebRTC to store the frame
    void pushPacketToWebRTC(AVPacket* packet) {
        std::wcout << L"[WebRTC] Pushing encoded packets (PTS: " << packet->pts << L") to WebRTC module.\n";
        {
            std::lock_guard<std::mutex> lock(g_frameMutex);
            g_latestFrameData.assign(packet->data, packet->data + packet->size);
            g_latestPTS = packet->pts;
            g_frameReady = true;
        }
        g_frameAvailable.notify_one();

        int result = sendVideoPacket(packet->data, packet->size, packet->pts);
        if (result != 0) {
            std::wcerr << L"[WebRTC] Failed to send video packet to WebRTC module. Error code: " << result << "\n";
        }
    }

    void InitializeEncoder(const std::string& fileName, int width, int height, int fps) {
        std::lock_guard<std::mutex> lock(g_encoderMutex);
        std::wcout << L"[DEBUG] Initializing encoder\n";

        avformat_alloc_output_context2(&formatCtx, nullptr, "mp4", fileName.c_str());
        if (!formatCtx) {
            std::cerr << "[Encoder] Failed to create output context.\n";
            return;
        }

        std::wcout << L"[DEBUG] Allocated output context\n";

        const AVCodec* codec = avcodec_find_encoder_by_name(ENCODER);
        if (!codec) {
            std::cerr << "[Encoder] Failed to find encoder: " << ENCODER << "\n";
            return;
        }

        std::wcout << L"[DEBUG] Found encoder: " << ENCODER << "\n";

        videoStream = avformat_new_stream(formatCtx, codec);
        if (!videoStream) {
            std::cerr << "[Encoder] Failed to create new stream.\n";
            return;
        }

        std::wcout << L"[DEBUG] Created video stream\n";

        codecCtx = avcodec_alloc_context3(codec);
        if (!codecCtx) {
            std::cerr << "[Encoder] Failed to allocate codec context.\n";
            return;
        }

        std::wcout << L"[DEBUG] Allocated codec context\n";

        codecCtx->width = 1920;
        codecCtx->height = 1080;
        codecCtx->time_base = AVRational{ 1, fps };
        videoStream->time_base = codecCtx->time_base;
        codecCtx->framerate = { fps, 1 };
        codecCtx->gop_size = 10;
        codecCtx->max_b_frames = 0;
        codecCtx->pix_fmt = AV_PIX_FMT_CUDA;  // Use CUDA pixel format
        codecCtx->bit_rate = 4000000;

        std::wcout << L"[DEBUG] Setting up hardware context\n";

        if (av_hwdevice_ctx_create(&hwDeviceCtx, AV_HWDEVICE_TYPE_CUDA, nullptr, nullptr, 0) < 0) {
            std::cerr << "[Encoder] Failed to create hardware device context. NVENC might not be supported on your system.\n";
            return;
        }

        std::wcout << L"[DEBUG] Hardware device context created successfully\n";

        codecCtx->hw_frames_ctx = av_hwframe_ctx_alloc(hwDeviceCtx);
        if (!codecCtx->hw_frames_ctx) {
            std::cerr << "[Encoder] Failed to allocate hardware frames context!\n";
            return;
        }

        AVHWFramesContext* framesCtx = (AVHWFramesContext*)codecCtx->hw_frames_ctx->data;
        framesCtx->format = AV_PIX_FMT_CUDA;
        framesCtx->sw_format = AV_PIX_FMT_NV12;
        framesCtx->width = width;
        framesCtx->height = height;
        framesCtx->initial_pool_size = 20;

        framesCtx->device_ref = av_buffer_ref(hwDeviceCtx);

        if (av_hwframe_ctx_init(codecCtx->hw_frames_ctx) < 0) {
            std::cerr << "[Encoder] Failed to initialize hardware frames context!\n";
            return;
        }

        std::wcout << L"[DEBUG] Hardware frame context initialized successfully\n";

        if (avcodec_open2(codecCtx, codec, nullptr) < 0) {
            std::cerr << "[Encoder] Failed to open codec.\n";
            return;
        }

        std::wcout << L"[DEBUG] Codec opened successfully\n";

        avcodec_parameters_from_context(videoStream->codecpar, codecCtx);

        packet = av_packet_alloc();
        if (!packet) {
            std::cerr << "[Encoder] Failed to allocate packet.\n";
            return;
        }

        std::wcout << L"[Encoder] Hardware Accelerated Encoder Initialized.\n";
    }

    void EncodeFrame() {
        try {
            std::wcout << L"[DEBUG] EncodeFrame() - Start\n";

            if (!nv12Frame) {
                std::cerr << "[Encoder] Invalid frame (nv12Frame is NULL).\n";
                return;
            }

            if (!codecCtx) {
                std::cerr << "[Encoder] Codec context not initialized.\n";
                return;
            }

            if (!codecCtx->hw_frames_ctx) {
                std::cerr << "[Encoder] Hardware frame context is NULL! Exiting.\n";
                return;
            }

            auto now = std::chrono::steady_clock::now();
            auto elapsedUS = std::chrono::duration_cast<std::chrono::microseconds>(now - startTime).count();
            nv12Frame->pts = av_rescale_q(elapsedUS, AVRational{ 1, 1000000 }, codecCtx->time_base);

            std::wcout << L"[DEBUG] Allocating GPU frame\n";
            AVFrame* hwFrame = av_frame_alloc();
            if (!hwFrame) {
                std::cerr << "[Encoder] Failed to allocate hwFrame.\n";
                return;
            }

            hwFrame->format = AV_PIX_FMT_CUDA;
            hwFrame->width = codecCtx->width;
            hwFrame->height = codecCtx->height;
            hwFrame->pts = nv12Frame->pts;

            std::wcout << L"[DEBUG] Allocating hardware buffer\n";
            int ret = av_hwframe_get_buffer(codecCtx->hw_frames_ctx, hwFrame, 0);
            if (ret < 0) {
                std::cerr << "[Encoder] Failed to allocate hardware buffer. FFmpeg Error Code: " << ret << "\n";
                return;
            }

            std::wcout << L"[DEBUG] Uploading NV12 frame to CUDA memory\n";
            ret = av_hwframe_transfer_data(hwFrame, nv12Frame, 0);
            if (ret < 0) {
                std::cerr << "[Encoder] Failed to upload frame to CUDA. FFmpeg Error Code: " << ret << "\n";
                return;
            }

            hwFrame->pts = nv12Frame->pts;

            std::wcout << L"[DEBUG] Sending frame to encoder\n";
            if (avcodec_send_frame(codecCtx, hwFrame) < 0) {
                std::cerr << "[Encoder] Failed to send frame to encoder.\n";
                return;
            }

            while (avcodec_receive_packet(codecCtx, packet) == 0) {
                packet->stream_index = videoStream->index;
                av_packet_rescale_ts(packet, codecCtx->time_base, videoStream->time_base);

                if (packet->dts < last_dts) {
                    packet->dts = last_dts + 1;
                }
                last_dts = packet->dts;

                if (g_onEncodedFrameCallback) {
                    g_onEncodedFrameCallback(packet);
                }
                else {
                    pushPacketToWebRTC(packet);
                }

                av_packet_unref(packet);
            }
            av_frame_free(&hwFrame);
            frameCounter++;
            std::wcout << L"[DEBUG] Frame encoded successfully.\n";
        }
        catch (const std::exception& e) {
            std::cerr << "[EXCEPTION] EncodeFrame() - Exception caught: " << e.what() << "\n";
        }
        catch (...) {
            std::cerr << "[EXCEPTION] EncodeFrame() - Unknown exception caught!\n";
        }
    }

    void ConvertFrame(const uint8_t* bgraData, int bgraPitch, int width, int height) {
        std::lock_guard<std::mutex> lock(g_encoderMutex);
        std::wcout << L"[CONV_DEBUG] ConvertFrame() - Start\n";

        if (!swsCtx || width != currentWidth || height != currentHeight) {
            std::wcout << L"[CONV_DEBUG] ConvertFrame() - Recreating SWS context\n";

            if (swsCtx) {
                sws_freeContext(swsCtx);
                swsCtx = nullptr;
            }
            if (nv12Frame) {
                av_frame_free(&nv12Frame);
            }

            swsCtx = sws_getContext(
                width, height, AV_PIX_FMT_BGRA,
                1920, 1080, AV_PIX_FMT_NV12,
                SWS_BICUBIC, nullptr, nullptr, nullptr
            );
            if (!swsCtx) {
                std::cerr << "[Encoder] Failed to create swsCtx.\n";
                return;
            }

            nv12Frame = av_frame_alloc();
            nv12Frame->format = AV_PIX_FMT_NV12;
            nv12Frame->width = 1920;
            nv12Frame->height = 1080;

            int ret = av_frame_get_buffer(nv12Frame, 32);
            if (ret < 0) {
                std::cerr << "[Encoder] Failed to allocate nv12Frame buffer.\n";
                return;
            }

            currentWidth = width;
            currentHeight = height;
            std::wcout << L"[CONV_DEBUG] ConvertFrame() - Performing sws_scale\n";
        }

        uint8_t* inData[4] = { const_cast<uint8_t*>(bgraData), nullptr, nullptr, nullptr };
        int inLineSize[4] = { bgraPitch, 0, 0, 0 };

        if (av_frame_make_writable(nv12Frame) < 0) {
            std::cerr << "[Encoder] Failed to make nv12Frame writable.\n";
            return;
        }

        sws_scale(
            swsCtx,
            inData,
            inLineSize,
            0,
            height,
            nv12Frame->data,
            nv12Frame->linesize
        );

        std::wcout << L"[CONV_DEBUG] ConvertFrame() - Frame converted successfully\n";

        EncodeFrame();
        std::wcout << L"[CONV_DEBUG] ConvertFrame() - End\n";
        std::cout << "[Encoder] Converted frame to NV12 ("
            << width << "x" << height << ")\n";
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

        avcodec_free_context(&codecCtx);
        avformat_free_context(formatCtx);
        av_packet_free(&packet);
        av_buffer_unref(&hwDeviceCtx);

        std::wcout << L"[Encoder] Encoder finalized.\n";
    }
}