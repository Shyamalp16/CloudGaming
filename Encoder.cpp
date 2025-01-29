#pragma once
#include "Encoder.h"


static SwsContext* swsCtx = nullptr;
static AVFrame* nv12Frame = nullptr;
static int currentWidth = 0;
static int currentHeight = 0;

static std::mutex g_encoderMutex;


namespace Encoder
{
    void ConvertFrame(
        const uint8_t* bgraData,
        int bgraPitch,
        int width,
        int height
    )
    {
        std::lock_guard<std::mutex> lock(g_encoderMutex);

        // 1) Re-create SWS context if resolution changed or not yet set
        if (!swsCtx || width != currentWidth || height != currentHeight)
        {
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

        // Now nv12Frame->data[0] = Y plane, data[1] = interleaved UV

        // 6) TODO: Use the NV12 data...
        //     - e.g., feed it into your hardware/software encoder
        //     - or store it somewhere, etc.
        // For demonstration:
        std::cout << "[Encoder] Converted frame to NV12 ("
            << width << "x" << height << ")\n";
    }
}
