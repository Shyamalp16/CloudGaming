#include "pch.h"
#include "ConfigUtils.h"
#include "Encoder.h"
#include "CaptureHelpers.h"
#include "AudioCapturer.h"
#include "ThreadPriorityManager.h"
#include "AdaptiveQualityControl.h"
#include "ConfigStore.h"

#include <fstream>
#include <filesystem>
#include <vector>
#include <Windows.h>
#include <Winhttp.h>

#pragma comment(lib, "Winhttp.lib")

namespace ConfigUtils {

namespace {
bool validServiceEndpoint(const std::string& value, bool websocket, bool secure) {
    const std::string required = websocket ? (secure ? "wss://" : "ws://")
                                           : (secure ? "https://" : "http://");
    if (value.rfind(required, 0) != 0 || value.size() > 2048) return false;
    std::string normalized = value;
    normalized.replace(0, required.size(), secure ? "https://" : "http://");
    const int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, normalized.data(),
        static_cast<int>(normalized.size()), nullptr, 0);
    if (size <= 0) return false;
    std::wstring wide(static_cast<size_t>(size), L'\0');
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, normalized.data(),
        static_cast<int>(normalized.size()), wide.data(), size) != size) return false;
    URL_COMPONENTSW parts{sizeof(parts)};
    parts.dwSchemeLength = parts.dwHostNameLength = parts.dwUrlPathLength =
        parts.dwExtraInfoLength = parts.dwUserNameLength = parts.dwPasswordLength =
            static_cast<DWORD>(-1);
    if (!WinHttpCrackUrl(wide.c_str(), 0, 0, &parts) || parts.dwHostNameLength == 0 ||
        parts.dwUserNameLength || parts.dwPasswordLength || parts.dwExtraInfoLength ||
        parts.dwUrlPathLength > 1) return false;
    return parts.nScheme == (secure ? INTERNET_SCHEME_HTTPS : INTERNET_SCHEME_HTTP);
}

bool openNetworkConfig(std::ifstream& stream, std::filesystem::path& selectedPath)
{
    const std::filesystem::path relativePath =
        std::filesystem::path("Client") / "html-server" / "network-config.json";

    std::vector<std::filesystem::path> candidates = {
        relativePath,
        "network-config.json"
    };

    char exePath[MAX_PATH]{};
    if (GetModuleFileNameA(nullptr, exePath, MAX_PATH)) {
        const auto exeDir = std::filesystem::path(exePath).parent_path();
        candidates.push_back(exeDir / "network-config.json");
        candidates.push_back(exeDir / ".." / ".." / relativePath);
    }

    for (const auto& candidate : candidates) {
        stream.open(candidate);
        if (stream.is_open()) {
            selectedPath = std::filesystem::absolute(candidate).lexically_normal();
            return true;
        }
        stream.clear();
    }
    return false;
}
}

bool LoadConfig(nlohmann::json& outConfig)
{
    std::string error;
    if (!ConfigStore::EnsureUserConfig(outConfig, error)) {
        std::cerr << "[config] " << error << std::endl;
        return false;
    }
    std::cout << "[config] Loaded user configuration: " << ConfigStore::Path().string() << std::endl;
    return true;
}

bool GetTargetProcessName(const nlohmann::json& config, std::string& outName)
{
    if (config.contains("host") && config["host"].contains("targetProcessName") && config["host"]["targetProcessName"].is_string()) {
        outName = config["host"]["targetProcessName"].get<std::string>();
        return !outName.empty();
    }
    return false;
}

