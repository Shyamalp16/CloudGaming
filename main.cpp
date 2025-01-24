//#pragma once
//#include <winrt/Windows.Foundation.h>
//#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>
//#include <d3d11.h>
//#include <dxgi.h>
#include <iostream>

#pragma once
#include <d3d11_4.h>
#include <Windows.Graphics.DirectX.Direct3D11.interop.h>
#include <unknwn.h>

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")

// creates the D3D device/context, logs errors, and returns success/failure.
bool SetupD3D(
    winrt::com_ptr<ID3D11Device>& d3dDevice,
    winrt::com_ptr<ID3D11DeviceContext>& d3dContext,
    D3D_FEATURE_LEVEL& selectedFeatureLevel)
{
    try
    {
        // Define the feature levels we want to check for
        D3D_FEATURE_LEVEL featureLevels[] = {
            D3D_FEATURE_LEVEL_11_1,
            D3D_FEATURE_LEVEL_11_0,
            D3D_FEATURE_LEVEL_10_1,
            D3D_FEATURE_LEVEL_10_0
        };

        // Create a Direct3D 11 device and context
        HRESULT hr = D3D11CreateDevice(
            nullptr,                        // Adapter (nullptr = use default adapter)
            D3D_DRIVER_TYPE_HARDWARE,       // Driver type
            nullptr,                        // Software rasterizer (nullptr = not used)
            D3D11_CREATE_DEVICE_BGRA_SUPPORT, // Device creation flags
            featureLevels,                  // Feature levels
            ARRAYSIZE(featureLevels),       // Number of feature levels
            D3D11_SDK_VERSION,              // SDK version
            d3dDevice.put(),                // Pointer to the device
            &selectedFeatureLevel,          // Selected feature level
            d3dContext.put()                // Pointer to the context
        );

        if (FAILED(hr))
        {
            std::wcerr << L"[SetupD3D] Failed to create Direct3D device/context. HRESULT: 0x"
                << std::hex << hr << std::endl;
            return false;
        }

        std::wcout << L"[SetupD3D] D3D device/context created successfully." << std::endl;
        std::wcout << L"[SetupD3D] Selected Feature Level: " << selectedFeatureLevel << std::endl;

        return true; // Success
    }
    catch (const std::exception& ex)
    {
        std::cerr << "[SetupD3D] std::exception: " << ex.what() << std::endl;
        return false;
    }
    catch (const winrt::hresult_error& ex)
    {
        std::wcerr << L"[SetupD3D] hresult_error: " << ex.message().c_str() << std::endl;
        return false;
    }
    catch (...)
    {
        std::wcerr << L"[SetupD3D] Unknown exception occurred." << std::endl;
        return false;
    }
}


bool SetupDXGI(winrt::com_ptr<ID3D11Device> d3dDevice) {
    winrt::com_ptr<::IDXGIDevice> m_dxgiDevice;
	std::wcout << L"[SetupDXGI] Creating DXGIDevice..." << std::endl;

    if (!d3dDevice) {
		std::wcerr << L"[SetupDXGI] D3D device is null. Cannot create DXGIDevice." << std::endl;
        return false;
    }
    try {
		m_dxgiDevice = d3dDevice.as<::IDXGIDevice>();
		std::wcout << L"[SetupDXGI] DXGIDevice created successfully." << std::endl;
        return true;
    }
    catch (const winrt::hresult_error& e) {
		std::wcerr << L"[SetupDXGI] Failed to create DXGIDevice. HRESULT: 0x" << std::hex << e.code() << std::endl;
        return false;
    }
}


int main()
{
    // Initialize the COM apartment for the main thread
    winrt::init_apartment(winrt::apartment_type::single_threaded);
    std::wcout << L"[main] COM apartment initialized successfully." << std::endl;

    // Prepare our device/context com_ptrs
    winrt::com_ptr<ID3D11Device> d3dDevice;     //d3dDevice
	winrt::com_ptr<ID3D11DeviceContext> d3dContext;   //d3dContext
	winrt::com_ptr<IDXGIDevice> m_dxgiDevice; //dxgiDevice
    D3D_FEATURE_LEVEL selectedFeatureLevel = D3D_FEATURE_LEVEL_11_1; // A default initialization

    // Call the function that sets up D3D and DXGI
    std::wcout << L"[main] Setting up D3D device/context..." << std::endl;
    bool success = SetupD3D(d3dDevice, d3dContext, selectedFeatureLevel);
    bool success2 = SetupDXGI(d3dDevice);
    
    if (success) {
        std::wcout << L"[main] SetupD3D succeeded." << std::endl;
    }
    else {
        std::wcerr << L"[main] SetupD3D failed." << std::endl;
        return -1;
    }

    if (success2) {
        std::wcout << L"[main] SetupDXGI succeeded." << std::endl;
    }
    else {
        std::wcerr << L"[main] SetupDXGI failed." << std::endl;
        return -1;
    }

    std::wcout << L"[main] SetupD3D and DXGIDevice succeeded. You can now use the D3D/DXGIDevice device/context." << std::endl;

    // Everything else in your program can proceed here
    // Cleanup is automatic with winrt::com_ptr

    return 0;
}
