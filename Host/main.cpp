
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

// RTCP Callback Function
void onRTCP(double packetLoss, double rtt, double jitter) {
    std::wcout << L"[main] RTCP Stats - Packet Loss: " << packetLoss 
               << L", RTT: " << rtt 
               << L", Jitter: " << jitter << std::endl;

    // Simple bitrate adjustment logic
    // This is a basic example. A more sophisticated algorithm would be needed for production.
    static int currentBitrate = 30000000; // Initial bitrate
    if (packetLoss > 0.05 && currentBitrate > 1000000) {
        currentBitrate *= 0.8; // Decrease bitrate by 20%
        Encoder::AdjustBitrate(currentBitrate);
    } else if (packetLoss < 0.02 && currentBitrate < 50000000) {
        currentBitrate *= 1.1; // Increase bitrate by 10%
        Encoder::AdjustBitrate(currentBitrate);
    }
}

// Function to monitor the WebRTC connection state
void monitorConnection() {
    int state = getPeerConnectionState();
    if (state == 4 || state == 5 || state == 6) { // Disconnected, Failed, Closed
        std::wcout << L"[main] Peer has disconnected. Shutting down." << std::endl;
        ShutdownManager::SetShutdown(true);
    }
}

int main()
{
    winrt::init_apartment(winrt::apartment_type::multi_threaded);
    std::wcout << L"[main] Apartment initialized.\n";

    initGo(); 
    SetRTCPCallback(onRTCP); 

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
    std::string outputAudioFile = config["host"]["outputAudioFile"].get<std::string>();
    std::wstring wideTargetProcessName(targetProcessName.begin(), targetProcessName.end());
    std::wstring wideOutputAudioFile(outputAudioFile.begin(), outputAudioFile.end());

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
    audioCapturer.StartCapture(wideOutputAudioFile, msedge[0].processId);
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
    audioCapturer.StopCapture();
    stopWebsocket();
    StopCapture(token, framePool);
    session.Close();
    framePool.Close();
    Encoder::FlushEncoder();
    closeGo(); 

    return 0;
}
