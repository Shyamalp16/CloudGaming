#include "CaptureHelpers.h"
#include "GlobalTime.h"
#include <chrono>
#include <iostream>
#include <wincodec.h>
#include <string>
#include <filesystem>
#include <windows.h>
#include <queue>
#include <algorithm>
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
    int poolIndex = -1; // index in texture pool; -1 if not pooled
    int64_t timestamp;
};

struct FrameComparator {
    bool operator()(const FrameData& a, FrameData& b) {
        return a.sequenceNumber > b.sequenceNumber;
    };
};

std::priority_queue<FrameData, std::vector<FrameData>, FrameComparator> framePriorityQueue;
static size_t g_maxQueuedFrames = 4;
std::atomic<int> frameSequenceCounter{ 0 }; 
std::mutex queueMutex;
std::condition_variable queueCV;

static std::atomic<int> g_targetFps{120};
void SetCaptureTargetFps(int fps) { if (fps > 0) g_targetFps.store(fps); }

void SetMaxQueuedFrames(int maxDepth) {
    if (maxDepth < 1) maxDepth = 1;
    g_maxQueuedFrames = static_cast<size_t>(maxDepth);
}

// Backpressure drop policy defaults
static std::atomic<int> g_dropWindowMs{200};
static std::atomic<int> g_dropMinEvents{2};
void SetBackpressureDropPolicy(int windowMs, int minEvents) {
    if (windowMs < 1) windowMs = 1;
    if (minEvents < 1) minEvents = 1;
    g_dropWindowMs.store(windowMs);
    g_dropMinEvents.store(minEvents);
}

// MMCSS config
static std::atomic<bool> g_enableMmcss{true};
static std::atomic<int>  g_mmcssPriority{2}; // 2 ~ HIGH
void SetMmcssConfig(bool enable, int priority) {
    g_enableMmcss.store(enable);
    g_mmcssPriority.store(std::clamp(priority, 0, 3));
}

// Texture pool for copy surfaces (per resolution)
static std::vector<winrt::com_ptr<ID3D11Texture2D>> g_copyPool;
static std::vector<bool> g_poolInUse;
static std::mutex g_poolMutex;
static int g_poolNextIndex = 0;
static int g_poolW = 0, g_poolH = 0;
static int g_poolSize = 0;

static void RecreateCopyPool(ID3D11Device* device, int width, int height, DXGI_FORMAT format) {
    std::lock_guard<std::mutex> lock(g_poolMutex);
    g_poolW = width & ~1; g_poolH = height & ~1;
    g_poolSize = static_cast<int>(std::max<size_t>(g_maxQueuedFrames, 4));
    g_copyPool.clear();
    g_poolInUse.clear();
    g_copyPool.resize(g_poolSize);
    g_poolInUse.resize(g_poolSize);
    g_poolNextIndex = 0;
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = static_cast<UINT>(g_poolW);
    desc.Height = static_cast<UINT>(g_poolH);
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = format;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    desc.CPUAccessFlags = 0;
    desc.MiscFlags = 0;
    for (int i = 0; i < g_poolSize; ++i) {
        g_poolInUse[i] = false;
        device->CreateTexture2D(&desc, nullptr, g_copyPool[i].put());
    }
}

static int AcquirePoolSlot() {
    std::lock_guard<std::mutex> lock(g_poolMutex);
    if (g_copyPool.empty()) return -1;
    int start = g_poolNextIndex;
    for (int i = 0; i < g_poolSize; ++i) {
        int idx = (start + i) % g_poolSize;
        if (!g_poolInUse[idx]) {
            g_poolInUse[idx] = true;
            g_poolNextIndex = (idx + 1) % g_poolSize;
            return idx;
        }
    }
    return -1; // all busy
}

