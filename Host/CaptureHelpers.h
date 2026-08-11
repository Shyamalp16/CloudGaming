#pragma once

#include <d3d11.h>
#include "D3DHelpers.h"
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>
#include <cstdint>

class StreamProfileManager;

struct CaptureHealth {
    bool running = false;
    int targetFps = 0;
    double measuredCaptureFps = 0.0;
    double measuredEncodeFps = 0.0;
    std::uint64_t framesArrived = 0;
    std::uint64_t framesSelected = 0;
    std::uint64_t pacingSkips = 0;
    std::uint64_t overwriteDrops = 0;
    std::uint64_t backpressureSkips = 0;
    std::uint64_t outOfOrderFrames = 0;
    std::size_t queueDepth = 0;
};

winrt::com_ptr<ID3D11Texture2D> GetTextureFromSurface(winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DSurface surface);

//Create FreeThreadedFramePool
winrt::Windows::Graphics::Capture::Direct3D11CaptureFramePool
createFreeThreadedFramePool(
    winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice d3dDevice,
    winrt::Windows::Graphics::SizeInt32 size);

// Configure number of buffers in the WGC free-threaded frame pool
void SetFramePoolBuffers(int bufferCount);

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

void StopCapture(winrt::event_token& token, winrt::Windows::Graphics::Capture::Direct3D11CaptureFramePool const& framePool);

// Configure capture/encoder target FPS used when initializing the encoder
void SetCaptureTargetFps(int fps);

// Connects the runtime-owned profile authority to the single capture consumer.
void SetStreamProfileManager(StreamProfileManager* manager);
CaptureHealth GetCaptureHealth();

// Configure maximum queued frames for capture backpressure
void SetMaxQueuedFrames(int maxDepth);

// Configure the application-owned BGRA texture pool used to retain WGC frames.
void SetCopyPoolSize(int poolSize);

// Configure MMCSS usage and priority (0=LOW,1=NORMAL,2=HIGH,3=CRITICAL)
void SetMmcssConfig(bool enable, int priority);

// Configure cursor visibility for capture session
void SetCursorCaptureEnabled(bool enable);

// Configure border requirement for capture session (visual border)
void SetBorderRequired(bool required);

// Configure WGC session MinUpdateInterval in 100ns units (0 disables)
void SetMinUpdateInterval100ns(long long interval100ns);