void ApplyVideoSettings(const nlohmann::json& config)
{
    if (!(config.contains("host") && config["host"].contains("video"))) return;
    auto vcfg = config["host"]["video"];
    int cfgFps = std::clamp(vcfg.value("fps", 60), 15, 240);
    // WAN-friendly defaults: 8–15 Mbps for 1080p60; 20M/50M punishes real internet
    int brMin = std::clamp(vcfg.value("bitrateMin", 3000000), 500000, 100000000);
    int brMax = std::clamp(vcfg.value("bitrateMax", 12000000), brMin, 100000000);
    int brStart = std::clamp(vcfg.value("bitrateStart", 8000000), brMin, brMax);
    Encoder::SetBitrateLimits(brMin, brMax);
    (void)brStart; // Applied by the StreamProfileManager at a frame boundary.
    (void)cfgFps;

    // Bitrate controller: decreaseCooldown 500–1000ms for WAN; 5s was too slow on real loss
    int increaseStep = 1000000;       // +1 Mbps
    int decreaseCooldownMs = 1000;     // 500–1000ms until stable
    int cleanSamplesRequired = 3;
    int increaseIntervalMs = 1000;
    if (vcfg.contains("bitrateController")) {
        auto bc = vcfg["bitrateController"];
        increaseStep = bc.value("increaseStepBps", increaseStep);
        decreaseCooldownMs = bc.value("decreaseCooldownMs", decreaseCooldownMs);
        cleanSamplesRequired = bc.value("cleanSamplesRequired", cleanSamplesRequired);
        increaseIntervalMs = bc.value("increaseIntervalMs", increaseIntervalMs);
    }
    Encoder::ConfigureBitrateController(brMin, brMax,
                                       increaseStep,
                                       decreaseCooldownMs,
                                       cleanSamplesRequired,
                                       increaseIntervalMs);

    bool fullRange = vcfg.value("fullRange", true);
    Encoder::SetFullRangeColor(fullRange);

    bool ignorePli = vcfg.value("ignorePli", false);
    int minPliIntervalMs = vcfg.value("minPliIntervalMs", 500);
    double minLossThreshold = vcfg.value("minPliLossThreshold", 0.03);
    Encoder::ConfigurePliPolicy(ignorePli, minPliIntervalMs, minLossThreshold);

    if (vcfg.contains("hwFramePoolSize")) {
        int pool = vcfg["hwFramePoolSize"].get<int>();
        Encoder::SetHwFramePoolSize(pool);
    }

    std::string preset = vcfg.value("preset", std::string("p2"));
    std::string rc     = vcfg.value("rc", std::string("cbr"));
    int bf             = std::clamp(vcfg.value("bf", 0), 0, 4);
    int rcLookahead    = std::clamp(vcfg.value("rcLookahead", 0), 0, 32);
    int asyncDepth     = std::clamp(vcfg.value("asyncDepth", 2), 1, 8);
    int surfaces       = std::clamp(vcfg.value("surfaces", 8), 2, 32);
    Encoder::SetNvencOptions(preset.c_str(), rc.c_str(), bf, rcLookahead, asyncDepth, surfaces);

    if (vcfg.contains("hdrToneMapping")) {
        auto hdrCfg = vcfg["hdrToneMapping"];
        bool hdrEnabled = hdrCfg.value("enabled", false);
        std::string method = hdrCfg.value("method", std::string("reinhard"));
        float exposure = hdrCfg.value("exposure", 0.0f);
        float gamma = hdrCfg.value("gamma", 2.2f);
        float saturation = hdrCfg.value("saturation", 1.0f);
        Encoder::SetHdrToneMappingConfig(hdrEnabled, method, exposure, gamma, saturation);
    }
}


void ApplyCaptureSettings(const nlohmann::json& config, int configuredFps)
{
    (void)configuredFps;
    if (!(config.contains("host") && config["host"].contains("capture"))) {
        // Source throttling aliases against high-refresh games. The capture
        // callback performs efficient, deadline-based selection instead.
        SetMinUpdateInterval100ns(0);
        return;
    }
    auto ccfg = config["host"]["capture"];
    if (ccfg.contains("copyPoolSize")) {
        SetCopyPoolSize(std::max(2, ccfg["copyPoolSize"].get<int>()));
    }
    if (ccfg.contains("maxQueueDepth")) {
        SetMaxQueuedFrames(std::max(1, ccfg["maxQueueDepth"].get<int>()));
    }
    if (ccfg.contains("framePoolBuffers")) {
        SetFramePoolBuffers(std::max(1, ccfg["framePoolBuffers"].get<int>()));
    } else if (ccfg.contains("numberOfBuffers")) {
        SetFramePoolBuffers(std::max(1, ccfg["numberOfBuffers"].get<int>()));
    }
    if (ccfg.contains("cursor")) {
        bool cursor = ccfg.value("cursor", true);
        SetCursorCaptureEnabled(cursor);
    }
    if (ccfg.contains("borderRequired")) {
        bool border = ccfg.value("borderRequired", true);
        SetBorderRequired(border);
    }
    if (ccfg.contains("mmcss")) {
        auto mcfg = ccfg["mmcss"];
        bool enable = mcfg.value("enable", true);
        int prio = mcfg.value("priority", 2);
        SetMmcssConfig(enable, prio);
    }
    SetMinUpdateInterval100ns(0);
}

