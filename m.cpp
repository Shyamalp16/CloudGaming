#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>
#include <d3d11.h>
#include <dxgi.h>
#include <iostream>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")

int main() {
    try {
        // Initialize the COM apartment for the main thread
        winrt::init_apartment(winrt::apartment_type::single_threaded);
        std::wcout << L"COM apartment initialized successfully." << std::endl;

        // Create a Direct3D 11 device and context
        D3D_FEATURE_LEVEL featureLevels[] = {
            D3D_FEATURE_LEVEL_11_1,
            D3D_FEATURE_LEVEL_11_0,
            D3D_FEATURE_LEVEL_10_1,
            D3D_FEATURE_LEVEL_10_0
        };

        D3D_FEATURE_LEVEL selectedFeatureLevel;
        winrt::com_ptr<ID3D11Device> d3dDevice;
        winrt::com_ptr<ID3D11DeviceContext> d3dContext;

        HRESULT hr = D3D11CreateDevice(
            nullptr, // Adapter (nullptr = use default adapter)
            D3D_DRIVER_TYPE_HARDWARE, // Driver type
            nullptr, // Software rasterizer (nullptr = not used)
            D3D11_CREATE_DEVICE_BGRA_SUPPORT, // Device creation flags
            featureLevels, // Feature levels
            ARRAYSIZE(featureLevels), // Number of feature levels
            D3D11_SDK_VERSION, // SDK version
            d3dDevice.put(), // Pointer to the device
            &selectedFeatureLevel, // Selected feature level
            d3dContext.put() // Pointer to the context
        );

        if (FAILED(hr)) {
            std::wcerr << L"Failed to create Direct3D device and context. HRESULT: " << hr << std::endl;
            return -1;
        }

        std::wcout << L"Direct3D 11 device and context created successfully." << std::endl;
        std::wcout << L"Selected Feature Level: " << selectedFeatureLevel << std::endl;

        // Cleanup is automatic with winrt::com_ptr
    }
    catch (const std::exception& ex) {
        std::cerr << "Exception: " << ex.what() << std::endl;
        return -1;
    }
    catch (const winrt::hresult_error& ex) {
        std::wcerr << L"HRESULT Exception: " << ex.message().c_str() << std::endl;
        return -1;
    }

    return 0;
}
