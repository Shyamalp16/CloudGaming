#include "CaptureHelpers.h"
#include "FrameCaptureThread.h"
#include <iostream>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.DirectX.h>

using namespace winrt::Windows::Graphics::Capture;
using namespace winrt::Windows::Graphics::DirectX;
using namespace winrt::Windows::Foundation;

//Global Variables
std::vector<std::thread> workerThreads;
ThreadSafeQueue<winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DSurface> frameQueue;
std::atomic<bool> isCapturing{ false };

// Create a normal (apartment-bound) frame pool
Direct3D11CaptureFramePool createD3DFramePool(
    Direct3D11::IDirect3DDevice d3dDevice,
    winrt::Windows::Graphics::SizeInt32 size)
{
    int numberOfBuffers = 2;
    auto pixelFormat = DirectXPixelFormat::B8G8R8A8UIntNormalized;

    std::wcout << L"[createD3DFramePool] Creating frame pool...\n";

    Direct3D11CaptureFramePool framePool = nullptr;
    try
    {
        framePool = Direct3D11CaptureFramePool::Create(
            d3dDevice,
            pixelFormat,
            numberOfBuffers,
            size
        );
        std::wcout << L"[createD3DFramePool] Success.\n";
    }
    catch (const winrt::hresult_error& e)
    {
        std::wcerr << L"[createD3DFramePool] Failed. HRESULT=0x"
            << std::hex << e.code() << std::endl;
    }
    return framePool;
}

// Create a free-threaded frame pool
Direct3D11CaptureFramePool createFreeThreadedFramePool(
    Direct3D11::IDirect3DDevice d3dDevice,
    winrt::Windows::Graphics::SizeInt32 size)
{
    int numberOfBuffers = 3;
    auto pixelFormat = DirectXPixelFormat::B8G8R8A8UIntNormalized;

    std::wcout << L"[createFreeThreadedFramePool] Creating free-threaded frame pool...\n";

    Direct3D11CaptureFramePool framePool = nullptr;
    try
    {
        framePool = Direct3D11CaptureFramePool::CreateFreeThreaded(
            d3dDevice,
            pixelFormat,
            numberOfBuffers,
            size
        );
        std::wcout << L"[createFreeThreadedFramePool] Success.\n";
    }
    catch (const winrt::hresult_error& e)
    {
        std::wcerr << L"[createFreeThreadedFramePool] Failed. HRESULT=0x"
            << std::hex << e.code() << std::endl;
    }

    return framePool;
}

GraphicsCaptureSession createCaptureSession(
    GraphicsCaptureItem item,
    Direct3D11CaptureFramePool framePool)
{
    std::wcout << L"[createCaptureSession] Creating session...\n";
    GraphicsCaptureSession session = nullptr;
    try
    {
        session = framePool.CreateCaptureSession(item);
        std::wcout << L"[createCaptureSession] Success.\n";
    }
    catch (const winrt::hresult_error& e)
    {
        std::wcerr << L"[createCaptureSession] Failed. HRESULT=0x"
            << std::hex << e.code() << std::endl;
    }
	session.IsCursorCaptureEnabled(true);
	session.IsBorderRequired(false);
    return session;
}

// Register FrameArrived
winrt::event_token FrameArrivedEventRegistration(
    Direct3D11CaptureFramePool const& framePool)
    //ThreadSafeQueue<winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DSurface>& frameQueue
{
    std::wcout << L"[FrameArrivedEventRegistration] Registering...\n";
    auto handler = TypedEventHandler<
        Direct3D11CaptureFramePool,
        IInspectable>(
            [](auto&& sender, auto&&)
            {
                std::wcout << L"[FrameArrived] We got a frame!\n";
                auto frame = sender.TryGetNextFrame();
                if (frame)
                {
                    auto time = frame.SystemRelativeTime();
                    std::wcout << L"[FrameArrived] Frame time=" << time.count() << std::endl;
                    auto surface = frame.Surface(); // Access the Direct3D surface
                    frameQueue.push(surface);
                }
            }
        );
    auto token = framePool.FrameArrived(handler);
    return token;
}

void ProcessFrames() {
    while (isCapturing.load()) {
		auto surface = frameQueue.pop();
        if (surface) {
			std::wcout << L"[ProcessFrames] Processing the frame!!\n";
			//Do something with the frame
        }
    }
}

void StartCapture() {
	isCapturing.store(true); 
	std::wcout << L"[StartCapture] Starting capture...\n";
	
    //Create Worker Threads
    int numThreads = std::thread::hardware_concurrency();
	for (int i = 0; i < numThreads; i++) {
		workerThreads.emplace_back(std::thread(ProcessFrames));
	}
	std::wcout << L"[StartCapture] Capture started with !" << numThreads << L"threads.\n";
}

void StopCapture() {
    std::wcout << L"[StopCapture] Stopping capture...\n";
    isCapturing.store(false);

    for (auto& thread : workerThreads)
    {
        if (thread.joinable())
        {
            thread.join();
        }
    }

	std::wcout << L"[StopCapture] Clearing FrameQueue.\n";
	while (!frameQueue.empty()) {
		frameQueue.pop();
	}
	std::wcout << L"[StopCapture] FrameQueue Cleared.\n";

    workerThreads.clear();
	std::wcout << L"[StopCapture] Worker threads stopped.\n";
    std::wcout << L"[StopCapture] Capture stopped.\n";
}