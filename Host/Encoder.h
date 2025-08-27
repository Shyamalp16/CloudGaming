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

    // Configure encoder bitrate defaults (used on InitializeEncoder)
    void SetBitrateConfig(int start_bitrate_bps, int min_bitrate_bps, int max_bitrate_bps);

    // Configure hardware frame pool size (ring of input D3D11 frames)
    void SetHwFramePoolSize(int pool_size);

    // Configure whether to signal full range (PC) or limited range (TV) in color metadata
    void SetFullRangeColor(bool enable_full_range);

    // Configure pacing: fixed FPS or fixed duration (microseconds)
    void SetPacingFps(int fps);
    void SetPacingFixedUs(int duration_us);

    // Configure PLI policy: ignore flag, min interval (ms), and min loss threshold
    void ConfigurePliPolicy(bool ignorePli, int minIntervalMs, double minLossThreshold);

    // Configure NVENC runtime options
    void SetNvencOptions(const char* preset,
                         const char* rc,
                         int bf,
                         int rc_lookahead,
                         int async_depth,
                         int surfaces);

    // RTCP-driven bitrate control (in-encoder strategy)
    void ConfigureBitrateController(int min_bps,
                                    int max_bps,
                                    int increase_step_bps,
                                    int decrease_cooldown_ms,
                                    int clean_samples_required,
                                    int increase_interval_ms);
    void OnRtcpFeedback(double packetLoss, double rtt, double jitter);

    // Backpressure visibility for capture loop
    bool IsBacklogged(int recent_window_ms, int min_events);
    void GetAndResetBackpressureStats(int &eagainEvents);

    // Enable/disable GPU timing instrumentation around VideoProcessorBlt
    void SetGpuTimingEnabled(bool enable);

    // Enable/disable D3D11 deferred context path for VideoProcessorBlt
    void SetDeferredContextEnabled(bool enable);

    extern "C" int sendVideoSample(uint8_t* data, int size, int64_t durationUs);

    extern int currentWidth;
    extern int currentHeight;

    extern std::mutex g_encoderMutex; 

    extern AVFormatContext* formatCtx;
    extern AVCodecContext* codecCtx;
    extern AVStream* videoStream;
    extern AVPacket* packet;
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

    // New direct NV12 path helpers
    // Acquire next NV12 encoder surface from the HW frame ring; returns slot index and NV12 texture pointer
    bool AcquireHwInputSurface(int &slotIndexOut, ID3D11Texture2D** nv12TextureOut);
    // Perform BGRA->NV12 on the given slot using the D3D11 VideoProcessor
    bool VideoProcessorBltToSlot(ID3D11Texture2D* bgraSrcTexture, int slotIndex);
    // Submit the prepared HW frame at slot to the encoder with timestamp (us)
    bool SubmitHwFrame(int slotIndex, int64_t timestampUs);
}
