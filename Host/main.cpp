
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
#include "FrameCaptureThread.h"
#include "Websocket.h"
#include "AudioCapturer.h"
#include "ShutdownManager.h"

#include "IdGenerator.h"

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <iostream>
#include <conio.h>
#include <fstream>
#include <nlohmann/json.hpp>

#include "D3DHelpers.h"
#include "WindowHelpers.h"
#include "CaptureHelpers.h"
#include "FrameCaptureThread.h"
#include "Websocket.h"
#include "AudioCapturer.h"
#include "ShutdownManager.h"
#include "IdGenerator.h"
#include "pion_webrtc.h" 
#include "Encoder.h"

// PLI callback implemented in Encoder.cpp
extern "C" void OnPLI();

// RTCP Callback Function
void onRTCP(double packetLoss, double rtt, double jitter) {
    std::wcout << L"[main] RTCP Stats - Packet Loss: " << packetLoss 
               << L", RTT: " << rtt 
               << L", Jitter: " << jitter << std::endl;

    // Adaptive bitrate controller (AIMD) with cooldown
    static int currentBitrate = 30000000;           // start at 30 Mbps
    static const int minBitrate = 10000000;         // 10 Mbps
    static const int maxBitrate = 50000000;         // 50 Mbps
    static int cleanSamples = 0;                    // consecutive good reports
    static auto lastChange = std::chrono::steady_clock::now();

    auto now = std::chrono::steady_clock::now();
    auto since = std::chrono::duration_cast<std::chrono::milliseconds>(now - lastChange).count();

    // Multiplicative decrease on loss
    if (packetLoss >= 0.03) { // >=3% loss
        if (since >= 300) { // short cooldown for decreases
            // Heavier drop if severe loss
            double factor = (packetLoss >= 0.10) ? 0.6 : 0.8;
            int target = static_cast<int>(currentBitrate * factor);
            currentBitrate = std::max(minBitrate, target);
            Encoder::AdjustBitrate(currentBitrate);
            lastChange = now;
        }
        cleanSamples = 0;
        return;
    }

    // Additive increase when clean
    cleanSamples++;
    if (since >= 1000 && cleanSamples >= 3) { // every ~1s and 3 clean samples
        int step = 5'000'000; // +5 Mbps
        int target = currentBitrate + step;
        if (target <= maxBitrate) {
            currentBitrate = target;
            Encoder::AdjustBitrate(currentBitrate);
            lastChange = now;
        }
        cleanSamples = 0;
    }
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
        configFile >> config;
    } catch (const std::exception& e) {
        std::wcerr << L"[main] Error reading config.json: " << e.what() << std::endl;
        return -1;
    }

    std::string targetProcessName = config["host"]["targetProcessName"].get<std::string>();
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
    // If requested resolution is 1920x1080 and client is smaller, resize client area
    int targetW = 1920, targetH = 1080;
    if (cW < targetW || cH < targetH) {
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
    StartCapture();
    initWebsocket(roomId);
    AudioCapturer audioCapturer;
    audioCapturer.StartCapture(msedge[0].processId);
    std::wcout << L"[main] Capture started! Press any key to stop.\n";

    // Main loop
    while (!ShutdownManager::IsShutdown()) {
        monitorConnection();
        if (_kbhit()) { // Check for keyboard input
            std::wcout << L"[main] Key pressed. Shutting down." << std::endl;
            ShutdownManager::SetShutdown(true);
        }
        Sleep(100); // Sleep to avoid busy-waiting
    }

    std::wcout << L"[main] Stopping capture...\n";
    // Order shutdown to avoid races: stop capture -> close PC -> stop ws -> flush/close encoder -> close Go
    audioCapturer.StopCapture();
    StopCapture(token, framePool);
    try { closePeerConnection(); } catch (...) {}
    stopWebsocket();
    session.Close();
    framePool.Close();
    // Encoder is finalized inside StopCapture(); avoid flushing/finalizing after free
    closeGo(); 

    return 0;
}
