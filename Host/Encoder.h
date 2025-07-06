#pragma once
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>
#include <libavutil/imgutils.h>
}
#include <cstdint>
#include <iostream>
#include <mutex>
#include <vector>
#include <condition_variable>
#include <functional>

namespace Encoder {
    // Function declarations
    void ConvertFrame(
        const uint8_t* bgraData,
        int bgraPitch,
        int width,
        int height
    );
    void InitializeEncoder(const std::string& fileName, int width, int height, int fps);
    void EncodeFrame(); // Updated to remove AVFrame* parameter since it's managed internally
    void FinalizeEncoder();

    // New function to retrieve encoded frame data and PTS
    bool getEncodedFrame(std::vector<uint8_t>& frameData, int64_t& pts);

    // Callback type for encoded frame handling
    typedef std::function<void(AVPacket* packet)> EncodedFrameCallback;

    // Function to set the callback for encoded frames
    void setEncodedFrameCallback(EncodedFrameCallback callback);

    // Push packet to WebRTC (declare here since it's used in Encoder.cpp)
    void pushPacketToWebRTC(AVPacket* packet);

    void FlushEncoder();

    void SignalEncoderShutdown();

    // External declaration for WebRTC interaction
    extern "C" int sendVideoPacket(uint8_t* data, int size, int64_t pts);

    // Internal state (declared here for visibility to other files if needed)
    extern SwsContext* swsCtx;
    extern AVFrame* nv12Frame;
    extern int currentWidth;
    extern int currentHeight;

    extern std::mutex g_encoderMutex; 

    extern AVFormatContext* formatCtx;
    extern AVCodecContext* codecCtx;
    extern AVStream* videoStream;
    extern AVPacket* packet;
    extern AVFrame* hwFrame;
    extern AVBufferRef* hwDeviceCtx;
    extern AVBufferRef* hwFramesCtx;
    extern int frameCounter;
    extern int64_t last_dts;

    extern EncodedFrameCallback g_onEncodedFrameCallback;

    extern std::mutex g_frameMutex;
    extern std::condition_variable g_frameAvailable;
    extern std::vector<uint8_t> g_latestFrameData;
    extern int64_t g_latestPTS;
    extern bool g_frameReady;
}