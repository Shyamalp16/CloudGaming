#include "CaptureHelpers.h"
#include <chrono>
#include <iostream>
#include <wincodec.h>
#include <string>
#include <filesystem>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.DirectX.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>
#include <windows.graphics.directx.direct3d11.interop.h>
#include <d3d11.h>
#include <winrt/base.h>

using namespace std::chrono;
using namespace winrt;
using namespace winrt::Windows::Graphics::Capture;
using namespace winrt::Windows::Graphics::DirectX;
using namespace winrt::Windows::Graphics::DirectX::Direct3D11;
using namespace winrt::Windows::Foundation;

//Global Variables
std::vector<std::thread> workerThreads;
ThreadSafeQueue<winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DSurface> frameQueue;
std::atomic<bool> isCapturing{ false };

static auto g_lastFrameTime = steady_clock::now();
static constexpr auto g_minInterval = 16ms; //~60fps

static int g_currentWidth = 0;  // Initialize to 0 or any default value
static int g_currentHeight = 0; // Initialize to 0 or any default value

winrt::com_ptr<ID3D11Texture2D> GetTextureFromSurface(
    winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DSurface surface)
{
    // Obtain the native DXGI interface for the surface
    auto dxgiInterfaceAccess = surface.as<::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();

    winrt::com_ptr<ID3D11Texture2D> texture;
    winrt::check_hresult(dxgiInterfaceAccess->GetInterface(
        __uuidof(ID3D11Texture2D),
        texture.put_void()));  // Get the ID3D11Texture2D interface

    return texture;
}

void SaveTextureAsPNG(winrt::com_ptr<ID3D11Device> device, winrt::com_ptr<ID3D11Texture2D> texture, const std::wstring& filePath){
	//Create a WIC factory
    winrt::com_ptr<IWICImagingFactory> wicFactory;
    HRESULT hr = CoCreateInstance(
        CLSID_WICImagingFactory,
        nullptr,
        CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(wicFactory.put())
    );

    if (FAILED(hr)) {
        std::wcerr << L"[SaveTextureAsPng] Failed to create WIC factory. HRESULT=0x"
            << std::hex << hr << std::endl;
        return;
    }

    //Map texture to memory
	winrt::com_ptr<ID3D11DeviceContext> context;
	(device)->GetImmediateContext(context.put());

    winrt::com_ptr<ID3D11Query> query;
    D3D11_QUERY_DESC queryDesc = {};
    queryDesc.Query = D3D11_QUERY_EVENT;
    hr = device->CreateQuery(&queryDesc, query.put());
    if (FAILED(hr)) {
        std::wcerr << L"[SaveTextureAsPng] Failed to create query. HRESULT=0x"
            << std::hex << hr << std::endl;
        return;
    }

    D3D11_TEXTURE2D_DESC desc;
	(texture)->GetDesc(&desc);

    //Create staging texture
	desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
	desc.Usage = D3D11_USAGE_STAGING;
	desc.BindFlags = 0;
	desc.MiscFlags = 0;

	winrt::com_ptr<ID3D11Texture2D> stagingTexture;
	
	hr = device->CreateTexture2D(&desc, nullptr, stagingTexture.put());
    if (FAILED(hr)) {
        std::wcerr << L"[SaveTextureAsPng] Failed to create staging texture. HRESULT=0x"
            << std::hex << hr << std::endl;
        return;
    }
    context->CopyResource(stagingTexture.get(), texture.get());

    context->End(query.get());
    //Busyloop to wait for frame to finish rendering
    while (S_FALSE == context->GetData(query.get(), nullptr, 0, 0)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1)); // Sleep Briefly
    }

	//Copy texture to staging texture
    D3D11_MAPPED_SUBRESOURCE mapped = {};
	hr = context->Map(stagingTexture.get(), 0, D3D11_MAP_READ, 0, &mapped);
    if (FAILED(hr)) {
        std::wcerr << L"[SaveTextureAsPng] Failed to map texture. HRESULT=0x"
            << std::hex << hr << std::endl;
        return;
    }

	winrt::com_ptr<IWICBitmap> wicBitmap;
	hr = wicFactory->CreateBitmapFromMemory(
		desc.Width,
		desc.Height,
        GUID_WICPixelFormat32bppBGRA,
		mapped.RowPitch,
		mapped.RowPitch * desc.Height,
		static_cast<BYTE*>(mapped.pData),
		wicBitmap.put()
	);

	if (FAILED(hr)) {
        std::wcerr << L"[SaveTextureAsPng] Failed to create WIC bitmap. HRESULT=0x"
            << std::hex << hr << std::endl;
        return;
	}

	context->Unmap(stagingTexture.get(), 0);

    
    //Creatw a WIC stream for the file
	winrt::com_ptr<IWICStream> wicStream;
	hr = wicFactory->CreateStream(wicStream.put());
	if (FAILED(hr)) {
        std::wcerr << L"[SaveTextureAsPng] Failed to create WIC stream. HRESULT=0x"
            << std::hex << hr << std::endl;
        return;
	}

	hr = wicStream->InitializeFromFilename(filePath.c_str(), GENERIC_WRITE);
	if (FAILED(hr)) {
        std::wcerr << L"[SaveTextureAsPng] Failed to initialize WIC stream. HRESULT=0x"
            << std::hex << hr << std::endl;
        return;
	}

	//Create a PNG encoder
	winrt::com_ptr<IWICBitmapEncoder> wicEncoder;
	hr = wicFactory->CreateEncoder(GUID_ContainerFormatPng, nullptr, wicEncoder.put());
	if (FAILED(hr)) {
        std::wcerr << L"[SaveTextureAsPng] Failed to create WIC encoder. HRESULT=0x"
            << std::hex << hr << std::endl;
        return;
	}

	hr = wicEncoder->Initialize(wicStream.get(), WICBitmapEncoderNoCache);
	if (FAILED(hr)) {
        std::wcerr << L"[SaveTextureAsPng] Failed to initialize encoder. HRESULT=0x"
            << std::hex << hr << std::endl;
        return;
	}

	//Create a frame
	winrt::com_ptr<IWICBitmapFrameEncode> frameEncode;
	winrt::com_ptr<IPropertyBag2> options;
	hr = wicEncoder->CreateNewFrame(frameEncode.put(), options.put());
	if (FAILED(hr)) {
        std::wcerr << L"[SaveTextureAsPng] Failed to create encoder frame. HRESULT=0x"
            << std::hex << hr << std::endl;
        return;
	}

	hr = frameEncode->Initialize(options.get());
	if (FAILED(hr)) {
		std::wcerr << L"[SaveTextureAsPng] Failed to initialize frame. HRESULT=0x"
			<< std::hex << hr << std::endl;
		return;
	}

	hr = frameEncode->WriteSource(wicBitmap.get(), nullptr);
	if (FAILED(hr)) {
		std::wcerr << L"[SaveTextureAsPng] Failed to write source. HRESULT=0x"
			<< std::hex << hr << std::endl;
		return;
	}


	hr = frameEncode->Commit();
	if (FAILED(hr)) {
		std::wcerr << L"[SaveTextureAsPng] Failed to commit frame. HRESULT=0x"
			<< std::hex << hr << std::endl;
		return;
	}

	hr = wicEncoder->Commit();
	if (FAILED(hr)) {
		std::wcerr << L"[SaveTextureAsPng] Failed to commit encoder. HRESULT=0x"
			<< std::hex << hr << std::endl;
		return;
	}

	std::wcout << L"[SaveTextureAsPng] Saved texture to " << filePath << std::endl;
}

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
	//session.IsBorderRequired(true);
    return session;
}

