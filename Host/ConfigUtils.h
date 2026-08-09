#pragma once

#include <string>
#include <nlohmann/json.hpp>

namespace ConfigUtils {
    struct NetworkEndpoints {
        std::string mode;
        std::string signalingUrl;
        std::string matchmakerUrl;
    };

    // Loads JSON from config.json into provided object; returns false on error
    bool LoadConfig(nlohmann::json& outConfig);

    // Loads the shared Client/html-server/network-config.json. The host always
    // reaches local/LAN test services over loopback; the browser derives the
    // server machine address from the page URL.
    bool LoadNetworkEndpoints(NetworkEndpoints& outEndpoints);
    bool LoadNetworkEndpoints(const nlohmann::json& config, NetworkEndpoints& outEndpoints);

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


