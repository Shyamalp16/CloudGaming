#include "pch.h"
#include "ConfigUtils.h"
#include "Encoder.h"
#include "CaptureHelpers.h"
#include "AudioCapturer.h"
#include "ThreadPriorityManager.h"
#include "AdaptiveQualityControl.h"

#include <fstream>
#include <Windows.h>

namespace ConfigUtils {

bool LoadConfig(nlohmann::json& outConfig)
{
    try {
        // Get current working directory for debugging
        char cwd[1024];
        if (GetCurrentDirectoryA(sizeof(cwd), cwd)) {
            std::wcout << L"[config] Current working directory: " << cwd << std::endl;
        }

        std::string configPath = "config.json";
        std::ifstream configFile(configPath);

        // If config.json not found in current directory, try the executable directory
        if (!configFile.is_open()) {
            char exePath[MAX_PATH];
            if (GetModuleFileNameA(NULL, exePath, MAX_PATH)) {
                std::string exeDir = exePath;
                size_t lastSlash = exeDir.find_last_of("\\/");
                if (lastSlash != std::string::npos) {
                    exeDir = exeDir.substr(0, lastSlash);
                    configPath = exeDir + "\\config.json";
                    configFile.open(configPath);
                    std::wcout << L"[config] Trying config path: " << configPath.c_str() << std::endl;
                }
            }
        }

        if (!configFile.is_open()) {
            std::wcerr << L"[config] Error opening config.json from any location" << std::endl;
            std::wcerr << L"[config] Make sure config.json exists in the working directory or executable directory" << std::endl;
            return false;
        }

        configFile >> outConfig;
        return true;
    } catch (const std::exception& e) {
        std::wcerr << L"[config] Error reading config.json: " << e.what() << std::endl;
        return false;
    }
}

bool GetTargetProcessName(const nlohmann::json& config, std::string& outName)
{
    try {
        if (config.contains("host") && config["host"].contains("targetProcessName") && config["host"]["targetProcessName"].is_string()) {
            outName = config["host"]["targetProcessName"].get<std::string>();
            return !outName.empty();
        }
    } catch (...) {}
    return false;
}

void ApplyVideoSettings(const nlohmann::json& config)
{
    try {
        if (!(config.contains("host") && config["host"].contains("video"))) return;
        auto vcfg = config["host"]["video"];
        int cfgFps = vcfg.value("fps", 120);
        int brStart = vcfg.value("bitrateStart", 20000000);
        int brMin = vcfg.value("bitrateMin", 10000000);
        int brMax = vcfg.value("bitrateMax", 50000000);
        Encoder::SetBitrateConfig(brStart, brMin, brMax);
        Encoder::ConfigureBitrateController(brMin, brMax,
                                           5'000'000,
                                           300,
                                           3,
                                           1000);
        SetCaptureTargetFps(cfgFps);

        bool fullRange = vcfg.value("fullRange", true);
        Encoder::SetFullRangeColor(fullRange);

        bool gpuTiming = vcfg.value("gpuTiming", false);
        Encoder::SetGpuTimingEnabled(gpuTiming);

        bool deferredCtx = vcfg.value("deferredContext", false);
        Encoder::SetDeferredContextEnabled(deferredCtx);

        if (vcfg.contains("pacingFixedUs")) {
            Encoder::SetPacingFixedUs(std::max(1, vcfg["pacingFixedUs"].get<int>()));
        } else if (vcfg.contains("pacingFps")) {
            Encoder::SetPacingFps(std::max(1, vcfg["pacingFps"].get<int>()));
        }

        bool ignorePli = vcfg.value("ignorePli", false);
        int minPliIntervalMs = vcfg.value("minPliIntervalMs", 500);
        double minLossThreshold = vcfg.value("minPliLossThreshold", 0.03);
        Encoder::ConfigurePliPolicy(ignorePli, minPliIntervalMs, minLossThreshold);

        if (vcfg.contains("hwFramePoolSize")) {
            int pool = vcfg["hwFramePoolSize"].get<int>();
            Encoder::SetHwFramePoolSize(pool);
        }

        std::string preset = vcfg.value("preset", std::string("p5"));
        std::string rc     = vcfg.value("rc", std::string("cbr"));
        int bf             = vcfg.value("bf", 0);
        int rcLookahead    = vcfg.value("rcLookahead", 0);
        int asyncDepth     = vcfg.value("asyncDepth", 2);
        int surfaces       = vcfg.value("surfaces", 8);
        Encoder::SetNvencOptions(preset.c_str(), rc.c_str(), bf, rcLookahead, asyncDepth, surfaces);

        // HDR tone mapping configuration
        if (vcfg.contains("hdrToneMapping")) {
            auto hdrCfg = vcfg["hdrToneMapping"];
            bool hdrEnabled = hdrCfg.value("enabled", false);
            std::string method = hdrCfg.value("method", std::string("reinhard"));
            float exposure = hdrCfg.value("exposure", 0.0f);
            float gamma = hdrCfg.value("gamma", 2.2f);
            float saturation = hdrCfg.value("saturation", 1.0f);
            Encoder::SetHdrToneMappingConfig(hdrEnabled, method, exposure, gamma, saturation);
        }
    } catch (...) {}
}


void ApplyCaptureSettings(const nlohmann::json& config, int configuredFps)
{
    try {
        if (!(config.contains("host") && config["host"].contains("capture"))) {
            if (configuredFps > 0) {
                long long interval = 10000000LL / configuredFps;
                // Allow system to run at configured FPS without artificial clamping
                SetMinUpdateInterval100ns(interval);
            }
            return;
        }
        auto ccfg = config["host"]["capture"];
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
        if (ccfg.contains("dropWindowMs") || ccfg.contains("dropMinEvents")) {
            int w = ccfg.value("dropWindowMs", 200);
            int m = ccfg.value("dropMinEvents", 2);
            SetBackpressureDropPolicy(w, m);
        }
        if (ccfg.contains("mmcss")) {
            auto mcfg = ccfg["mmcss"];
            bool enable = mcfg.value("enable", true);
            int prio = mcfg.value("priority", 2);
            SetMmcssConfig(enable, prio);
        }
        if (ccfg.contains("minUpdateInterval100ns")) {
            long long interval = 0;
            try { interval = ccfg["minUpdateInterval100ns"].get<long long>(); } catch (...) { interval = 0; }
            SetMinUpdateInterval100ns(interval);
        } else {
            if (configuredFps > 0) {
                long long interval = 10000000LL / configuredFps;
                // Allow system to run at configured FPS without artificial clamping
                SetMinUpdateInterval100ns(interval);
            }
        }
        if (ccfg.contains("skipUnchanged")) {
            bool skip = false;
            try { skip = ccfg["skipUnchanged"].get<bool>(); } catch (...) { skip = false; }
            SetSkipUnchanged(skip);
        }
    } catch (...) {}
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