// Register FrameArrived
//winrt::event_token FrameArrivedEventRegistration(
//    Direct3D11CaptureFramePool const& framePool)
//    //ThreadSafeQueue<winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DSurface>& frameQueue
//{
//    std::wcout << L"[FrameArrivedEventRegistration] Registering...\n";
//    auto handler = TypedEventHandler<
//        Direct3D11CaptureFramePool,
//        winrt::Windows::Foundation::IInspectable>(
//            [](auto&& sender, auto&&)
//            {
//                auto now = steady_clock::now();
//                if (now - g_lastFrameTime < g_minInterval) {
//                    //Skip if new frame dropped too early
//                    return;
//                }
//                g_lastFrameTime = now;
//
//                //Then we go to the next frame
//                std::wcout << L"[FrameArrived] We got a frame!\n";
//                auto frame = sender.TryGetNextFrame();
//                if (!frame) {
//                    std::wcout << L"[FrameArrived] Failed to get the frame.\n";
//                    return;
//                }
//
//                if (contentSize.Width < g_currentWidth || contentSize.Height < g_currentHeight) {
//                    std::wcout << L"[FrameArrived] Skipping Incomplete Frame.\n";
//                    return;
//                }
//
//                if (frame)
//                {
//                    auto time = frame.SystemRelativeTime();/*
//                    std::wcout << L"[FrameArrived] Frame time=" << time.count() << std::endl;*/
//                    auto surface = frame.Surface(); // Access the Direct3D surface
//                    SaveTextureAsPNG(GetD3DDevice(), GetTextureFromSurface(surface), L"frame.png");
//                    frameQueue.push(surface);
//                }
//            }
//        );
//    auto token = framePool.FrameArrived(handler);
//    return token;
//}

