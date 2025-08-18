#include "CaptureHelpers.h"
#include "GlobalTime.h"
#include <chrono>
#include <iostream>
#include <wincodec.h>
#include <string>
#include <filesystem>
#include <windows.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.DirectX.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>
#include <windows.graphics.directx.direct3d11.interop.h>
#include <d3d11.h>
#include <winrt/base.h>
#include "Encoder.h"

using namespace std::chrono;
using namespace winrt;
using namespace winrt::Windows::Graphics::Capture;
using namespace winrt::Windows::Graphics::DirectX;
using namespace winrt::Windows::Graphics::DirectX::Direct3D11;
using namespace winrt::Windows::Foundation;

//Global Variables
std::vector<std::thread> workerThreads;
std::atomic<bool> isCapturing{ false };
// (Removed) advanced WGC pool tracking

struct FrameData {
    int sequenceNumber;
    winrt::com_ptr<ID3D11Texture2D> texture; // private copy for lifetime safety
    int64_t timestamp;
};

struct FrameComparator {
    bool operator()(const FrameData& a, FrameData& b) {
        return a.sequenceNumber > b.sequenceNumber;
    };
};

std::priority_queue<FrameData, std::vector<FrameData>, FrameComparator> framePriorityQueue;
static constexpr size_t kMaxQueuedFrames = 4;
std::atomic<int> frameSequenceCounter{ 0 }; 
std::mutex queueMutex;
std::condition_variable queueCV;

static std::atomic<int> g_targetFps{120};
void SetCaptureTargetFps(int fps) { if (fps > 0) g_targetFps.store(fps); }

winrt::com_ptr<ID3D11Texture2D> GetTextureFromSurface(
    winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DSurface surface)
{
    auto dxgiInterfaceAccess = surface.as<::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();
    winrt::com_ptr<ID3D11Texture2D> texture;
    winrt::check_hresult(dxgiInterfaceAccess->GetInterface(
        __uuidof(ID3D11Texture2D),
        texture.put_void()));
    return texture;
}

