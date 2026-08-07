
#include <winrt/Windows.Foundation.h>
#include <windows.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <iostream>
#include <conio.h>
#include <fstream>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <chrono>
#include <utility>

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
#include "MatchmakerClient.h"

int main()
{
    AppInit::InitializeProcess();

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

    std::string targetProcessName = "";
    if (!ConfigUtils::GetTargetProcessName(config, targetProcessName)) {
        std::wcerr << L"[main] Missing host.targetProcessName in config.json" << std::endl;
        return -1;
    }
    std::wstring wideTargetProcessName(targetProcessName.begin(), targetProcessName.end());

    int windowSelectionTimeoutMs = 60000;
    int windowPollIntervalMs = 500;
    int windowReattachGraceMs = 1000;
    bool windowReattachEnabled = true;
    std::wstring preferredWindowTitle;
    if (config.contains("host") && config["host"].contains("window")) {
        const auto& windowConfig = config["host"]["window"];
        windowSelectionTimeoutMs = windowConfig.value("selectionTimeoutMs", windowSelectionTimeoutMs);
        windowPollIntervalMs = windowConfig.value("pollIntervalMs", windowPollIntervalMs);
        windowReattachGraceMs = windowConfig.value("reattachGraceMs", windowReattachGraceMs);
        windowReattachEnabled = windowConfig.value("reattach", windowReattachEnabled);
        const std::string preferredTitle = windowConfig.value("preferredTitleContains", std::string());
        preferredWindowTitle.assign(preferredTitle.begin(), preferredTitle.end());
    }

    // --- Room ID and Host ID Generation ---
    std::string roomId = generateRoomId();
    std::string hostId = generateHostId();
    Runtime::PrintBanner(roomId);
    std::cout << "[main] Host ID: " << hostId << std::endl;
    // --------------------------
    
    // --- Load the one shared network profile used by both host and browser ---
    ConfigUtils::NetworkEndpoints networkEndpoints;
    if (!ConfigUtils::LoadNetworkEndpoints(networkEndpoints)) {
        return -1;
    }

    const std::string& matchmakerUrl = networkEndpoints.matchmakerUrl;
    const std::string& signalingUrl = networkEndpoints.signalingUrl;
    std::string hostSecret = "";
    int heartbeatIntervalMs = 25000;
    const bool matchmakerEnabled = !matchmakerUrl.empty();
    
    if (config.contains("host") && config["host"].contains("matchmaker")) {
        auto& mmCfg = config["host"]["matchmaker"];
        if (mmCfg.contains("hostSecret") && mmCfg["hostSecret"].is_string()) {
            hostSecret = mmCfg["hostSecret"].get<std::string>();
        }
        if (mmCfg.contains("heartbeatIntervalMs") && mmCfg["heartbeatIntervalMs"].is_number()) {
            heartbeatIntervalMs = mmCfg["heartbeatIntervalMs"].get<int>();
        }
    }
    
    if (matchmakerEnabled) {
        std::cout << "[main] Matchmaker URL: " << matchmakerUrl << std::endl;
    } else {
        std::cout << "[main] Matchmaker not configured, running in standalone mode" << std::endl;
    }
    // -------------------------------------

    HWND hwnd = nullptr; DWORD pid = 0;
    if (!WindowUtils::WaitForWindowByProcessName(
            wideTargetProcessName, hwnd, pid,
            windowSelectionTimeoutMs, windowPollIntervalMs, preferredWindowTitle) || !hwnd) {
        std::wcerr << L"[main] Timed out waiting for a viable window from process '"
                   << wideTargetProcessName << L"'." << std::endl;
        return -1;
    }
    std::wcout << L"[main] Got hwnd: " << hwnd << std::endl;
    WindowUtils::MaybeResizeClientArea(hwnd, config);

    GraphicsAndCapture::D3DContext d3d;
    if (!GraphicsAndCapture::InitializeDevice(d3d, hwnd)) return -1;

    auto item = WindowUtils::CreateItem(hwnd);
    if (!item) {
        std::wcerr << L"[main] Failed to create capture item." << std::endl;
        return -1;
    }
    // Apply config-driven pacing/encoder knobs before capture session creation so
    // MinUpdateInterval and related settings are active from the first frame.
    int cfgFps = config.contains("host") && config["host"].contains("video") ? config["host"]["video"].value("fps", 120) : 120;
    ConfigUtils::ApplyVideoSettings(config);
    ConfigUtils::ApplyCaptureSettings(config, cfgFps);
    ConfigUtils::ApplyAudioSettings(config);
    ConfigUtils::ApplyThreadPrioritySettings(config);
    ConfigUtils::ApplyAdaptiveQualityControlSettings(config);

    GraphicsAndCapture::CaptureContext cap;
    if (!GraphicsAndCapture::InitializeCapture(cap, d3d, item)) return -1;

    // Do not start the Go/WebRTC runtime until all configuration, device, window,
    // and capture setup that can fail has succeeded.  Otherwise an early return
    // leaves its background goroutines running until process termination.
    AppInit::InitializeRtcBindings();
    StartCapture();
    GraphicsAndCapture::Start(cap);

    // Start background input threads only after every fallible startup step.
    // Earlier failures must not leave joinable threads alive during process exit.
    if (!InputIntegrationLayer::initialize() || !InputIntegrationLayer::start()) {
        std::cerr << "[main] Failed to start input integration layer" << std::endl;
        GraphicsAndCapture::Stop(cap);
        closeGo();
        return -1;
    }
    std::cout << "[main] Input integration layer started successfully" << std::endl;

    std::cout << "[main] Signaling URL: " << signalingUrl << std::endl;
    initWebsocket(roomId, signalingUrl);
    
    // --- Matchmaker Registration ---
    if (matchmakerEnabled) {
        if (MatchmakerClient::initialize(matchmakerUrl, hostSecret)) {
            // Send initial heartbeat
            if (MatchmakerClient::sendHeartbeat(hostId, roomId)) {
                std::cout << "[main] Successfully registered with matchmaker" << std::endl;
            } else {
                std::cerr << "[main] Warning: Failed to register with matchmaker (will retry via heartbeat)" << std::endl;
            }
            // Start background heartbeat thread
            MatchmakerClient::startHeartbeatThread(hostId, roomId, heartbeatIntervalMs);
        } else {
            std::cerr << "[main] Failed to initialize matchmaker client" << std::endl;
        }
    }
    // -------------------------------
    
    AudioCapturer audioCapturer;
    if (!audioCapturer.StartCapture(pid, targetProcessName)) {
        std::wcerr << L"[main] Audio capture failed to start; continuing with video only" << std::endl;
    }

    // Start WAV recording for debugging if enabled in config
    bool enableWAV = false;
    std::string wavFilename = "output.wav";

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

    std::wcout << L"[main] Capture started! Press any key to stop.\n";

    // Main loop with better monitoring
    auto lastMonitorTime = std::chrono::steady_clock::now();
    auto invalidWindowSince = std::chrono::steady_clock::time_point{};
    while (!ShutdownManager::IsShutdown()) {
        Runtime::MonitorConnection();
        if (_kbhit()) { // Check for keyboard input
            std::wcout << L"[main] Key pressed. Shutting down." << std::endl;
            ShutdownManager::SetShutdown(true);
        }

        // Monitor performance every second
        auto now = std::chrono::steady_clock::now();
        if (std::chrono::duration_cast<std::chrono::seconds>(now - lastMonitorTime).count() >= 1) {
            if (windowReattachEnabled && (!hwnd || !IsWindow(hwnd))) {
                if (invalidWindowSince == std::chrono::steady_clock::time_point{}) {
                    invalidWindowSince = now;
                    WindowUtils::SetTargetWindow(nullptr);
                    std::wcout << L"[window] Captured window was destroyed; waiting for replacement..." << std::endl;
                }

                const auto missingMs = std::chrono::duration_cast<std::chrono::milliseconds>(now - invalidWindowSince).count();
                if (missingMs >= std::max(0, windowReattachGraceMs)) {
                    HWND replacementHwnd = nullptr;
                    DWORD replacementPid = 0;
                    if (WindowUtils::PickWindowByProcessName(
                            wideTargetProcessName, replacementHwnd, replacementPid,
                            preferredWindowTitle, false) && replacementHwnd) {
                        auto replacementItem = WindowUtils::CreateItem(replacementHwnd);
                        GraphicsAndCapture::CaptureContext replacementCap;
                        if (replacementItem && GraphicsAndCapture::InitializeCapture(
                                replacementCap, d3d, replacementItem)) {
                            const DWORD oldPid = pid;
                            GraphicsAndCapture::Stop(cap);
                            cap = std::move(replacementCap);
                            hwnd = replacementHwnd;
                            pid = replacementPid;
                            WindowUtils::SetTargetWindow(hwnd);
                            WindowUtils::MaybeResizeClientArea(hwnd, config);
                            StartCapture();
                            GraphicsAndCapture::Start(cap);
                            invalidWindowSince = {};
                            std::wcout << L"[window] Reattached capture to HWND=" << hwnd
                                       << L" pid=" << pid << std::endl;

                            if (oldPid != pid) {
                                audioCapturer.StopCapture();
                                if (!audioCapturer.StartCapture(pid, targetProcessName)) {
                                    std::wcerr << L"[window] Audio failed to restart for replacement process" << std::endl;
                                }
                            }
                        }
                    }
                }
            } else {
                invalidWindowSince = {};
            }
            lastMonitorTime = now;
        }

        Sleep(10); // Reduced sleep for more responsive monitoring
    }

    std::wcout << L"[main] Stopping capture...\n";
    
    // Stop matchmaker heartbeat first
    if (matchmakerEnabled) {
        std::cout << "[main] Stopping matchmaker heartbeat..." << std::endl;
        MatchmakerClient::stopHeartbeatThread();
    }
    
    // Order shutdown to avoid races: stop capture -> close PC -> stop ws -> flush/close encoder -> close Go
    audioCapturer.StopCapture();
    GraphicsAndCapture::Stop(cap);
    try { closePeerConnection(); } catch (...) {}
    stopWebsocket();
    // Encoder is finalized inside StopCapture(); avoid flushing/finalizing after free
    closeGo(); 

    return 0;
}