bool LoadNetworkEndpoints(NetworkEndpoints& outEndpoints)
{
    try {
        std::ifstream configFile;
        std::filesystem::path configPath;
        if (!openNetworkConfig(configFile, configPath)) {
            std::cerr << "[network] Cannot find Client/html-server/network-config.json" << std::endl;
            return false;
        }

        nlohmann::json networkConfig;
        configFile >> networkConfig;

        if (!networkConfig.contains("mode") || !networkConfig["mode"].is_string()) {
            std::cerr << "[network] network-config.json requires a string 'mode'" << std::endl;
            return false;
        }

        outEndpoints.mode = networkConfig["mode"].get<std::string>();
        if (outEndpoints.mode == "local") {
            const auto ports = networkConfig.value("ports", nlohmann::json::object());
            const int signalingPort = ports.value("signaling", 3002);
            const int matchmakerPort = ports.value("matchmaker", 3000);
            if (signalingPort < 1 || signalingPort > 65535 ||
                matchmakerPort < 1 || matchmakerPort > 65535) {
                std::cerr << "[network] Local service ports must be between 1 and 65535" << std::endl;
                return false;
            }
            outEndpoints.signalingUrl = "ws://127.0.0.1:" + std::to_string(signalingPort);
            outEndpoints.matchmakerUrl = "http://127.0.0.1:" + std::to_string(matchmakerPort);
        } else if (outEndpoints.mode == "production") {
            if (!networkConfig.contains("production") || !networkConfig["production"].is_object()) {
                std::cerr << "[network] Production mode requires a 'production' object" << std::endl;
                return false;
            }
            const auto& production = networkConfig["production"];
            outEndpoints.signalingUrl = production.value("signalingUrl", std::string{});
            outEndpoints.matchmakerUrl = production.value("matchmakerUrl", std::string{});
            if (!validServiceEndpoint(outEndpoints.signalingUrl, true, true) ||
                !validServiceEndpoint(outEndpoints.matchmakerUrl, false, true)) {
                std::cerr << "[network] Production endpoints must use wss:// and https://" << std::endl;
                return false;
            }
        } else {
            std::cerr << "[network] Invalid or insecure mode '" << outEndpoints.mode
                      << "' (expected local or production; cleartext LAN mode is disabled)" << std::endl;
            return false;
        }

        std::cout << "[network] Loaded " << configPath.string()
                  << " (mode=" << outEndpoints.mode << ")" << std::endl;
        return true;
    } catch (const std::exception& e) {
        std::cerr << "[network] Failed to load network configuration: " << e.what() << std::endl;
        return false;
    }
}

bool LoadNetworkEndpoints(const nlohmann::json& config, NetworkEndpoints& outEndpoints)
{
    if (!config.contains("network") || !config["network"].is_object()) {
        return LoadNetworkEndpoints(outEndpoints);
    }
    try {
        const auto& networkConfig = config["network"];
        outEndpoints.mode = networkConfig.value("mode", std::string{});
        if (outEndpoints.mode == "local") {
            const auto ports = networkConfig.value("ports", nlohmann::json::object());
            const int signalingPort = ports.value("signaling", 3002);
            const int matchmakerPort = ports.value("matchmaker", 3000);
            if (signalingPort < 1 || signalingPort > 65535 || matchmakerPort < 1 || matchmakerPort > 65535) return false;
            outEndpoints.signalingUrl = "ws://127.0.0.1:" + std::to_string(signalingPort);
            outEndpoints.matchmakerUrl = "http://127.0.0.1:" + std::to_string(matchmakerPort);
        } else if (outEndpoints.mode == "production") {
            const auto production = networkConfig.value("production", nlohmann::json::object());
            outEndpoints.signalingUrl = production.value("signalingUrl", std::string{});
            outEndpoints.matchmakerUrl = production.value("matchmakerUrl", std::string{});
            if (!validServiceEndpoint(outEndpoints.signalingUrl, true, true) ||
                !validServiceEndpoint(outEndpoints.matchmakerUrl, false, true)) return false;
        } else return false;
        return true;
    } catch (...) { return false; }
}

void ApplyAudioSettings(const nlohmann::json& config)
{
    try {
        if (!(config.contains("host") && config["host"].contains("audio"))) {
            std::wcout << L"[ConfigUtils] No audio configuration found, using defaults" << std::endl;
            return;
        }

        auto acfg = config["host"]["audio"];

        // Apply audio settings to AudioCapturer
        AudioCapturer::SetAudioConfig(acfg);

        std::wcout << L"[ConfigUtils] Audio configuration applied successfully" << std::endl;

    } catch (const std::exception& e) {
        std::wcerr << L"[ConfigUtils] Error applying audio settings: " << e.what() << std::endl;
    } catch (...) {
        std::wcerr << L"[ConfigUtils] Unknown error applying audio settings" << std::endl;
    }
}

