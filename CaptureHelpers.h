#pragma once

#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>
#include "FrameCaptureThread.h"


//Create D3DFramePool (NOT USED)
winrt::Windows::Graphics::Capture::Direct3D11CaptureFramePool
createD3DFramePool(
    winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice d3dDevice,
    winrt::Windows::Graphics::SizeInt32 size);

//Create FreeThreadedFramePool
winrt::Windows::Graphics::Capture::Direct3D11CaptureFramePool
createFreeThreadedFramePool(
    winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice d3dDevice,
    winrt::Windows::Graphics::SizeInt32 size);

//Create CaptureSession
winrt::Windows::Graphics::Capture::GraphicsCaptureSession
createCaptureSession(
    winrt::Windows::Graphics::Capture::GraphicsCaptureItem item,
    winrt::Windows::Graphics::Capture::Direct3D11CaptureFramePool framePool
);

//Register for FrameArrived
winrt::event_token FrameArrivedEventRegistration(
    winrt::Windows::Graphics::Capture::Direct3D11CaptureFramePool const& framePool
);
//ThreadSafeQueue<winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DSurface>& frameQueue

//Create Worker Threads
//std::vector<std::thread> workerThreads;
void StartCapture();

void StopCapture();
void ProcessFrames();
