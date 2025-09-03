#include "pch.h"
#include "ConfigUtils.h"
#include "Encoder.h"
#include "CaptureHelpers.h"
#include "AudioCapturer.h"

#include <fstream>

namespace ConfigUtils {

bool LoadConfig(nlohmann::json& outConfig)
{
    try {
        std::ifstream configFile("config.json");
        if (!configFile.is_open()) {
            std::wcerr << L"[config] Error opening config.json" << std::endl;
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

}