void ApplyThreadPrioritySettings(const nlohmann::json& config)
{
    try {
        if (!(config.contains("host") && config["host"].contains("input") &&
              config["host"]["input"].contains("threadPriority"))) {
            std::cout << "[ConfigUtils] No thread priority configuration found, using environment/defaults" << std::endl;
            return;
        }

        auto tcfg = config["host"]["input"]["threadPriority"];

        // Configure MMCSS
        bool enableMMCSS = tcfg.value("enableMMCSS", true);
        ThreadPriorityManager::enableMMCSS(enableMMCSS);

        // Configure MMCSS class
        std::string mmcssClassStr = tcfg.value("mmcssClass", std::string("Games"));
        if (mmcssClassStr == "Games") {
            ThreadPriorityManager::setMMCSSClass(ThreadPriorityManager::MMCSSClass::Games);
        } else if (mmcssClassStr == "Display") {
            ThreadPriorityManager::setMMCSSClass(ThreadPriorityManager::MMCSSClass::Display);
        } else if (mmcssClassStr == "Audio") {
            ThreadPriorityManager::setMMCSSClass(ThreadPriorityManager::MMCSSClass::Audio);
        } else if (mmcssClassStr == "Playback") {
            ThreadPriorityManager::setMMCSSClass(ThreadPriorityManager::MMCSSClass::Playback);
        } else if (mmcssClassStr == "Capture") {
            ThreadPriorityManager::setMMCSSClass(ThreadPriorityManager::MMCSSClass::Capture);
        } else {
            ThreadPriorityManager::setMMCSSClass(ThreadPriorityManager::MMCSSClass::Games);
        }

        // Configure TIME_CRITICAL priority
        bool enableTimeCritical = tcfg.value("enableTimeCritical", true);
        ThreadPriorityManager::enableTimeCritical(enableTimeCritical);

        // Configure thread priority level
        int threadPriority = tcfg.value("threadPriority", THREAD_PRIORITY_TIME_CRITICAL);
        ThreadPriorityManager::setThreadPriority(threadPriority);

        // Configure task name
        std::string taskName = tcfg.value("taskName", std::string("InputInjection"));
        ThreadPriorityManager::setTaskName(taskName);

        // Configure fallback options
        bool fallbackToWin32Priority = tcfg.value("fallbackToWin32Priority", true);
        ThreadPriorityManager::globalPriorityConfig.fallbackToWin32Priority = fallbackToWin32Priority;

        bool showDiagnosticsOnFailure = tcfg.value("showDiagnosticsOnFailure", true);
        ThreadPriorityManager::globalPriorityConfig.showDiagnosticsOnFailure = showDiagnosticsOnFailure;

        bool retryMMCSSOnFailure = tcfg.value("retryMMCSSOnFailure", false);
        ThreadPriorityManager::globalPriorityConfig.retryMMCSSOnFailure = retryMMCSSOnFailure;

        int mmcssRetryDelayMs = tcfg.value("mmcssRetryDelayMs", 1000);
        ThreadPriorityManager::globalPriorityConfig.mmcssRetryDelayMs = mmcssRetryDelayMs;

        std::cout << "[ConfigUtils] Thread priority configuration applied successfully" << std::endl;
        std::cout << "  MMCSS: " << (enableMMCSS ? "enabled" : "disabled") << std::endl;
        std::cout << "  MMCSS Class: " << mmcssClassStr << std::endl;
        std::cout << "  TIME_CRITICAL: " << (enableTimeCritical ? "enabled" : "disabled") << std::endl;
        std::cout << "  Thread Priority: " << threadPriority << std::endl;
        std::cout << "  Task Name: " << taskName << std::endl;
        std::cout << "  Fallback to Win32: " << (fallbackToWin32Priority ? "enabled" : "disabled") << std::endl;
        std::cout << "  Show Diagnostics: " << (showDiagnosticsOnFailure ? "enabled" : "disabled") << std::endl;
        std::cout << "  Retry MMCSS: " << (retryMMCSSOnFailure ? "enabled" : "disabled") << std::endl;

    } catch (const std::exception& e) {
        std::cerr << "[ConfigUtils] Error applying thread priority settings: " << e.what() << std::endl;
    } catch (...) {
        std::cerr << "[ConfigUtils] Unknown error applying thread priority settings" << std::endl;
    }
}

