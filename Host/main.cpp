
#include <winrt/Windows.Foundation.h>
#include <windows.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <iostream>
#include <conio.h>
#include <fstream>
#include <nlohmann/json.hpp>

#include "D3DHelpers.h"
#include "WindowHelpers.h"
#include "CaptureHelpers.h"
#include "Websocket.h"
#include "AudioCapturer.h"
#include "ShutdownManager.h"
#include "IdGenerator.h"
#include "pion_webrtc.h" 
#include "Encoder.h"

// PLI callback implemented in Encoder.cpp
extern "C" void OnPLI();

// RTCP Callback Function (delegates to Encoder-managed controller)
void onRTCP(double packetLoss, double rtt, double jitter) {
    Encoder::OnRtcpFeedback(packetLoss, rtt, jitter);
}

// Function to monitor the WebRTC connection state
void monitorConnection() {
    int state = getPeerConnectionState();
    if (state == 4 || state == 5 || state == 6) { // Disconnected, Failed, Closed
        std::wcout << L"[main] Peer disconnected (state=" << state << L"). Keeping host alive for reconnection." << std::endl;
    }
}

int main()
{
    // Set process priority to HIGH for better performance
    if (!SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS)) {
        std::wcerr << L"[main] Warning: Failed to set HIGH_PRIORITY_CLASS" << std::endl;
    } else {
        std::wcout << L"[main] Process priority set to HIGH" << std::endl;
    }

    // Ensure the process is DPI-aware so WGC item.Size() is not DPI-virtualized (which shrinks sizes)
    if (!SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2)) {
        SetProcessDPIAware();
    }
    winrt::init_apartment(winrt::apartment_type::multi_threaded);
    std::wcout << L"[main] Apartment initialized.\n";

    initGo(); 
    SetRTCPCallback(onRTCP);
    // Register PLI callback to force IDR on encoder
    SetPLICallback(OnPLI);

    // --- Load Configuration ---
    nlohmann::json config;
    try {
        std::ifstream configFile("config.json");
        if (!configFile.is_open()) {
            std::wcerr << L"[main] Error opening config.json" << std::endl;
            return -1;
        }
        configFile >> config;
    } catch (const std::exception& e) {
        std::wcerr << L"[main] Error reading config.json: " << e.what() << std::endl;
        return -1;
    }

    std::string targetProcessName = "";
    try {
        if (config.contains("host") && config["host"].contains("targetProcessName") && config["host"]["targetProcessName"].is_string()) {
            targetProcessName = config["host"]["targetProcessName"].get<std::string>();
        }
    } catch (...) {}
    if (targetProcessName.empty()) {
        std::wcerr << L"[main] Missing host.targetProcessName in config.json" << std::endl;
        return -1;
    }
    std::wstring wideTargetProcessName(targetProcessName.begin(), targetProcessName.end());

    // --- Room ID Generation ---
    std::string roomId = generateRoomId();
    std::wcout << L"\n----------------------------------------\n";
    std::wcout << L"  Cloud Gaming Host Initialized\n";
    std::wcout << L"  Your Room ID is: " << winrt::to_hstring(roomId).c_str() << L"\n";
    std::wcout << L"  Please copy this ID into the web client.\n";
    std::wcout << L"----------------------------------------\n\n";
    // --------------------------

    winrt::com_ptr<ID3D11Device> d3dDevice;

    winrt::com_ptr<ID3D11DeviceContext> d3dContext;
    winrt::com_ptr<IDXGIDevice> dxgiDevice;
    D3D_FEATURE_LEVEL selectedFeatureLevel = D3D_FEATURE_LEVEL_11_1;

    bool okD3D = SetupD3D(d3dDevice, d3dContext, selectedFeatureLevel);
    bool okDXGI = SetupDXGI(d3dDevice, dxgiDevice);
    if (!okD3D || !okDXGI)
    {
        std::wcerr << L"[main] Failed to init D3D or DXGI.\n";
        return -1;
    }

    auto winrtDevice = createIDirect3DDevice(dxgiDevice);
    if (!winrtDevice)
    {
        std::wcerr << L"[main] Failed to create IDirect3DDevice.\n";
        return -1;
    }

    Sleep(2000);
    //Get the window handle     //HWND hwnd = fetchForegroundWindow();
    //std::wcout << L"[main] Found " << windows.size() << L" windows.\n";
  
	//Enumerate All Windows, Then From The Enumerated Windows Find The Windows With The Process Name "cs2.exe"
    auto windows = EnumerateAllWindows();
    //auto msedge = FindWindowsByProcessName(L"vlc.exe");
    auto msedge = FindWindowsByProcessName(wideTargetProcessName.c_str());
    std::wcout << L"[main] Found " << msedge.size() << L" CS2 windows.\n";
    for (auto& w : msedge) {
        std::wcout << L"[main] HWND = " << w.hwnd << L"\n Title = " << w.title << L"\n Process = " << w.processName << L"\n";
    }

    if (msedge.empty())
    {
        std::wcerr << L"[main] No window with the specified process name found.\n";
        return -1;
    }

	HWND hwnd = msedge[0].hwnd;
    if (!hwnd)
    {
        std::wcerr << L"[main] Could not get a valid hwnd.\n";
        return -1;
    }
    std::wcout << L"[main] Got hwnd: " << hwnd << std::endl;
    // Log current client size and optionally enforce target client area
    int cW = 0, cH = 0;
    if (GetClientAreaSize(hwnd, cW, cH)) {
        std::wcout << L"[main] Initial client area: " << cW << L"x" << cH << std::endl;
    }
    // If requested resolution from config and client is smaller, resize client area
    int targetW = 1920, targetH = 1080;
    bool resizeClient = true;
    try {
        if (config.contains("host") && config["host"].contains("window")) {
            auto wcfg = config["host"]["window"];
            if (wcfg.contains("targetWidth")) targetW = wcfg["targetWidth"].get<int>();
            if (wcfg.contains("targetHeight")) targetH = wcfg["targetHeight"].get<int>();
            if (wcfg.contains("resizeClientArea")) resizeClient = wcfg["resizeClientArea"].get<bool>();
        }
    } catch (...) {}
    if (resizeClient && (cW < targetW || cH < targetH)) {
        if (SetWindowClientAreaSize(hwnd, targetW, targetH)) {
            std::wcout << L"[main] Resized window client area to " << targetW << L"x" << targetH << std::endl;
            // Re-read client rect
            if (GetClientAreaSize(hwnd, cW, cH)) {
                std::wcout << L"[main] New client area: " << cW << L"x" << cH << std::endl;
            }
        } else {
            std::wcout << L"[main] Failed to resize window client area." << std::endl;
        }
    }

    auto item = CreateCaptureItemForWindow(hwnd);
    if (!item)
    {
        std::wcerr << L"[main] Failed to create capture item.\n";
        return -1;
    }

    auto size = item.Size();
    auto framePool = createFreeThreadedFramePool(winrtDevice, size);
    if (!framePool)
    {
        std::wcerr << L"[main] Could not create free-threaded frame pool.\n";
        return -1;
    }

    auto session = createCaptureSession(item, framePool);
    if (!session)
    {
        std::wcerr << L"[main] Could not create capture session.\n";
        return -1;
    }

    auto token = FrameArrivedEventRegistration(framePool);

    session.StartCapture();
    // Configure encoder defaults from config
    try {
        if (config.contains("host") && config["host"].contains("video")) {
            auto vcfg = config["host"]["video"];
            int fps = vcfg.value("fps", 120);
            int brStart = vcfg.value("bitrateStart", 20000000);
            int brMin = vcfg.value("bitrateMin", 10000000);
            int brMax = vcfg.value("bitrateMax", 50000000);
            Encoder::SetBitrateConfig(brStart, brMin, brMax);
            Encoder::ConfigureBitrateController(brMin, brMax,
                                               5'000'000, // increase step
                                               300,       // decrease cooldown
                                               3,         // clean samples required
                                               1000);     // increase interval
            SetCaptureTargetFps(fps);
            // Optional color range control (default full range true)
            bool fullRange = vcfg.value("fullRange", true);
            Encoder::SetFullRangeColor(fullRange);
            // Optional fixed pacing
            if (vcfg.contains("pacingFixedUs")) {
                Encoder::SetPacingFixedUs(std::max(1, vcfg["pacingFixedUs"].get<int>()));
            } else if (vcfg.contains("pacingFps")) {
                Encoder::SetPacingFps(std::max(1, vcfg["pacingFps"].get<int>()));
            }
            // Optional PLI policy
            bool ignorePli = vcfg.value("ignorePli", false);
            int minPliIntervalMs = vcfg.value("minPliIntervalMs", 500);
            double minLossThreshold = vcfg.value("minPliLossThreshold", 0.03);
            Encoder::ConfigurePliPolicy(ignorePli, minPliIntervalMs, minLossThreshold);
            // Optional: configure encoder hardware frame pool size
            if (vcfg.contains("hwFramePoolSize")) {
                int pool = vcfg["hwFramePoolSize"].get<int>();
                Encoder::SetHwFramePoolSize(pool);
            }
            // Optional: NVENC tuning knobs
            std::string preset = vcfg.value("preset", std::string("p5"));
            std::string rc     = vcfg.value("rc", std::string("cbr"));
            int bf             = vcfg.value("bf", 0);
            int rcLookahead    = vcfg.value("rcLookahead", 0);
            int asyncDepth     = vcfg.value("asyncDepth", 2);
            int surfaces       = vcfg.value("surfaces", 8);
            Encoder::SetNvencOptions(preset.c_str(), rc.c_str(), bf, rcLookahead, asyncDepth, surfaces);
        }
        if (config.contains("host") && config["host"].contains("capture")) {
            auto ccfg = config["host"]["capture"];
            if (ccfg.contains("maxQueueDepth")) {
                SetMaxQueuedFrames(std::max(1, ccfg["maxQueueDepth"].get<int>()));
            }
            // (Removed) copyPoolSize configuration (unused)
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
                int prio = mcfg.value("priority", 2); // 0..3
                SetMmcssConfig(enable, prio);
            }
            if (ccfg.contains("minUpdateInterval100ns")) {
                long long interval = 0;
                try { interval = ccfg["minUpdateInterval100ns"].get<long long>(); } catch (...) { interval = 0; }
                SetMinUpdateInterval100ns(interval);
            }
        }
    } catch (...) {}
    StartCapture();
    initWebsocket(roomId);
    AudioCapturer audioCapturer;
    audioCapturer.StartCapture(msedge[0].processId);
    std::wcout << L"[main] Capture started! Press any key to stop.\n";

    // Main loop with better monitoring
    auto lastMonitorTime = std::chrono::steady_clock::now();
    while (!ShutdownManager::IsShutdown()) {
        monitorConnection();
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
    StopCapture(token, framePool);
    try { closePeerConnection(); } catch (...) {}
    stopWebsocket();
    session.Close();
    // Encoder is finalized inside StopCapture(); avoid flushing/finalizing after free
    closeGo(); 

    return 0;
}