Direct3D11CaptureFramePool createFreeThreadedFramePool(
    Direct3D11::IDirect3DDevice d3dDevice,
    winrt::Windows::Graphics::SizeInt32 size)
{
    int numberOfBuffers = 2; // lower buffering to reduce latency
    auto pixelFormat = DirectXPixelFormat::B8G8R8A8UIntNormalized;
    Direct3D11CaptureFramePool framePool = nullptr;
    try
    {
        framePool = Direct3D11CaptureFramePool::CreateFreeThreaded(
            d3dDevice,
            pixelFormat,
            numberOfBuffers,
            size
        );
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
    GraphicsCaptureSession session = nullptr;
    try
    {
        session = framePool.CreateCaptureSession(item);
	session.IsCursorCaptureEnabled(true);
    }
    catch (const winrt::hresult_error& e)
    {
        std::wcerr << L"[createCaptureSession] Failed. HRESULT=0x"
            << std::hex << e.code() << std::endl;
    }
    return session;
}

winrt::event_token FrameArrivedEventRegistration(Direct3D11CaptureFramePool const& framePool) {
    auto handler = TypedEventHandler<Direct3D11CaptureFramePool, winrt::Windows::Foundation::IInspectable>(
        [](Direct3D11CaptureFramePool sender, winrt::Windows::Foundation::IInspectable) {
            try {
                auto frame = sender.TryGetNextFrame();
                if (!frame) return;

                // (Removed) pool Recreate; revert to previous behavior

                auto surface = frame.Surface();
                if (!surface) return;

                int sequenceNumber = frameSequenceCounter++;
                // Timestamp in microseconds for encoder/Go layer
                int64_t timestamp = duration_cast<microseconds>(steady_clock::now().time_since_epoch()).count();
                // Log ContentSize when it changes
                try {
                    auto cs = frame.ContentSize();
                    static std::atomic<int> lastLoggedW{ 0 };
                    static std::atomic<int> lastLoggedH{ 0 };
                    if (cs.Width != lastLoggedW.load() || cs.Height != lastLoggedH.load()) {
                        std::wcout << L"[WGC] ContentSize changed: " << cs.Width << L"x" << cs.Height << std::endl;
                        lastLoggedW.store(cs.Width);
                        lastLoggedH.store(cs.Height);
                    }
                } catch (...) {}
                {
                    // Copy frame to a private texture immediately to avoid lifetime issues
                    auto device = GetD3DDevice();
                    winrt::com_ptr<ID3D11DeviceContext> context;
                    device->GetImmediateContext(context.put());

                    auto srcTex = GetTextureFromSurface(surface);
                    D3D11_TEXTURE2D_DESC desc{};
                    srcTex->GetDesc(&desc);
                    // Ensure bind flags suitable for CopyResource and SRV reads
                    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
                    desc.Usage = D3D11_USAGE_DEFAULT;
                    desc.CPUAccessFlags = 0;
                    desc.MiscFlags = 0;

                    winrt::com_ptr<ID3D11Texture2D> copyTex;
                    if (SUCCEEDED(device->CreateTexture2D(&desc, nullptr, copyTex.put()))) {
                        context->CopyResource(copyTex.get(), srcTex.get());
                        std::lock_guard<std::mutex> lock(queueMutex);
                        if (framePriorityQueue.size() >= kMaxQueuedFrames) {
                            framePriorityQueue.pop();
                        }
                        framePriorityQueue.push({ sequenceNumber, copyTex, timestamp });
                    }
                }
                queueCV.notify_one();
            }
            catch (const std::exception& e) {
                std::wcerr << L"[FrameArrived] Exception: " << e.what() << L"\n";
            }
        });
    return framePool.FrameArrived(handler);
}

void ProcessFrames() {
    auto device = GetD3DDevice();
    winrt::com_ptr<ID3D11DeviceContext> context;
    device->GetImmediateContext(context.put());

    // Raise thread priority and register with MMCSS (Games profile) dynamically
    HANDLE hThread = GetCurrentThread();
    SetThreadPriority(hThread, THREAD_PRIORITY_HIGHEST);
    HMODULE hAvrt = LoadLibraryW(L"Avrt.dll");
    HANDLE hTask = nullptr;
    DWORD taskIndex = 0;
    if (hAvrt) {
        using PFN_AvSetMmThreadCharacteristicsW = HANDLE (WINAPI *)(LPCWSTR, LPDWORD);
        using PFN_AvSetMmThreadPriority = BOOL (WINAPI *)(HANDLE, int);
        using PFN_AvRevertMmThreadCharacteristics = BOOL (WINAPI *)(HANDLE);
        auto pSetChars = reinterpret_cast<PFN_AvSetMmThreadCharacteristicsW>(GetProcAddress(hAvrt, "AvSetMmThreadCharacteristicsW"));
        auto pSetPrio  = reinterpret_cast<PFN_AvSetMmThreadPriority>(GetProcAddress(hAvrt, "AvSetMmThreadPriority"));
        if (pSetChars) {
            hTask = pSetChars(L"Games", &taskIndex);
            if (hTask && pSetPrio) {
                // 2 corresponds to AVRT_PRIORITY_HIGH without needing headers
                pSetPrio(hTask, 2);
            }
        }
    }

    // Initialize encoder on first real frame with actual capture size
    static int lastInitW = 0;
    static int lastInitH = 0;

    while (true) {
        FrameData frameData;

        {
            std::unique_lock<std::mutex> lock(queueMutex);
            queueCV.wait(lock, [&]() { return !framePriorityQueue.empty() || !isCapturing.load(); });

            if (!isCapturing.load() && framePriorityQueue.empty()) break;

            if (!framePriorityQueue.empty()) {
                frameData = framePriorityQueue.top();
                framePriorityQueue.pop();
            }
        }

        if (frameData.sequenceNumber == -1) break;

        if (frameData.texture) {
            auto texture = frameData.texture;
            D3D11_TEXTURE2D_DESC desc{};
            texture->GetDesc(&desc);
            // Log texture desc size when it changes
            static int lastDescW = 0;
            static int lastDescH = 0;
            if ((int)desc.Width != lastDescW || (int)desc.Height != lastDescH) {
                std::wcout << L"[WGC] TextureDesc size: " << desc.Width << L"x" << desc.Height << std::endl;
                lastDescW = (int)desc.Width;
                lastDescH = (int)desc.Height;
            }
            // Ensure even dimensions for H.264
            int encW = static_cast<int>(desc.Width & ~1U);
            int encH = static_cast<int>(desc.Height & ~1U);
            // Debounce re-initialization: require stability across a few frames and a minimal time gap
            static int pendingW = 0, pendingH = 0, pendingCount = 0;
            static auto lastInitTime = std::chrono::steady_clock::now();
            if (lastInitW == 0 || lastInitH == 0) {
                Encoder::InitializeEncoder("output.mp4", encW, encH, g_targetFps.load());
                lastInitW = encW;
                lastInitH = encH;
                lastInitTime = std::chrono::steady_clock::now();
                pendingW = pendingH = pendingCount = 0;
            } else if (encW != lastInitW || encH != lastInitH) {
                if (encW == pendingW && encH == pendingH) {
                    pendingCount++;
                } else {
                    pendingW = encW;
                    pendingH = encH;
                    pendingCount = 1;
                }
                auto now = std::chrono::steady_clock::now();
                auto since = std::chrono::duration_cast<std::chrono::milliseconds>(now - lastInitTime).count();
                // Re-init only if the new size is seen in 3 consecutive frames and last init was at least 300ms ago
                if (pendingCount >= 3 && since >= 300) {
                    Encoder::InitializeEncoder("output.mp4", encW, encH, g_targetFps.load());
                    lastInitW = encW;
                    lastInitH = encH;
                    lastInitTime = now;
                    pendingW = pendingH = pendingCount = 0;
                }
            }
            // Create a Copyable SRV texture if the capture surface is not bindable
            if ((desc.BindFlags & (D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE)) == 0) {
                D3D11_TEXTURE2D_DESC copyDesc = desc;
                copyDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
                copyDesc.MiscFlags = 0;
                copyDesc.Usage = D3D11_USAGE_DEFAULT;
                copyDesc.CPUAccessFlags = 0;
                winrt::com_ptr<ID3D11Texture2D> copyTex;
                if (SUCCEEDED(GetD3DDevice()->CreateTexture2D(&copyDesc, nullptr, copyTex.put()))) {
                    // Ensure even dimensions
                    copyDesc.Width &= ~1U;
                    copyDesc.Height &= ~1U;
                    context->CopyResource(copyTex.get(), texture.get());
                    Encoder::EncodeFrame(copyTex.get(), context.get(), copyDesc.Width, copyDesc.Height, frameData.timestamp);
                } else {
                    Encoder::EncodeFrame(texture.get(), context.get(), desc.Width, desc.Height, frameData.timestamp);
                }
            } else {
                Encoder::EncodeFrame(texture.get(), context.get(), desc.Width, desc.Height, frameData.timestamp);
            }
        }
    }

    // Revert MMCSS and free library before exit
    if (hAvrt && hTask) {
        auto pRevert = reinterpret_cast<BOOL (WINAPI *)(HANDLE)>(GetProcAddress(hAvrt, "AvRevertMmThreadCharacteristics"));
        if (pRevert) { pRevert(hTask); }
    }
    if (hAvrt) {
        FreeLibrary(hAvrt);
    }
}

void StartCapture() {
    for (auto& thread : workerThreads) {
        if (thread.joinable()) thread.join();
    }
    workerThreads.clear();

    isCapturing.store(true);
    frameSequenceCounter.store(0);

    // Use a single processing thread to avoid contention in the encoder
    int numThreads = 1;
    for (int i = 0; i < numThreads; i++) {
        workerThreads.emplace_back(ProcessFrames);
    }
}

void StopCapture(winrt::event_token& token, winrt::Windows::Graphics::Capture::Direct3D11CaptureFramePool const& framePool) {
    isCapturing.store(false);
    queueCV.notify_all();

    {
        std::lock_guard<std::mutex> lock(queueMutex);
        for (size_t i = 0; i < workerThreads.size(); i++) {
            framePriorityQueue.push({-1, nullptr, 0});
        }
    }
    queueCV.notify_all();

    for (auto& thread : workerThreads) {
        if (thread.joinable()) thread.join();
    }
    workerThreads.clear();

    std::lock_guard<std::mutex> lock(queueMutex);
    while (!framePriorityQueue.empty()) framePriorityQueue.pop();

    framePool.FrameArrived(token);
    framePool.Close();
    Encoder::FinalizeEncoder();
}