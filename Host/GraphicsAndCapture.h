#pragma once

#include <d3d11.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>
#include <winrt/Windows.Graphics.Capture.h>

namespace GraphicsAndCapture {
    struct D3DContext {
        winrt::com_ptr<ID3D11Device> device;
        winrt::com_ptr<ID3D11DeviceContext> context;
        winrt::com_ptr<IDXGIDevice> dxgiDevice;
        winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice winrtDevice{ nullptr };
        D3D_FEATURE_LEVEL featureLevel = D3D_FEATURE_LEVEL_11_1;
    };

    bool InitializeDevice(D3DContext& out);

    struct CaptureContext {
        winrt::Windows::Graphics::Capture::GraphicsCaptureItem item{ nullptr };
        winrt::Windows::Graphics::Capture::Direct3D11CaptureFramePool framePool{ nullptr };
        winrt::Windows::Graphics::Capture::GraphicsCaptureSession session{ nullptr };
        winrt::event_token frameArrivedToken{};
    };

    bool InitializeCapture(CaptureContext& cap, const D3DContext& d3d, winrt::Windows::Graphics::Capture::GraphicsCaptureItem item);
    void Start(CaptureContext& cap);
    void Stop(CaptureContext& cap);
}


