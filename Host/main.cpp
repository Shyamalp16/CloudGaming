﻿
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <iostream>
#include <conio.h>

#include "D3DHelpers.h"
#include "WindowHelpers.h"
#include "CaptureHelpers.h"
#include "FrameCaptureThread.h"
#include "Websocket.h"

int main()
{
    //Initialize C++/WinRT apartment
    winrt::init_apartment(winrt::apartment_type::multi_threaded);
    std::wcout << L"[main] Apartment initialized.\n";
    //Create D3D device + context
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

    //Convert to WinRT IDirect3DDevice
    auto winrtDevice = createIDirect3DDevice(dxgiDevice);
    if (!winrtDevice)
    {
        std::wcerr << L"[main] Failed to create IDirect3DDevice.\n";
        return -1;
    }

    Sleep(2000); // Give time to see the console output
    //Get the window handle     //HWND hwnd = fetchForegroundWindow();
    //std::wcout << L"[main] Found " << windows.size() << L" windows.\n";
  
	//Enumerate All Windows, Then From The Enumerated Windows Find The Windows With The Process Name "cs2.exe"
    auto windows = EnumerateAllWindows();
    //auto msedge = FindWindowsByProcessName(L"vlc.exe");
    auto msedge = FindWindowsByProcessName(L"vlc.exe");
    std::wcout << L"[main] Found " << msedge.size() << L" CS2 windows.\n";
    for (auto& w : msedge) {
        std::wcout << L"[main] HWND = " << w.hwnd << L"\n Title = " << w.title << L"\n Process = " << w.processName << L"\n";
    }

	//Set Cs2.exe window as the window to capture
	HWND hwnd = msedge[0].hwnd;
    if (!hwnd)
    {
        std::wcerr << L"[main] Could not get a valid hwnd.\n";
        return -1;
    }
    std::wcout << L"[main] Got hwnd: " << hwnd << std::endl;

    //Create capture item from that hwnd
    auto item = CreateCaptureItemForWindow(hwnd);
    if (!item)
    {
        std::wcerr << L"[main] Failed to create capture item.\n";
        return -1;
    }

    //Create a free-threaded frame pool
    auto size = item.Size();
    auto framePool = createFreeThreadedFramePool(winrtDevice, size);
    if (!framePool)
    {
        std::wcerr << L"[main] Could not create free-threaded frame pool.\n";
        return -1;
    }

    //Create a session from the frame pool & item
    auto session = createCaptureSession(item, framePool);
    if (!session)
    {
        std::wcerr << L"[main] Could not create capture session.\n";
        return -1;
    }

    //Register for FrameArrived
    auto token = FrameArrivedEventRegistration(framePool);

    //Start windows graphics capture
    session.StartCapture();

	//Start worker threads to process frames
    StartCapture();
    initWebsocket();
    std::wcout << L"[main] Capture started!\n";

    // Keep the app alive for 10 seconds to see frame events
    /*for (int i = 0; i < 10; i++)
    {
        Sleep(1000);
        std::wcout << L"[main] Still capturing...\n";
    } */

    std::wcout << L"[main] Press '0' to stop capturing";
    while (true) {
        if (_kbhit()) {
			char key = _getch();
			if (key == '0') {
				break;
			}
        }
        Sleep(100);
		std::wcout << L"[main] Still capturing...\n";
    }
    std::wcout << L"[main] Stopping capture...\n";
    StopCapture(token, framePool);
    //framePool.Close();
    session.Close();
    Encoder::FlushEncoder();

    //Optionally unsubscribe from the event
     framePool.FrameArrived(token);

    /*for (auto& w : windows) {
		std::wcout << L"[main] HWND = " << w.hwnd << L"\n Title = " << w.title << L"\n Process = " << w.processName << L".\n";
    }*/

	std::wcout << L"[main] Press any key...\n";
    std::wstring dummy;
	std::getline(std::wcin, dummy);
    return 0;
}
