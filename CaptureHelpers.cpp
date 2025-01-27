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
std::mutex queueMutex;
std::condition_variable queueCV;

static auto g_lastFrameTime = steady_clock::now();
static constexpr auto g_minInterval = 16ms; //~30fps

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

void SaveTextureAsPNG(winrt::com_ptr<ID3D11Device> device, winrt::com_ptr<ID3D11Texture2D> texture, const std::wstring& filePath) {
    // Create a WIC factory
    winrt::com_ptr<IWICImagingFactory> wicFactory;
    HRESULT hr = CoCreateInstance(
        CLSID_WICImagingFactory,
        nullptr,
        CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(wicFactory.put())
    );

    if (FAILED(hr)) {
        std::wcerr << L"[SaveTextureAsPNG] Failed to create WIC factory. HRESULT=0x"
            << std::hex << hr << std::endl;
        return;
    }

    // Get the immediate context
    winrt::com_ptr<ID3D11DeviceContext> context;
    device->GetImmediateContext(context.put());

    // Get texture description
    D3D11_TEXTURE2D_DESC desc;
    texture->GetDesc(&desc);

    // Double-buffered staging textures
    static winrt::com_ptr<ID3D11Texture2D> stagingTexture1;
    static winrt::com_ptr<ID3D11Texture2D> stagingTexture2;
    static bool useFirst = true;

    // Toggle between buffers
    winrt::com_ptr<ID3D11Texture2D> currentStagingTexture = useFirst ? stagingTexture1 : stagingTexture2;
    useFirst = !useFirst;

    // Ensure the staging texture is valid and matches the frame size
    if (!currentStagingTexture || desc.Width != g_currentWidth || desc.Height != g_currentHeight) {
        g_currentWidth = desc.Width;
        g_currentHeight = desc.Height;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        desc.Usage = D3D11_USAGE_STAGING;
        desc.BindFlags = 0;
        desc.MiscFlags = 0;

        hr = device->CreateTexture2D(&desc, nullptr, currentStagingTexture.put());
        if (FAILED(hr)) {
            std::wcerr << L"[SaveTextureAsPNG] Failed to create staging texture. HRESULT=0x"
                << std::hex << hr << std::endl;
            return;
        }
    }

    if (!currentStagingTexture) {
        std::wcerr << "[SaveTextureAsPNG] CurrentStagingTexture Not Created.\n";
    }

    // Copy the texture to the staging texture
    context->CopyResource(currentStagingTexture.get(), texture.get());

    // Map the staging texture
    D3D11_MAPPED_SUBRESOURCE mapped = {};
    hr = context->Map(currentStagingTexture.get(), 0, D3D11_MAP_READ, 0, &mapped);
    if (FAILED(hr)) {
        std::wcerr << L"[SaveTextureAsPNG] Failed to map texture. HRESULT=0x"
            << std::hex << hr << std::endl;
        return;
    }

    // Create a WIC bitmap from the mapped texture data
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
        std::wcerr << L"[SaveTextureAsPNG] Failed to create WIC bitmap. HRESULT=0x"
            << std::hex << hr << std::endl;
        context->Unmap(currentStagingTexture.get(), 0);
        return;
    }

    // Unmap the staging texture
    context->Unmap(currentStagingTexture.get(), 0);

    // Create a WIC stream for the file
    winrt::com_ptr<IWICStream> wicStream;
    hr = wicFactory->CreateStream(wicStream.put());
    if (FAILED(hr)) {
        std::wcerr << L"[SaveTextureAsPNG] Failed to create WIC stream. HRESULT=0x"
            << std::hex << hr << std::endl;
        return;
    }

    hr = wicStream->InitializeFromFilename(filePath.c_str(), GENERIC_WRITE);
    if (FAILED(hr)) {
        std::wcerr << L"[SaveTextureAsPNG] Failed to initialize WIC stream. HRESULT=0x"
            << std::hex << hr << std::endl;
        return;
    }

    // Create a PNG encoder
    winrt::com_ptr<IWICBitmapEncoder> wicEncoder;
    hr = wicFactory->CreateEncoder(GUID_ContainerFormatPng, nullptr, wicEncoder.put());
    if (FAILED(hr)) {
        std::wcerr << L"[SaveTextureAsPNG] Failed to create WIC encoder. HRESULT=0x"
            << std::hex << hr << std::endl;
        return;
    }

    hr = wicEncoder->Initialize(wicStream.get(), WICBitmapEncoderNoCache);
    if (FAILED(hr)) {
        std::wcerr << L"[SaveTextureAsPNG] Failed to initialize encoder. HRESULT=0x"
            << std::hex << hr << std::endl;
        return;
    }

    // Create a frame in the encoder
    winrt::com_ptr<IWICBitmapFrameEncode> frameEncode;
    winrt::com_ptr<IPropertyBag2> options;
    hr = wicEncoder->CreateNewFrame(frameEncode.put(), options.put());
    if (FAILED(hr)) {
        std::wcerr << L"[SaveTextureAsPNG] Failed to create encoder frame. HRESULT=0x"
            << std::hex << hr << std::endl;
        return;
    }

    hr = frameEncode->Initialize(options.get());
    if (FAILED(hr)) {
        std::wcerr << L"[SaveTextureAsPNG] Failed to initialize frame. HRESULT=0x"
            << std::hex << hr << std::endl;
        return;
    }

    hr = frameEncode->WriteSource(wicBitmap.get(), nullptr);
    if (FAILED(hr)) {
        std::wcerr << L"[SaveTextureAsPNG] Failed to write source. HRESULT=0x"
            << std::hex << hr << std::endl;
        return;
    }

    hr = frameEncode->Commit();
    if (FAILED(hr)) {
        std::wcerr << L"[SaveTextureAsPNG] Failed to commit frame. HRESULT=0x"
            << std::hex << hr << std::endl;
        return;
    }

    hr = wicEncoder->Commit();
    if (FAILED(hr)) {
        std::wcerr << L"[SaveTextureAsPNG] Failed to commit encoder. HRESULT=0x"
            << std::hex << hr << std::endl;
        return;
    }

    std::wcout << L"[SaveTextureAsPNG] Saved texture to " << filePath << std::endl;
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

