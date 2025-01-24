//#pragma once
//#include <winrt/Windows.Foundation.h>
//#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>
//#include <d3d11.h>
//#include <dxgi.h>
#include <iostream>

#pragma once
#include <d3d11_4.h>
#include <initguid.h> // Required to define GUIDs
#include <Windows.Graphics.DirectX.Direct3D11.interop.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <windows.graphics.capture.interop.h>
#include <unknwn.h>

#include <Windows.h>
#include <winrt/Windows.UI.WindowManagement.h>
#include <winrt/Windows.UI.Core.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")

DEFINE_GUID(CLSID_GraphicsCaptureItemInterop,
    0x79c3f95b, 0x31f7, 0x4ec2, 0xa4, 0x56, 0xcd, 0x93, 0x85, 0xa6, 0x4c, 0x57);

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

HWND fetchForegroundWindow() {
    HWND foregroundWindow = NULL;
    try {
        std::wcout << L"[fetchForegroundWindow] Fetching the foreground window..." << std::endl;
        foregroundWindow = GetForegroundWindow();
        if (foregroundWindow == NULL) {
            std::wcerr << L"[fetchForegroundWindow] GetForegroundWindow returned NULL." << std::endl;
            return NULL;
        }
    }catch (winrt::hresult_error& e) {
        std::wcerr << L"[fetchForegroundWindow] Failed to fetch the foreground window. HRESULT: 0x" << std::hex << e.code() << std::endl;
    }
    return foregroundWindow;
}

uint64_t GetWindowIdFromHWND(HWND hwnd) {
    if (!hwnd) {
		std::wcerr << L"[GetWindowIdFromHWND] Invalid HWND." << std::endl;
		return NULL;
    }
	return reinterpret_cast<uint64_t>(hwnd);
}

winrt::Windows::Graphics::Capture::GraphicsCaptureItem CreateCaptureItemForWindow(HWND hwnd) {
    using namespace winrt::Windows::Graphics::Capture;

    if (!hwnd || !IsWindow(hwnd)) {
        throw std::invalid_argument("Invalid HWND passed to CreateCaptureItemForWindow.");
    }

    try {
        std::wcout << L"[CreateCaptureItemForWindow] Creating a GraphicsCaptureItem for the window..." << std::endl;
        auto interopFactory = winrt::get_activation_factory<
            winrt::Windows::Graphics::Capture::GraphicsCaptureItem,
            IGraphicsCaptureItemInterop>();

        if (!interopFactory) {
            throw std::runtime_error("Failed to get the IGraphicsCaptureItemInterop factory.");
        }

        std::wcout << L"[CreateCaptureItemForWindow] Activation factory obtained successfully." << std::endl;

        winrt::Windows::Graphics::Capture::GraphicsCaptureItem item{ nullptr };

        HRESULT hr = interopFactory->CreateForWindow(
            hwnd,
            winrt::guid_of<winrt::Windows::Graphics::Capture::GraphicsCaptureItem>(),
            winrt::put_abi(item)
        );

        if (FAILED(hr)) {
            std::wcerr << L"[CreateCaptureItemForWindow] Failed to create GraphicsCaptureItem for the window. HRESULT: 0x" << std::hex << hr << std::endl;
            return nullptr;
        }

        std::wcout << L"[CreateCaptureItemForWindow] GraphicsCaptureItem created successfully." << std::endl;
        return item;
    }
    catch (const winrt::hresult_error& e) {
        std::wcerr << L"[CreateCaptureItemForWindow] Failed to create GraphicsCaptureItem for the window. HRESULT: 0x" << std::hex << e.code() << std::endl;
    }
    catch (const std::exception& ex) {
        std::cerr << "[CreateCaptureItemForWindow] std::exception: " << ex.what() << std::endl;
    }
    catch (...) {
        std::wcerr << L"[CreateCaptureItemForWindow] Unknown exception occurred." << std::endl;
        throw;
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
    bool successD3D = SetupD3D(d3dDevice, d3dContext, selectedFeatureLevel);
    bool successDXGI = SetupDXGI(d3dDevice);
    
    if (successD3D) {
        std::wcout << L"[main] SetupD3D succeeded." << std::endl;
    }
    else {
        std::wcerr << L"[main] SetupD3D failed." << std::endl;
        return -1;
    }

    if (successDXGI) {
        std::wcout << L"[main] SetupDXGI succeeded." << std::endl;
    }
    else {
        std::wcerr << L"[main] SetupDXGI failed." << std::endl;
        return -1;
    }

    std::wcout << L"[main] SetupD3D and DXGIDevice succeeded. You can now use the D3D/DXGIDevice device/context." << std::endl;
    HWND hwnd = fetchForegroundWindow();
    std::wcout << L"[main] Foreground window handle and windowID: " << hwnd << std::endl;
    uint64_t windowID = GetWindowIdFromHWND(hwnd);
	std::wcout << L"[main] Window ID: " << windowID << std::endl;
    auto item = CreateCaptureItemForWindow(hwnd);
	std::wcout << L"[main] GraphicsCaptureItem created successfully." << std::endl;
    
    return 0;
}
