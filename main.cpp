#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <iostream>

#include "D3DHelpers.h"
#include "WindowHelpers.h"
#include "CaptureHelpers.h"

int main()
{
    // 1) Initialize C++/WinRT apartment
    winrt::init_apartment(winrt::apartment_type::single_threaded);
    std::wcout << L"[main] Apartment initialized.\n";

    // 2) Create D3D device + context
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

    // 3) Convert to WinRT IDirect3DDevice
    auto winrtDevice = createIDirect3DDevice(dxgiDevice);
    if (!winrtDevice)
    {
        std::wcerr << L"[main] Failed to create IDirect3DDevice.\n";
        return -1;
    }

    // 4) Get the window handle
    HWND hwnd = fetchForegroundWindow();
    if (!hwnd)
    {
        std::wcerr << L"[main] Could not get a valid hwnd.\n";
        return -1;
    }
    std::wcout << L"[main] Got hwnd: " << hwnd << std::endl;

    // 5) Create capture item from that hwnd
    auto item = CreateCaptureItemForWindow(hwnd);
    if (!item)
    {
        std::wcerr << L"[main] Failed to create capture item.\n";
        return -1;
    }

    // 6) Create a free-threaded frame pool
    auto size = item.Size();
    auto framePool = createFreeThreadedFramePool(winrtDevice, size);
    if (!framePool)
    {
        std::wcerr << L"[main] Could not create free-threaded frame pool.\n";
        return -1;
    }

    // 7) Create a session from the frame pool & item
    auto session = createCaptureSession(item, framePool);
    if (!session)
    {
        std::wcerr << L"[main] Could not create capture session.\n";
        return -1;
    }

    // 8) Register for FrameArrived
    auto token = FrameArrivedEventRegistration(framePool);

    // 9) Start capture
    session.StartCapture();
    std::wcout << L"[main] Capture started!\n";

    // Keep the app alive for 10 seconds to see frame events
    for (int i = 0; i < 10; i++)
    {
        Sleep(1000);
        std::wcout << L"[main] Still capturing...\n";
    }

    // 10) Optionally unsubscribe from the event
    // framePool.FrameArrived(token);

    return 0;
}
