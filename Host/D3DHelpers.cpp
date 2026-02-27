#include "D3DHelpers.h"
#include <iostream>
#include <winrt/Windows.Foundation.h>
#include <d3d11.h>
#include <dxgi1_2.h> 
#include <Windows.Graphics.DirectX.Direct3D11.interop.h>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")

winrt::com_ptr<ID3D11Device> myD3DDevice;
UINT g_vendorId = 0; // Default to unknown

winrt::com_ptr<ID3D11Device> GetD3DDevice()
{
	return myD3DDevice;
}

UINT GetGpuVendorId()
{
    return g_vendorId;
}

std::wstring GetVendorName(UINT vendorId)
{
    switch (vendorId)
    {
        case 0x10DE:
            return L"NVIDIA";
        case 0x8086:
            return L"Intel";
        case 0x1002:
            return L"AMD";
        default:
            return L"Unknown";
    }
}

bool SetupD3D(
    winrt::com_ptr<ID3D11Device>& d3dDevice,
    winrt::com_ptr<ID3D11DeviceContext>& d3dContext,
    D3D_FEATURE_LEVEL& selectedFeatureLevel)
{
    try
    {
        // Create a DXGI factory
        winrt::com_ptr<IDXGIFactory1> dxgiFactory;
        winrt::check_hresult(CreateDXGIFactory1(__uuidof(IDXGIFactory1), dxgiFactory.put_void()));

        // Enumerate adapters
        winrt::com_ptr<IDXGIAdapter1> bestAdapter;
        SIZE_T maxDedicatedVideoMemory = 0;

        winrt::com_ptr<IDXGIAdapter1> currentAdapter;
        for (UINT i = 0; dxgiFactory->EnumAdapters1(i, currentAdapter.put()) != DXGI_ERROR_NOT_FOUND; ++i)
        {
            DXGI_ADAPTER_DESC1 desc;
            winrt::check_hresult(currentAdapter->GetDesc1(&desc));

            // Print adapter information
            std::wcout << L"[SetupD3D] Adapter " << i << L": " << desc.Description << std::endl;
            std::wcout << L"  Vendor: " << GetVendorName(desc.VendorId) << L" (ID: 0x" << std::hex << desc.VendorId << L")" << std::endl;
            std::wcout << L"  Dedicated Video Memory: " << desc.DedicatedVideoMemory / 1024 / 1024 << L" MB" << std::endl;

            // Choose the adapter with the most dedicated video memory
            if (desc.DedicatedVideoMemory > maxDedicatedVideoMemory)
            {
                maxDedicatedVideoMemory = desc.DedicatedVideoMemory;
                bestAdapter = currentAdapter;
            }

            currentAdapter = nullptr;
        }

        if (!bestAdapter)
        {
            std::wcerr << L"[SetupD3D] No suitable adapter found." << std::endl;
            return false;
        }

        DXGI_ADAPTER_DESC1 bestDesc;
        winrt::check_hresult(bestAdapter->GetDesc1(&bestDesc));
        g_vendorId = bestDesc.VendorId; // Store the vendor ID
        std::wcout << L"[SetupD3D] Selected Adapter: " << bestDesc.Description << std::endl;
        std::wcout << L"  Vendor: " << GetVendorName(bestDesc.VendorId) << std::endl;


        D3D_FEATURE_LEVEL featureLevels[] = {
            D3D_FEATURE_LEVEL_11_1,
            D3D_FEATURE_LEVEL_11_0,
            D3D_FEATURE_LEVEL_10_1,
            D3D_FEATURE_LEVEL_10_0
        };

        HRESULT hr = D3D11CreateDevice(
            bestAdapter.get(),              // selected adapter
            D3D_DRIVER_TYPE_UNKNOWN,
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
		myD3DDevice = d3dDevice;
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

        // Keep GPU thread priority at default (0). The game is protected from
        // GPU contention via the process priority being NORMAL; throttling the
        // GPU device to -5 was also delaying the VideoProcessor BLT (BGRA→NV12),
        // capping stream throughput at ~60fps under load.
        std::wcout << L"[SetupDXGI] GPU thread priority left at default (0)\n";

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