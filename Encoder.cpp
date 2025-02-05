#pragma once
#include "Encoder.h"


static SwsContext* swsCtx = nullptr;
static AVFrame* nv12Frame = nullptr;
static int currentWidth = 0;
static int currentHeight = 0;

static std::mutex g_encoderMutex;

static AVFormatContext* formatCtx = nullptr;
static AVCodecContext* codecCtx = nullptr;
static AVStream* videoStream = nullptr;
static AVPacket* packet = nullptr;
static AVFrame* hwFrame = nullptr;
static AVBufferRef* hwDeviceCtx = nullptr;
static int frameCounter = 0;

#define ENCODER "h264_nvenc"



namespace Encoder{
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

        codecCtx->width = width;
        codecCtx->height = height;
        codecCtx->time_base = AVRational({ 1, fps });
		videoStream->time_base = codecCtx->time_base;
        codecCtx->framerate = { fps, 1 };
        codecCtx->gop_size = 10;
        codecCtx->max_b_frames = 0;
        codecCtx->pix_fmt = AV_PIX_FMT_CUDA;  //  Use CUDA pixel format
        codecCtx->bit_rate = 4000000;

        std::wcout << L"[DEBUG] Setting up hardware context\n";

        if (av_hwdevice_ctx_create(&hwDeviceCtx, AV_HWDEVICE_TYPE_CUDA, nullptr, nullptr, 0) < 0) {
            std::cerr << "[Encoder] Failed to create hardware device context. NVENC might not be supported on your system.\n";
            return;
        }

        std::wcout << L"[DEBUG] Hardware device context created successfully\n";

        //  Create and Initialize Hardware Frames Context Properly
        codecCtx->hw_frames_ctx = av_hwframe_ctx_alloc(hwDeviceCtx);
        if (!codecCtx->hw_frames_ctx) {
            std::cerr << "[Encoder] Failed to allocate hardware frames context!\n";
            return;
        }

        AVHWFramesContext* framesCtx = (AVHWFramesContext*)codecCtx->hw_frames_ctx->data;
        framesCtx->format = AV_PIX_FMT_CUDA;  //  Use CUDA!
        framesCtx->sw_format = AV_PIX_FMT_NV12;  //  Software format is still NV12
        framesCtx->width = width;
        framesCtx->height = height;
        framesCtx->initial_pool_size = 20; // Preallocate GPU frames

        //  Add Constraints for NVENC
        framesCtx->device_ref = av_buffer_ref(hwDeviceCtx);
        //framesCtx->data_align = 32;  // Align memory for better GPU performance

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

        if (!(formatCtx->oformat->flags & AVFMT_NOFILE)) {
            if (avio_open(&formatCtx->pb, fileName.c_str(), AVIO_FLAG_WRITE) < 0) {
                std::cerr << "[Encoder] Failed to open output file.\n";
                return;
            }
        }

        std::wcout << L"[DEBUG] Output file opened\n";

        if (avformat_write_header(formatCtx, nullptr) < 0) {
            std::cerr << "[Encoder] Failed to write header.\n";
            return;
        }

        std::wcout << L"[DEBUG] Format header written\n";

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

            // ✅ Ensure PTS increases correctly based on frame rate
            nv12Frame->pts = frameCounter * codecCtx->time_base.den / codecCtx->time_base.num / codecCtx->framerate.num;

            std::wcout << L"[DEBUG] Allocating GPU frame\n";
            AVFrame* hwFrame = av_frame_alloc();
            if (!hwFrame) {
                std::cerr << "[Encoder] Failed to allocate hwFrame.\n";
                return;
            }

            hwFrame->format = AV_PIX_FMT_CUDA;
            hwFrame->width = codecCtx->width;
            hwFrame->height = codecCtx->height;
            hwFrame->pts = nv12Frame->pts;  // ✅ Ensure correct PTS assignment

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