//winrt::event_token FrameArrivedEventRegistration(Direct3D11CaptureFramePool const& framePool)
//{
//    std::wcout << L"[FrameArrivedEventRegistration] Registering...\n";
//
//    auto handler = TypedEventHandler<Direct3D11CaptureFramePool, winrt::Windows::Foundation::IInspectable>(
//        [](auto&& sender, auto&&)
//        {
//            auto now = steady_clock::now();
//            if (now - g_lastFrameTime < g_minInterval) {
//                // Skip frame if it’s too soon for the desired FPS limit
//                return;
//            }
//            g_lastFrameTime = now;
//
//            // Retrieve the next frame
//            auto frame = sender.TryGetNextFrame();
//            if (!frame) {
//                std::wcout << L"[FrameArrived] Failed to get frame.\n";
//                return;
//            }
//
//            auto contentSize = frame.ContentSize();
//            std::wcout << L"[FrameArrived] Frame ContentSize: "
//                << contentSize.Width << L"x" << contentSize.Height << std::endl;
//
//            // Check if frame size is incomplete or invalid
//            if (contentSize.Width < g_currentWidth || contentSize.Height < g_currentHeight) {
//                std::wcout << L"[FrameArrived] Skipping incomplete frame.\n";
//                return;
//            }
//
//            // Update current size if changed
//            g_currentWidth = contentSize.Width;
//            g_currentHeight = contentSize.Height;
//
//            // Retrieve the surface and process it
//            auto surface = frame.Surface();
//            if (!surface) {
//                std::wcout << L"[FrameArrived] Surface is null. Skipping frame.\n";
//                return;
//            }
//
//            winrt::com_ptr <ID3D11Texture2D> texture = GetTextureFromSurface(surface);
//            winrt::com_ptr<ID3D11DeviceContext> context;
//            GetD3DDevice()->GetImmediateContext(context.put());
//
//            winrt::com_ptr<ID3D11Query> query;
//            D3D11_QUERY_DESC queryDesc = {};
//            queryDesc.Query = D3D11_QUERY_EVENT;
//            GetD3DDevice()->CreateQuery(&queryDesc, query.put());
//            context->End(query.get());
//            
//            while (S_FALSE == context->GetData(query.get(), nullptr, 0, 0)) {
//                std::this_thread::sleep_for(std::chrono::milliseconds(1));
//            }
//
//            // Save the frame as PNG
//            try {
//                /*SaveTextureAsPNG(GetD3DDevice(), GetTextureFromSurface(surface), L"frame.png");*/
//                frameQueue.push(surface);
//                ProcessFrames();
//                std::wcout << L"[FrameArrived] Frame processed and enqueued.\n";
//                std::this_thread::sleep_for(std::chrono::milliseconds(100));
//            }
//            catch (const std::exception& e) {
//                std::wcerr << L"[FrameArrived] Exception during frame processing: "
//                    << e.what() << std::endl;
//            }
//        });
//
//    // Register the handler and return the token
//    auto token = framePool.FrameArrived(handler);
//    return token;
//}