static void ReleasePoolSlot(int idx) {
    if (idx < 0) return;
    std::lock_guard<std::mutex> lock(g_poolMutex);
    g_poolInUse[static_cast<size_t>(idx)] = false;
}

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
                    // Copy frame to a private texture using pool to avoid per-frame allocations
                    auto device = GetD3DDevice();
                    winrt::com_ptr<ID3D11DeviceContext> context;
                    device->GetImmediateContext(context.put());

                    auto srcTex = GetTextureFromSurface(surface);
                    D3D11_TEXTURE2D_DESC srcDesc{};
                    srcTex->GetDesc(&srcDesc);

                    int evenW = static_cast<int>(srcDesc.Width & ~1U);
                    int evenH = static_cast<int>(srcDesc.Height & ~1U);
                    // Recreate pool on size change or first use
                    if (g_copyPool.empty() || g_poolW != evenW || g_poolH != evenH) {
                        RecreateCopyPool(device.get(), evenW, evenH, srcDesc.Format);
                        std::wcout << L"[WGC] Recreated copy pool for " << evenW << L"x" << evenH << std::endl;
                    }

                    int slot = AcquirePoolSlot();
                    if (slot >= 0) {
                        context->CopyResource(g_copyPool[slot].get(), srcTex.get());
                        std::lock_guard<std::mutex> lock(queueMutex);
                        if (framePriorityQueue.size() >= g_maxQueuedFrames) {
                            framePriorityQueue.pop();
                            static int dropWarnCounter = 0;
                            if ((++dropWarnCounter % 60) == 0) {
                                std::wcout << L"[WGC] Dropping frames due to full queue (depth=" << g_maxQueuedFrames << L")" << std::endl;
                            }
                        }
                        framePriorityQueue.push({ sequenceNumber, g_copyPool[slot], slot, timestamp });
                    } else {
                        // Pool exhausted: drop to preserve latency
                        static int poolDropWarn = 0;
                        if ((++poolDropWarn % 60) == 0) {
                            std::wcout << L"[WGC] Dropping frame (copy pool exhausted)" << std::endl;
                        }
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

    // Raise thread priority and optionally register with MMCSS (Games profile)
    HANDLE hThread = GetCurrentThread();
    SetThreadPriority(hThread, THREAD_PRIORITY_HIGHEST);
    HMODULE hAvrt = nullptr; HANDLE hTask = nullptr; DWORD taskIndex = 0;
    if (g_enableMmcss.load()) {
        hAvrt = LoadLibraryW(L"Avrt.dll");
        if (hAvrt) {
            using PFN_AvSetMmThreadCharacteristicsW = HANDLE (WINAPI *)(LPCWSTR, LPDWORD);
            using PFN_AvSetMmThreadPriority = BOOL (WINAPI *)(HANDLE, int);
            auto pSetChars = reinterpret_cast<PFN_AvSetMmThreadCharacteristicsW>(GetProcAddress(hAvrt, "AvSetMmThreadCharacteristicsW"));
            auto pSetPrio  = reinterpret_cast<PFN_AvSetMmThreadPriority>(GetProcAddress(hAvrt, "AvSetMmThreadPriority"));
            if (pSetChars) {
                hTask = pSetChars(L"Games", &taskIndex);
                if (hTask && pSetPrio) {
                    int prio = std::clamp(g_mmcssPriority.load(), 0, 3); // 0..3
                    pSetPrio(hTask, prio);
                }
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
                    if (!Encoder::IsBacklogged(g_dropWindowMs.load(), g_dropMinEvents.load())) {
                        Encoder::EncodeFrame(copyTex.get(), context.get(), copyDesc.Width, copyDesc.Height, frameData.timestamp);
                    } // else drop frame to reduce latency
                } else {
                    if (!Encoder::IsBacklogged(g_dropWindowMs.load(), g_dropMinEvents.load())) {
                        Encoder::EncodeFrame(texture.get(), context.get(), desc.Width, desc.Height, frameData.timestamp);
                    }
                }
            } else {
                if (!Encoder::IsBacklogged(g_dropWindowMs.load(), g_dropMinEvents.load())) {
                    Encoder::EncodeFrame(texture.get(), context.get(), desc.Width, desc.Height, frameData.timestamp);
                }
            }
            // Release pool slot if used
            if (frameData.poolIndex >= 0) {
                ReleasePoolSlot(frameData.poolIndex);
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