#pragma once

#include <d3d11.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_d3d11va.h>
#include <libavutil/opt.h>
}
#include <cstdint>
#include <iostream>
#include <mutex>
#include <vector>
#include <condition_variable>
#include <functional>

namespace Encoder {
    // Updated EncodeFrame signature to include PTS
    void EncodeFrame(ID3D11Texture2D* texture, ID3D11DeviceContext* context, int width, int height, int64_t pts);
    void InitializeEncoder(const std::string& fileName, int width, int height, int fps);
    void FinalizeEncoder();

    bool getEncodedFrame(std::vector<uint8_t>& frameData, int64_t& pts);

    typedef std::function<void(AVPacket* packet)> EncodedFrameCallback;
    void setEncodedFrameCallback(EncodedFrameCallback callback);

    void pushPacketToWebRTC(AVPacket* packet);
    void FlushEncoder();
    void SignalEncoderShutdown();
    void AdjustBitrate(int new_bitrate);
    void RequestIDR();

    extern "C" int sendVideoSample(uint8_t* data, int size, int64_t durationUs);

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