void ApplyAdaptiveQualityControlSettings(const nlohmann::json& config)
{
    try {
        if (!(config.contains("host") && config["host"].contains("input") &&
              config["host"]["input"].contains("adaptiveQualityControl"))) {
            std::cout << "[ConfigUtils] No adaptive quality control configuration found, using defaults" << std::endl;
            return;
        }

        auto qcfg = config["host"]["input"]["adaptiveQualityControl"];

        AdaptiveQualityControl::DroppingConfig droppingConfig;

        // Network condition thresholds
        droppingConfig.rttExcellentThreshold = qcfg.value("rttExcellentThreshold", 10.0);
        droppingConfig.rttGoodThreshold = qcfg.value("rttGoodThreshold", 50.0);
        droppingConfig.rttFairThreshold = qcfg.value("rttFairThreshold", 100.0);
        droppingConfig.rttPoorThreshold = qcfg.value("rttPoorThreshold", 200.0);

        droppingConfig.lossExcellentThreshold = qcfg.value("lossExcellentThreshold", 0.01);
        droppingConfig.lossGoodThreshold = qcfg.value("lossGoodThreshold", 0.05);
        droppingConfig.lossFairThreshold = qcfg.value("lossFairThreshold", 0.10);
        droppingConfig.lossPoorThreshold = qcfg.value("lossPoorThreshold", 0.20);

        droppingConfig.queueExcellentThreshold = qcfg.value("queueExcellentThreshold", 1u);
        droppingConfig.queueGoodThreshold = qcfg.value("queueGoodThreshold", 2u);
        droppingConfig.queueFairThreshold = qcfg.value("queueFairThreshold", 5u);
        droppingConfig.queuePoorThreshold = qcfg.value("queuePoorThreshold", 10u);

        // Dropping ratios
        droppingConfig.excellentDropRatio = qcfg.value("excellentDropRatio", 0.0);
        droppingConfig.goodDropRatio = qcfg.value("goodDropRatio", 0.0);
        droppingConfig.fairDropRatio = qcfg.value("fairDropRatio", 0.25);
        droppingConfig.poorDropRatio = qcfg.value("poorDropRatio", 0.5);
        droppingConfig.criticalDropRatio = qcfg.value("criticalDropRatio", 0.75);

        // Control settings
        droppingConfig.enableAdaptiveDropping = qcfg.value("enableAdaptiveDropping", true);
        droppingConfig.minFrameIntervalMs = qcfg.value("minFrameIntervalMs", 5u);
        droppingConfig.statsUpdateIntervalMs = qcfg.value("statsUpdateIntervalMs", 100u);

        // Apply configuration
        AdaptiveQualityControl::globalQualityController.setConfig(droppingConfig);

        // Enable adaptive quality control
        if (droppingConfig.enableAdaptiveDropping) {
            AdaptiveQualityControl::enableAdaptiveQualityControl();
        }

        std::cout << "[ConfigUtils] Adaptive quality control configuration applied successfully" << std::endl;
        std::cout << "  Adaptive Dropping: " << (droppingConfig.enableAdaptiveDropping ? "enabled" : "disabled") << std::endl;
        std::cout << "  RTT Thresholds: " << droppingConfig.rttExcellentThreshold << "/"
                  << droppingConfig.rttGoodThreshold << "/"
                  << droppingConfig.rttFairThreshold << "/"
                  << droppingConfig.rttPoorThreshold << "ms" << std::endl;
        std::cout << "  Loss Thresholds: " << (droppingConfig.lossExcellentThreshold * 100) << "%/"
                  << (droppingConfig.lossGoodThreshold * 100) << "%/"
                  << (droppingConfig.lossFairThreshold * 100) << "%/"
                  << (droppingConfig.lossPoorThreshold * 100) << "%" << std::endl;
        std::cout << "  Drop Ratios: " << droppingConfig.fairDropRatio << "/"
                  << droppingConfig.poorDropRatio << "/"
                  << droppingConfig.criticalDropRatio << std::endl;

    } catch (const std::exception& e) {
        std::cerr << "[ConfigUtils] Error applying adaptive quality control settings: " << e.what() << std::endl;
    } catch (...) {
        std::cerr << "[ConfigUtils] Unknown error applying adaptive quality control settings" << std::endl;
    }
}

}


