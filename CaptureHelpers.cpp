#include "CaptureHelpers.h"
#include <iostream>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.DirectX.h>

using namespace winrt::Windows::Graphics::Capture;
using namespace winrt::Windows::Graphics::DirectX;
using namespace winrt::Windows::Foundation;

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
    int numberOfBuffers = 2;
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
    return session;
}

// Register FrameArrived
winrt::event_token FrameArrivedEventRegistration(
    Direct3D11CaptureFramePool const& framePool)
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
                }
            }
        );

    auto token = framePool.FrameArrived(handler);
    return token;
}