winrt::event_token FrameArrivedEventRegistration(Direct3D11CaptureFramePool const& framePool) {
    std::wcout << L"[FrameArrivedEventRegistration] Registering frame arrival handler...\n";

    // Lambda function to handle frame arrival events
    auto handler = TypedEventHandler<Direct3D11CaptureFramePool, winrt::Windows::Foundation::IInspectable>(
        [](Direct3D11CaptureFramePool sender, winrt::Windows::Foundation::IInspectable) {
            try {
                auto now = std::chrono::steady_clock::now();

                // Enforce the frame rate limit (e.g., ~60 FPS)
                if (now - g_lastFrameTime < g_minInterval) {
                    std::wcout << L"[FrameArrived] Skipping frame due to FPS limit.\n";
                    return;
                }
                g_lastFrameTime = now;

                // Retrieve the next frame
                auto frame = sender.TryGetNextFrame();
                if (!frame) {
                    std::wcout << L"[FrameArrived] Failed to retrieve frame.\n";
                    return;
                }

                // Get the frame's content size
                auto contentSize = frame.ContentSize();
                std::wcout << L"[FrameArrived] Frame ContentSize: "
                    << contentSize.Width << L"x" << contentSize.Height << std::endl;

                // Check if the frame's size is valid
                if (contentSize.Width < g_currentWidth || contentSize.Height < g_currentHeight) {
                    std::wcout << L"[FrameArrived] Incomplete frame received, skipping.\n";
                    return;
                }

                // Update the current frame size if it changes
                g_currentWidth = contentSize.Width;
                g_currentHeight = contentSize.Height;

                // Extract the frame's surface
                auto surface = frame.Surface();
                if (!surface) {
                    std::wcout << L"[FrameArrived] Frame surface is null, skipping.\n";
                    return;
                }

                // Push the frame surface into the processing queue
                {
                    std::lock_guard<std::mutex> lock(queueMutex);
                    frameQueue.push(surface);
                }
                queueCV.notify_one(); // Notify worker threads to process the frame

                std::wcout << L"[FrameArrived] Frame enqueued successfully.\n";

            }
            catch (const winrt::hresult_error& e) {
                std::wcerr << L"[FrameArrived] HRESULT error: " << e.message().c_str() << L" (0x"
                    << std::hex << e.code() << L")\n";
            }
            catch (const std::exception& e) {
                std::wcerr << L"[FrameArrived] Standard exception: " << e.what() << L"\n";
            }
            catch (...) {
                std::wcerr << L"[FrameArrived] Unknown exception occurred during frame processing.\n";
            }
        });

    // Register the handler and return the event token
    auto token = framePool.FrameArrived(handler);
    std::wcout << L"[FrameArrivedEventRegistration] Handler registered successfully.\n";
    return token;
}


//void ProcessFrames() {
//    while (true) {
//        std::unique_lock<std::mutex> lock(queueMutex);
//        queueCV.wait(lock, [&]() { return !frameQueue.empty() || !isCapturing.load(); });
//
//        if (!isCapturing.load() && frameQueue.empty()) {
//            break;  // Exit if capturing has stopped and the queue is empty
//        }
//
//        auto surface = frameQueue.pop();  // Retrieve surface from the 
//        lock.unlock();
//
//        if (!surface) {
//            break;  // Sentinel value detected, exit the loop
//        }
//
//        std::wcout << L"[ProcessFrames] Processing the frame!!\n";
//
//        // Process the frame (e.g., save it as PNG or perform any operation)
//        SaveTextureAsPNG(GetD3DDevice(), GetTextureFromSurface(surface), L"frame.png");
//    }
//}

void ProcessFrames() {
    while (isCapturing.load() || !frameQueue.empty()) {
        std::unique_lock<std::mutex> lock(queueMutex);
        queueCV.wait(lock, [&]() { return !frameQueue.empty() || !isCapturing.load(); });

        if (!frameQueue.empty()) {
            auto surface = frameQueue.pop();
            lock.unlock();

            if (!surface) {
                break;  // Sentinel value detected, exit the loop
            }

            std::wcout << L"[ProcessFrames] Processing the frame!!\n";

            // Process the frame (e.g., save it as PNG)
            SaveTextureAsPNG(GetD3DDevice(), GetTextureFromSurface(surface), L"frame.png");
        }
        else {
            lock.unlock();
        }
    }
    std::wcout << L"[ProcessFrames] Exiting.\n";
}