winrt::event_token FrameArrivedEventRegistration(Direct3D11CaptureFramePool const& framePool)
{
    std::wcout << L"[FrameArrivedEventRegistration] Registering...\n";

    auto handler = TypedEventHandler<Direct3D11CaptureFramePool, winrt::Windows::Foundation::IInspectable>(
        [](auto&& sender, auto&&)
        {
            auto now = steady_clock::now();
            if (now - g_lastFrameTime < g_minInterval) {
                // Skip frame if it’s too soon for the desired FPS limit
                return;
            }
            g_lastFrameTime = now;

            // Retrieve the next frame
            auto frame = sender.TryGetNextFrame();
            if (!frame) {
                std::wcout << L"[FrameArrived] Failed to get frame.\n";
                return;
            }

            auto contentSize = frame.ContentSize();
            std::wcout << L"[FrameArrived] Frame ContentSize: "
                << contentSize.Width << L"x" << contentSize.Height << std::endl;

            // Check if frame size is incomplete or invalid
            if (contentSize.Width < g_currentWidth || contentSize.Height < g_currentHeight) {
                std::wcout << L"[FrameArrived] Skipping incomplete frame.\n";
                return;
            }

            // Update current size if changed
            g_currentWidth = contentSize.Width;
            g_currentHeight = contentSize.Height;

            // Retrieve the surface and process it
            auto surface = frame.Surface();
            if (!surface) {
                std::wcout << L"[FrameArrived] Surface is null. Skipping frame.\n";
                return;
            }

            // Save the frame as PNG
            try {
                SaveTextureAsPNG(GetD3DDevice(), GetTextureFromSurface(surface), L"frame.png");
                frameQueue.push(surface);
                std::wcout << L"[FrameArrived] Frame processed and enqueued.\n";
            }
            catch (const std::exception& e) {
                std::wcerr << L"[FrameArrived] Exception during frame processing: "
                    << e.what() << std::endl;
            }
        });

    // Register the handler and return the token
    auto token = framePool.FrameArrived(handler);
    return token;
}

void ProcessFrames() {
    while (isCapturing.load()) {
		auto surface = frameQueue.pop();
        if (!surface) {
            break;
        }else{
			std::wcout << L"[ProcessFrames] Processing the frame!!\n";
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

void StopCapture(winrt::event_token& token, winrt::Windows::Graphics::Capture::Direct3D11CaptureFramePool const& framePool) {
	//Unsubscribe from FrameArrived
    std::wcout << L"[StopCapture] Stopping capture...\n";
    framePool.FrameArrived(token);
    std::wcout << L"[StopCapture] Unsubscribed From FrameArrived...\n";

    isCapturing.store(false);
    std::wcout << L"[StopCapture] isCapture set to false...\n";

    //Pushing N sentinal so each thread can exit
	for (int i = 0; i < workerThreads.size(); i++) {
		frameQueue.push(nullptr);
	}

    for (auto& thread : workerThreads)
    {
        if (thread.joinable())
        {
            thread.join();
        }
    }

	std::wcout << L"[StopCapture] Worker threads joined.\n";

    std::wcout << L"[StopCapture] Clearing FrameQueue.\n";
    while (!frameQueue.empty()) {
        frameQueue.pop();
    }
    std::wcout << L"[StopCapture] FrameQueue Cleared.\n";
}


// Manually define the interface IDirect3DDxgiInterfaceAccess:
// (It must match the actual Windows SDK interface GUID and signature)
//struct __declspec(uuid("A9B3D012-3DF2-4EE3-B7D5-7B2E9EAFB321"))
//    IDirect3DDxgiInterfaceAccess : public ::IUnknown
//{
//    virtual HRESULT __stdcall GetInterface(REFIID id, void** object) = 0;
//};

//winrt::com_ptr<ID3D11Texture2D> GetTextureFromSurface(
//    winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DSurface const& surface)
//{
//    // If 'surface' is invalid, just return null
//    if (!surface)
//    {
//        return nullptr;
//    }
//
//    // 1. Convert the WinRT object to IInspectable, then query for our manual interface
//
//    auto unknown = surface.as<winrt::Windows::Foundation::IInspectable>();
//    auto* rawInspectable = reinterpret_cast<::IUnknown*>(winrt::get_abi(unknown));
//    winrt::com_ptr<IDirect3DDxgiInterfaceAccess> dxgiInterfaceAccess;
//    winrt::check_hresult(
//        rawInspectable->QueryInterface(
//            __uuidof(IDirect3DDxgiInterfaceAccess),
//            reinterpret_cast<void**>(dxgiInterfaceAccess.put())
//        )
//	);
//
//
//    // 2. From the dxgiInterfaceAccess, retrieve ID3D11Texture2D:
//    winrt::com_ptr<ID3D11Texture2D> texture;
//    winrt::check_hresult(
//        dxgiInterfaceAccess->GetInterface(__uuidof(ID3D11Texture2D),
//            texture.put_void())
//    );
//
//    return texture;
//}
