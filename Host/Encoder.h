#pragma once

#include <d3d11.h>

#include <cstdint>
#include <string>

namespace Encoder {
    bool InitializeEncoder(int width, int height, int fps);
    void FinalizeEncoder();

    void AdjustBitrate(int new_bitrate);
    void RequestIDR();

    // Configure encoder bitrate defaults (used on InitializeEncoder)
    void SetBitrateConfig(int start_bitrate_bps, int min_bitrate_bps, int max_bitrate_bps);

    // Configure hardware frame pool size (ring of input D3D11 frames)
    void SetHwFramePoolSize(int pool_size);

    // Configure encode resolution (0 = use capture size). VP downscales capture->encode for FPS.
    void SetEncodeSize(int width, int height);

    // Configure whether to signal full range (PC) or limited range (TV) in color metadata
    void SetFullRangeColor(bool enable_full_range);

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

    // HDR tone mapping configuration
    void SetHdrToneMappingConfig(bool enabled, const std::string& method, float exposure, float gamma, float saturation);

    // New direct NV12 path helpers
    // Acquire next NV12 encoder surface from the HW frame ring; returns slot index and NV12 texture pointer
    bool AcquireHwInputSurface(int &slotIndexOut, ID3D11Texture2D** nv12TextureOut);
    // Perform BGRA->NV12 on the given slot using the D3D11 VideoProcessor
    bool VideoProcessorBltToSlot(ID3D11Texture2D* bgraSrcTexture, int slotIndex);
    // Submit the prepared HW frame at slot to the encoder with timestamp (us)
    bool SubmitHwFrame(int slotIndex, int64_t timestampUs);
}