void StartCapture() {
    // Clear existing threads if any
    for (auto& thread : workerThreads) {
        if (thread.joinable()) {
            thread.join();
        }
    }
    workerThreads.clear();

    isCapturing.store(true);
    std::wcout << L"[StartCapture] Starting capture...\n";

    // Create Worker Threads
    int numThreads = std::thread::hardware_concurrency();
    for (int i = 0; i < numThreads; i++) {
        workerThreads.emplace_back(std::thread(ProcessFrames));
    }
    std::wcout << L"[StartCapture] Capture started with " << numThreads << L" threads.\n";
}


//void StopCapture(winrt::event_token& token, winrt::Windows::Graphics::Capture::Direct3D11CaptureFramePool const& framePool) {
//	//Unsubscribe from FrameArrived
//    auto startTime = steady_clock::now();
//    while (!frameQueue.empty()) {
//        std::wcout << L"[StopCapture] Waiting for the frame queue to empty.\n";
//        std::this_thread::sleep_for(std::chrono::milliseconds(100));
//    }
//
//    if (!frameQueue.empty()) {
//        std::wcerr << L"[StopCapture] Timeout reached while waiting for frame queue to empty.\n";
//    }
//
//    isCapturing.store(false);
//
//    // Notify threads to exit
//    {
//        std::lock_guard<std::mutex> lock(queueMutex);
//        for (size_t i = 0; i < workerThreads.size(); i++) {
//            frameQueue.push(nullptr);  // Sentinel value for each thread
//        }
//    }
//    queueCV.notify_all();  // Wake up all waiting threads
//
//
//    for (auto& thread : workerThreads) {
//        if (thread.joinable()) {
//            thread.join();
//        }
//    }
//    std::wcout << L"[StopCapture] Worker thread joined.\n";
//    workerThreads.clear();
//
//    framePool.FrameArrived(token);
//    framePool.Close();
//    std::wcout << L"[StopCapture] Capture stopped and resources released.\n";
//}

void StopCapture(winrt::event_token& token, winrt::Windows::Graphics::Capture::Direct3D11CaptureFramePool const& framePool) {
    std::wcout << L"[StopCapture] Stopping capture...\n";

    // Stop capturing new frames
    isCapturing.store(false);

    // Notify worker threads to stop waiting
    queueCV.notify_all();

    // Join all worker threads
    for (auto& thread : workerThreads) {
        if (thread.joinable()) {
            thread.join();
            std::wcout << L"[StopCapture] Worker thread joined.\n";
        }
    }
    workerThreads.clear();

    // Clear the frame queue
    while (!frameQueue.empty()) {
        frameQueue.pop();
    }

    // Release the frame pool and session resources
    framePool.FrameArrived(token);
    framePool.Close();

    std::wcout << L"[StopCapture] Capture stopped and resources released.\n";
}


    //std::wcout << L"[StopCapture] Stopping capture...\n";
    //try {
    //    framePool.FrameArrived(token);
    //    framePool.Close();
    //    std::wcout << L"[StopCapture] Unsubscribed From FrameArrived...\n";
    //    std::wcout << L"[StopCapture] isCapture set to false...\n";

    //    //Pushing N sentinal so each thread can exit
    //    for (int i = 0; i < workerThreads.size(); i++) {
    //        frameQueue.push(nullptr);
    //    }

    //    for (auto& thread : workerThreads)
    //    {
    //        if (thread.joinable())
    //        {
    //            thread.join();
    //            std::wcout << L"[StopCapture] Worker thread joined.\n";
    //        }
    //    }

    //    std::wcout << L"[StopCapture] Clearing FrameQueue.\n";
    //    while (!frameQueue.empty()) {
    //        frameQueue.pop();
    //    }
    //    std::wcout << L"[StopCapture] FrameQueue Cleared.\n";
    //}catch (const std::exception& e) {
    //    std::wcerr << L"[StopCapture] Exception during cleanup: " << e.what() << std::endl;
    //}catch (...) {
    //    std::wcerr << L"[StopCapture] Unknown Exception during cleanup.\n";
    //}
//}


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
