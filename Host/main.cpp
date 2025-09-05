
#include <winrt/Windows.Foundation.h>
#include <windows.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <iostream>
#include <conio.h>
#include <fstream>
#include <nlohmann/json.hpp>

#include "AppInit.h"
#include "ConfigUtils.h"
#include "WindowUtils.h"
#include "GraphicsAndCapture.h"
#include "CaptureHelpers.h"
#include "Websocket.h"
#include "AudioCapturer.h"
#include "ShutdownManager.h"
#include "IdGenerator.h"
#include "Runtime.h"
#include "InputConfig.h"
#include "ErrorUtils.h"
#include "InputIntegrationLayer.h"

// removed: custom callbacks and monitorConnection (moved into modules)

int main()
{
    AppInit::InitializeProcess();
    AppInit::InitializeRtcBindings();

    // --- Load Configuration ---
    nlohmann::json config;
    if (!ConfigUtils::LoadConfig(config)) return -1;

    // --- Load Input Configuration ---
    if (config.contains("host") && config["host"].contains("input")) {
        if (!InputConfig::loadFromJson(config["host"]["input"])) {
            LOG_SYSTEM_ERROR("Failed to load input configuration");
            return -1;
        }
        std::cout << "[main] Input configuration loaded successfully" << std::endl;
        std::cout << "[main] " << InputConfig::getConfigurationSummary() << std::endl;
    } else {
        std::cout << "[main] No input configuration found in config.json, using defaults" << std::endl;
        InputConfig::resetToDefaults();
    }

    // Initialize input integration layer
    if (!InputIntegrationLayer::initialize()) {
        std::cerr << "[main] Failed to initialize input integration layer" << std::endl;
        return -1;
    }

    if (!InputIntegrationLayer::start()) {
        std::cerr << "[main] Failed to start input integration layer" << std::endl;
        return -1;
    }

    std::cout << "[main] Input integration layer started successfully" << std::endl;

    std::string targetProcessName = "";
    if (!ConfigUtils::GetTargetProcessName(config, targetProcessName)) {
        std::wcerr << L"[main] Missing host.targetProcessName in config.json" << std::endl;
        return -1;
    }
    std::wstring wideTargetProcessName(targetProcessName.begin(), targetProcessName.end());

    // --- Room ID Generation ---
    std::string roomId = generateRoomId();
    Runtime::PrintBanner(roomId);
    // --------------------------

    GraphicsAndCapture::D3DContext d3d;
    if (!GraphicsAndCapture::InitializeDevice(d3d)) return -1;

    Sleep(2000);
    HWND hwnd = nullptr; DWORD pid = 0;
    if (!WindowUtils::PickWindowByProcessName(wideTargetProcessName.c_str(), hwnd, pid) || !hwnd) {
        std::wcerr << L"[main] No window with the specified process name found." << std::endl;
        return -1;
    }
    std::wcout << L"[main] Got hwnd: " << hwnd << std::endl;
    WindowUtils::MaybeResizeClientArea(hwnd, config);

    auto item = WindowUtils::CreateItem(hwnd);
    if (!item) {
        std::wcerr << L"[main] Failed to create capture item." << std::endl;
        return -1;
    }
    GraphicsAndCapture::CaptureContext cap;
    if (!GraphicsAndCapture::InitializeCapture(cap, d3d, item)) return -1;
    GraphicsAndCapture::Start(cap);
    // Configure encoder defaults from config
    int cfgFps = config.contains("host") && config["host"].contains("video") ? config["host"]["video"].value("fps", 120) : 120;
    ConfigUtils::ApplyVideoSettings(config);
    ConfigUtils::ApplyCaptureSettings(config, cfgFps);
    ConfigUtils::ApplyAudioSettings(config);
    ConfigUtils::ApplyThreadPrioritySettings(config);
    ConfigUtils::ApplyAdaptiveQualityControlSettings(config);
    StartCapture();
    initWebsocket(roomId);
    // Optional metrics export to signaling channel
    try {
        if (config.contains("host") && config["host"].contains("video")) {
            auto vcfg = config["host"]["video"];
            bool exportMetrics = vcfg.value("exportMetrics", false);
            if (exportMetrics) {
                extern void startMetricsExport(bool enable);
                startMetricsExport(true);
            }
        }
    } catch (...) {}
    AudioCapturer audioCapturer;
    audioCapturer.StartCapture(pid);

    // Start WAV recording for debugging if enabled in config
    try {
        // Simple check for WAV recording
        bool enableWAV = false;
        std::string wavFilename = "output.wav";

        // Try to get the WAV recording setting
        if (config.contains("host") && config["host"].contains("debug")) {
            auto& debugSection = config["host"]["debug"];
            if (debugSection.contains("enableWAVRecording") && debugSection["enableWAVRecording"].is_boolean()) {
                enableWAV = debugSection["enableWAVRecording"];
            }
            if (debugSection.contains("wavFilename") && debugSection["wavFilename"].is_string()) {
                wavFilename = debugSection["wavFilename"];
            }
        }

        if (enableWAV) {
            std::wcout << L"[main] Starting WAV recording to: " << wavFilename.c_str() << std::endl;
            if (!audioCapturer.StartWAVRecording(wavFilename)) {
                std::wcerr << L"[main] Failed to start WAV recording to: " << wavFilename.c_str() << std::endl;
            } else {
                std::wcout << L"[main] WAV recording started successfully" << std::endl;
            }
        } else {
            std::wcout << L"[main] WAV recording disabled in config" << std::endl;
        }
    }
    catch (const std::exception& e) {
        std::wcerr << L"[main] Exception in WAV recording setup: " << e.what() << std::endl;
        std::wcout << L"[main] WAV recording disabled due to error" << std::endl;
    }

    std::wcout << L"[main] Capture started! Press any key to stop.\n";

    // Main loop with better monitoring
    auto lastMonitorTime = std::chrono::steady_clock::now();
    while (!ShutdownManager::IsShutdown()) {
        Runtime::MonitorConnection();
        if (_kbhit()) { // Check for keyboard input
            std::wcout << L"[main] Key pressed. Shutting down." << std::endl;
            ShutdownManager::SetShutdown(true);
        }

        // Monitor performance every second
        auto now = std::chrono::steady_clock::now();
        if (std::chrono::duration_cast<std::chrono::seconds>(now - lastMonitorTime).count() >= 1) {
            // Could add performance monitoring here if needed
            lastMonitorTime = now;
        }

        Sleep(10); // Reduced sleep for more responsive monitoring
    }

    std::wcout << L"[main] Stopping capture...\n";
    // Order shutdown to avoid races: stop capture -> close PC -> stop ws -> flush/close encoder -> close Go
    audioCapturer.StopCapture();
    GraphicsAndCapture::Stop(cap);
    try { closePeerConnection(); } catch (...) {}
    stopWebsocket();
    // Encoder is finalized inside StopCapture(); avoid flushing/finalizing after free
    closeGo(); 

    return 0;
}
