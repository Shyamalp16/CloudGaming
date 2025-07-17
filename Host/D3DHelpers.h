#pragma once

#include <winrt/base.h>          // for com_ptr
#include <d3d11_4.h>             // for ID3D11Device, etc.
#include <dxgi.h>                // for IDXGIDevice
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>

winrt::com_ptr<ID3D11Device> GetD3DDevice();
UINT GetGpuVendorId();

bool SetupD3D(
    winrt::com_ptr<ID3D11Device>& d3dDevice,
    winrt::com_ptr<ID3D11DeviceContext>& d3dContext,
    D3D_FEATURE_LEVEL& selectedFeatureLevel);

bool SetupDXGI(
    winrt::com_ptr<ID3D11Device> d3dDevice,
    winrt::com_ptr<IDXGIDevice>& dxgiDevice);

winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice
createIDirect3DDevice(
    winrt::com_ptr<IDXGIDevice> dxgiDevice
);