            std::wcout << L"[DEBUG] Sending frame to encoder\n";
            if (avcodec_send_frame(codecCtx, hwFrame) < 0) {
                std::cerr << "[Encoder] Failed to send frame to encoder.\n";
                return;
            }

            while (avcodec_receive_packet(codecCtx, packet) == 0) {
                packet->stream_index = videoStream->index;
                av_packet_rescale_ts(packet, codecCtx->time_base, videoStream->time_base);  // ✅ Rescale timestamps

                if (av_interleaved_write_frame(formatCtx, packet) < 0) {
                    std::cerr << "[Encoder] Failed to write frame.\n";
                    return;
                }
                av_packet_unref(packet);
            }
            av_frame_free(&hwFrame);
            frameCounter++;  // ✅ Increment frame count properly
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


        // 1) Re-create SWS context if resolution changed or not yet set
        if (!swsCtx || width != currentWidth || height != currentHeight)
        {
            std::wcout << L"[CONV_DEBUG] ConvertFrame() - Recreating SWS context\n";

            // Free old
            if (swsCtx) {
                sws_freeContext(swsCtx);
                swsCtx = nullptr;
            }
            if (nv12Frame) {
                av_frame_free(&nv12Frame);
            }

            // Create new context
            swsCtx = sws_getContext(
                width, height, AV_PIX_FMT_BGRA,  // Input is BGRA
                width, height, AV_PIX_FMT_NV12,  // Output is NV12
                SWS_BILINEAR, nullptr, nullptr, nullptr
            );
            if (!swsCtx) {
                std::cerr << "[Encoder] Failed to create swsCtx.\n";
                return;
            }

            // 2) Create NV12 frame
            nv12Frame = av_frame_alloc();
            nv12Frame->format = AV_PIX_FMT_NV12;
            nv12Frame->width = width;
            nv12Frame->height = height;

            int ret = av_frame_get_buffer(nv12Frame, 32); // 32 alignment
            if (ret < 0) {
                std::cerr << "[Encoder] Failed to allocate nv12Frame buffer.\n";
                return;
            }

            currentWidth = width;
            currentHeight = height;
            std::wcout << L"[CONV_DEBUG] ConvertFrame() - Performing sws_scale\n";

        }

        // 3) Prepare input arrays for sws_scale
        uint8_t* inData[4] = { const_cast<uint8_t*>(bgraData), nullptr, nullptr, nullptr };
        int      inLineSize[4] = { bgraPitch, 0, 0, 0 };

        // 4) Make sure output frame is writable
        if (av_frame_make_writable(nv12Frame) < 0) {
            std::cerr << "[Encoder] Failed to make nv12Frame writable.\n";
            return;
        }

        // 5) Perform sws_scale from BGRA -> NV12
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


        // Now nv12Frame->data[0] = Y plane, data[1] = interleaved UV

        // 6) TODO: Use the NV12 data...
        //     - e.g., feed it into your hardware/software encoder
        //     - or store it somewhere, etc.
        std::cout << "[Encoder] Converted frame to NV12 ("
            << width << "x" << height << ")\n";
    }

    void FinalizeEncoder() {
        std::lock_guard<std::mutex> lock(g_encoderMutex);
        std::wcout << L"[DEBUG] Finalizing encoder...\n";

        // ✅ Flush any remaining frames
        avcodec_send_frame(codecCtx, nullptr);
        while (avcodec_receive_packet(codecCtx, packet) == 0) {
            av_interleaved_write_frame(formatCtx, packet);
            av_packet_unref(packet);
        }

        av_write_trailer(formatCtx);  // ✅ Ensure MP4 file is finalized

        avio_closep(&formatCtx->pb);
        avcodec_free_context(&codecCtx);
        avformat_free_context(formatCtx);
        av_packet_free(&packet);
        av_buffer_unref(&hwDeviceCtx);

        std::wcout << L"[Encoder] Encoder finalized.\n";
    }


}
