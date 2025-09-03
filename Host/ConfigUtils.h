#pragma once

#include <string>
#include <nlohmann/json.hpp>

namespace ConfigUtils {
    // Loads JSON from config.json into provided object; returns false on error
    bool LoadConfig(nlohmann::json& outConfig);

    // Extract target process name from config (host.targetProcessName)
    bool GetTargetProcessName(const nlohmann::json& config, std::string& outName);

    // Apply video-related settings to Encoder and capture helpers
    void ApplyVideoSettings(const nlohmann::json& config);

    // Apply capture-related settings to WGC/capture helpers
    void ApplyCaptureSettings(const nlohmann::json& config, int configuredFps);

    // Apply audio-related settings to AudioCapturer
    void ApplyAudioSettings(const nlohmann::json& config);

    // Apply thread priority settings to ThreadPriorityManager
    void ApplyThreadPrioritySettings(const nlohmann::json& config);

    // Apply adaptive quality control settings to AdaptiveQualityControl
    void ApplyAdaptiveQualityControlSettings(const nlohmann::json& config);
}


