#include "D3DHelpers.h"
#include <iostream>  // for wcout, wcerr
#include <winrt/Windows.Foundation.h>
#include <d3d11.h>
#include <dxgi1_2.h> // or dxgi.h
#include <Windows.Graphics.DirectX.Direct3D11.interop.h> // CreateDirect3D11DeviceFromDXGIDevice

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")

bool SetupD3D(
    winrt::com_ptr<ID3D11Device>& d3dDevice,
    winrt::com_ptr<ID3D11DeviceContext>& d3dContext,
    D3D_FEATURE_LEVEL& selectedFeatureLevel)
{
    try
    {
        D3D_FEATURE_LEVEL featureLevels[] = {
            D3D_FEATURE_LEVEL_11_1,
            D3D_FEATURE_LEVEL_11_0,
            D3D_FEATURE_LEVEL_10_1,
            D3D_FEATURE_LEVEL_10_0
        };

        HRESULT hr = D3D11CreateDevice(
            nullptr,                        // default adapter
            D3D_DRIVER_TYPE_HARDWARE,
            nullptr,
            D3D11_CREATE_DEVICE_BGRA_SUPPORT,
            featureLevels,
            ARRAYSIZE(featureLevels),
            D3D11_SDK_VERSION,
            d3dDevice.put(),
            &selectedFeatureLevel,
            d3dContext.put()
        );

        if (FAILED(hr))
        {
            std::wcerr << L"[SetupD3D] Failed. HRESULT=0x"
                << std::hex << hr << std::endl;
            return false;
        }

        std::wcout << L"[SetupD3D] Created D3D device & context. FeatureLevel="
            << selectedFeatureLevel << std::endl;
        return true;
    }
    catch (...)
    {
        std::wcerr << L"[SetupD3D] Exception creating device.\n";
        return false;
    }
}

bool SetupDXGI(
    winrt::com_ptr<ID3D11Device> d3dDevice,
    winrt::com_ptr<IDXGIDevice>& dxgiDevice)
{
    std::wcout << L"[SetupDXGI] Creating DXGIDevice...\n";
    if (!d3dDevice)
    {
        std::wcerr << L"[SetupDXGI] Null D3D device.\n";
        return false;
    }
    try
    {
        dxgiDevice = d3dDevice.as<IDXGIDevice>();
        std::wcout << L"[SetupDXGI] DXGIDevice created.\n";
        return true;
    }
    catch (const winrt::hresult_error& e)
    {
        std::wcerr << L"[SetupDXGI] Failed. HRESULT=0x"
            << std::hex << e.code() << std::endl;
        return false;
    }
}

winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice
createIDirect3DDevice(winrt::com_ptr<IDXGIDevice> dxgiDevice)
{
    std::wcout << L"[createIDirect3DDevice] Creating IDirect3DDevice...\n";
    winrt::com_ptr<IInspectable> deviceInspectable;
    try
    {
        winrt::check_hresult(
            CreateDirect3D11DeviceFromDXGIDevice(
                dxgiDevice.get(),
                deviceInspectable.put()
            )
        );
        auto direct3DDevice =
            deviceInspectable.as<winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice>();
        std::wcout << L"[createIDirect3DDevice] Success.\n";
        return direct3DDevice;
    }
    catch (const winrt::hresult_error& e)
    {
        std::wcerr << L"[createIDirect3DDevice] Failed. HRESULT=0x"
            << std::hex << e.code() << std::endl;
        return nullptr;
    }
}
