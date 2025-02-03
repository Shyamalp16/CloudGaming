#include "CaptureHelpers.h"
#include "Encoder.h"
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
//ThreadSafeQueue<winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DSurface> frameQueue;
std::atomic<bool> isCapturing{ false };

struct FrameData {
    int sequenceNumber;
    winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DSurface surface;
};

struct FrameComparator {
    bool operator()(const FrameData& a, FrameData& b) {
        return a.sequenceNumber > b.sequenceNumber;
    };
};

std::priority_queue<FrameData, std::vector<FrameData>, FrameComparator> framePriorityQueue;
std::atomic<int> frameSequenceCounter{ 0 }; 
std::mutex queueMutex;
std::condition_variable queueCV;

int frameCounter = 0;

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

void SaveTextureAsPNG(winrt::com_ptr<ID3D11Device> device, winrt::com_ptr<ID3D11Texture2D> texture, const std::wstring& filePath, int frameCounter) {
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

    std::wstring newFilePath = filePath + L"_" + std::to_wstring(frameCounter) + L".png";
    hr = wicStream->InitializeFromFilename(newFilePath.c_str(), GENERIC_WRITE);
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
    std::wcout << L"[SaveTextureAsPNG] Saved texture to " << newFilePath << std::endl;
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

winrt::event_token FrameArrivedEventRegistration(Direct3D11CaptureFramePool const& framePool) {
    std::wcout << L"[FrameArrivedEventRegistration] Registering frame arrival handler...\n";

    auto handler = TypedEventHandler<Direct3D11CaptureFramePool, winrt::Windows::Foundation::IInspectable>(
        [](Direct3D11CaptureFramePool sender, winrt::Windows::Foundation::IInspectable) {
            try {
                // Retrieve the next frame
                auto frame = sender.TryGetNextFrame();
                if (!frame) {
                    std::wcout << L"[FrameArrived] Failed to retrieve frame.\n";
                    return;
                }

                auto surface = frame.Surface();
                if (!surface) {
                    std::wcout << L"[FrameArrived] Frame surface is null, skipping.\n";
                    return;
                }

                // Assign a sequence number and enqueue the frame
                int sequenceNumber = frameSequenceCounter++;
                {
                    std::lock_guard<std::mutex> lock(queueMutex);
                    framePriorityQueue.push({ sequenceNumber, surface });
                }
                queueCV.notify_one(); // Notify worker threads

                std::wcout << L"[FrameArrived] Frame enqueued with sequence number: " << sequenceNumber << std::endl;
            }
            catch (const std::exception& e) {
                std::wcerr << L"[FrameArrived] Exception: " << e.what() << L"\n";
            }
        });
    return framePool.FrameArrived(handler);
}

//void ProcessFrames() {
//    while (true) {
//        std::unique_lock<std::mutex> lock(queueMutex);
//        queueCV.wait(lock, [&]() { return !frameQueue.empty() || !isCapturing.load(); });
//
//        // If capturing is stopped and queue is empty, exit the loop
//        if (!isCapturing.load() && frameQueue.empty()) {
//            break;
//        }
//
//        // Retrieve and process the next surface from the queue
//        if (!frameQueue.empty()) {
//            auto surface = frameQueue.pop();
//            lock.unlock();
//
//            if (!surface) {
//                break;  // Sentinel value detected
//            }
//
//            std::wcout << L"[ProcessFrames] Processing the frame!!\n";
//
//            // Save the frame
//            SaveTextureAsPNG(GetD3DDevice(), GetTextureFromSurface(surface), L"frame.png", frameCounter++);
//        }
//        else {
//            lock.unlock();
//        }
//    }
//    std::wcout << L"[ProcessFrames] Exiting.\n";
//}

void PreProcessFrameConversion(winrt::com_ptr<ID3D11Device> device, winrt::com_ptr<ID3D11Texture2D> texture, int sequenceNumber) {
    winrt::com_ptr<ID3D11DeviceContext> context;
    device->GetImmediateContext(context.put());
    D3D11_TEXTURE2D_DESC desc;
    texture->GetDesc(&desc);

    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = 0;
    desc.MiscFlags = 0;

    winrt::com_ptr<ID3D11Texture2D> stagingTexture;
    HRESULT hr = device->CreateTexture2D(&desc, nullptr, stagingTexture.put());
    if (FAILED(hr)) {
        std::wcerr << L"[ProcessFrames] Failed to create staging texture. HRESULT=0x";
    }

    context->CopyResource(stagingTexture.get(), texture.get());
	D3D11_MAPPED_SUBRESOURCE mapped = {};
	hr = context->Map(stagingTexture.get(), 0, D3D11_MAP_READ, 0, &mapped);
	if (FAILED(hr)) {
		std::wcerr << L"[ProcessFrames] Failed to map texture. HRESULT=0x";
	}
	
	auto bgraData = static_cast<uint8_t*>(mapped.pData);
	int bgraPitch = mapped.RowPitch;

	Encoder::ConvertFrame(bgraData, bgraPitch, desc.Width, desc.Height);
	context->Unmap(stagingTexture.get(), 0);
    std::wcout << L"[ProcessFrames] Processed frame with sequence number: "
        << sequenceNumber << std::endl;
}

void ProcessFrames() {
    while (true) {
        FrameData frameData;

        {
            std::unique_lock<std::mutex> lock(queueMutex);
            queueCV.wait(lock, [&]() { return !framePriorityQueue.empty() || !isCapturing.load(); });

            // If capturing is stopped and queue is empty, exit the loop
            if (!isCapturing.load() && framePriorityQueue.empty()) {
                break;
            }

            // Get the next frame to process
            if (!framePriorityQueue.empty()) {
                frameData = framePriorityQueue.top();
                framePriorityQueue.pop();
            }
        }

        // Check for sentinel value to exit thread
        if (frameData.sequenceNumber == -1) {
            std::wcout << L"[ProcessFrames] Sentinel received, exiting thread.\n";
            break;
        }

        if (frameData.surface) {
            // Process the frame (e.g., save it)
            SaveTextureAsPNG(GetD3DDevice(), GetTextureFromSurface(frameData.surface), L"frame", frameData.sequenceNumber);
            std::wcout << L"[ProcessFrames] Processed frame with sequence number: " << frameData.sequenceNumber << std::endl;

			auto device = GetD3DDevice();
			auto texture = GetTextureFromSurface(frameData.surface);
            PreProcessFrameConversion(device, texture, frameData.sequenceNumber);
        }
    }
    std::wcout << L"[ProcessFrames] Exiting.\n";
    //TEST
}

//void ProcessFrames() {
//    while (isCapturing.load() || !frameQueue.empty()) {
//        std::unique_lock<std::mutex> lock(queueMutex);
//        queueCV.wait(lock, [&]() { return !frameQueue.empty() || !isCapturing.load(); });
//
//        if (!frameQueue.empty()) {
//            auto surface = frameQueue.pop();
//            lock.unlock();
//
//            if (!surface) {
//                break;  // Sentinel value detected, exit the loop
//            }
//
//            std::wcout << L"[ProcessFrames] Processing the frame!!\n";
//
//            // Process the frame (e.g., save it as PNG)
//            SaveTextureAsPNG(GetD3DDevice(), GetTextureFromSurface(surface), L"frame.png");
//        }
//        else {
//            lock.unlock();
//        }
//    }
//    std::wcout << L"[ProcessFrames] Exiting.\n";
//}

//void StartCapture() {
//    // Clear existing threads if any
//    for (auto& thread : workerThreads) {
//        if (thread.joinable()) {
//            thread.join();
//        }
//    }
//    workerThreads.clear();
//
//    isCapturing.store(true);
//    std::wcout << L"[StartCapture] Starting capture...\n";
//
//    // Create Worker Threads
//    int numThreads = std::thread::hardware_concurrency();
//    //int numThreads = 2;
//    for (int i = 0; i < numThreads; i++) {
//        workerThreads.emplace_back(std::thread(ProcessFrames));
//    }
//    std::wcout << L"[StartCapture] Capture started with " << numThreads << L" threads.\n";
//}

void StartCapture() {
    // Clear existing threads if any
    for (auto& thread : workerThreads) {
        if (thread.joinable()) {
            thread.join();
        }
    }
    workerThreads.clear();

    isCapturing.store(true);
    frameSequenceCounter.store(0); // Reset sequence counter
    std::wcout << L"[StartCapture] Starting capture...\n";

    int numThreads = std::thread::hardware_concurrency();
    for (int i = 0; i < numThreads; i++) {
        workerThreads.emplace_back(ProcessFrames);
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

    queueCV.notify_all();
    while (!framePriorityQueue.empty()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50)); // Allow time for processing
    }

    {
        std::lock_guard<std::mutex> lock(queueMutex);
        for (size_t i = 0; i < workerThreads.size(); i++) {
            framePriorityQueue.push({-1, nullptr});  // Add sentinel values to the queue
        }
    }

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
    std::lock_guard<std::mutex> lock(queueMutex);
    while (!framePriorityQueue.empty()) {
        framePriorityQueue.pop();
    }

    // Release the frame pool and session resources
    framePool.FrameArrived(token);
    framePool.Close();
    std::wcout << L"[StopCapture] Capture stopped and resources released.\n";
}

//void StopCapture(winrt::event_token& token, Direct3D11CaptureFramePool const& framePool) {
//    std::wcout << L"[StopCapture] Stopping capture...\n";
//
//    // Stop capturing new frames
//    isCapturing.store(false);
//
//    // Notify all worker threads to stop
//    {
//        std::lock_guard<std::mutex> lock(queueMutex);
//        for (size_t i = 0; i < workerThreads.size(); i++) {
//            framePriorityQueue.push({ -1, nullptr }); // Push sentinel value for each thread
//        }
//    }
//    queueCV.notify_all();
//
//    // Join all worker threads
//    for (auto& thread : workerThreads) {
//        if (thread.joinable()) {
//            thread.join();
//            std::wcout << L"[StopCapture] Worker thread joined.\n";
//        }
//    }
//    workerThreads.clear();
//
//    // Clear the frame queue (though it should be empty due to sentinels)
//    {
//        std::lock_guard<std::mutex> lock(queueMutex);
//        while (!framePriorityQueue.empty()) {
//            framePriorityQueue.pop();
//        }
//    }
//
//    // Release resources
//    framePool.FrameArrived(token);
//    framePool.Close();
//
//    std::wcout << L"[StopCapture] Capture stopped and resources released.\n";
//}
