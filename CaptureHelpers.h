#pragma once

#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>

winrt::Windows::Graphics::Capture::Direct3D11CaptureFramePool
createD3DFramePool(
    winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice d3dDevice,
    winrt::Windows::Graphics::SizeInt32 size);

winrt::Windows::Graphics::Capture::Direct3D11CaptureFramePool
createFreeThreadedFramePool(
    winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice d3dDevice,
    winrt::Windows::Graphics::SizeInt32 size);

winrt::Windows::Graphics::Capture::GraphicsCaptureSession
createCaptureSession(
    winrt::Windows::Graphics::Capture::GraphicsCaptureItem item,
    winrt::Windows::Graphics::Capture::Direct3D11CaptureFramePool framePool
);

winrt::event_token FrameArrivedEventRegistration(
    winrt::Windows::Graphics::Capture::Direct3D11CaptureFramePool const& framePool
);
