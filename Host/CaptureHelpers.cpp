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
    winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DSurface surface{ nullptr }; // deferred copy source
};

struct FrameComparator {
    bool operator()(const FrameData& a, FrameData& b) {
        return a.sequenceNumber > b.sequenceNumber;
    };
};

// Two-stage queues: surfaces from WGC callback -> copy worker -> encode queue
static std::priority_queue<FrameData, std::vector<FrameData>, FrameComparator> g_surfaceQueue;
static std::priority_queue<FrameData, std::vector<FrameData>, FrameComparator> g_encodeQueue;
static size_t g_maxQueuedFrames = 8;
std::atomic<int> frameSequenceCounter{ 0 }; 
static std::mutex g_surfaceMutex;
static std::condition_variable g_surfaceCV;
static std::mutex g_encodeMutex;
static std::condition_variable g_encodeCV;

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

// Session options
static std::atomic<bool> g_cursorCaptureEnabled{true};
static std::atomic<bool> g_borderRequired{true};
void SetCursorCaptureEnabled(bool enable) { g_cursorCaptureEnabled.store(enable); }
void SetBorderRequired(bool required) { g_borderRequired.store(required); }
// MinUpdateInterval (100ns units). 0 means not set
static std::atomic<long long> g_minUpdateInterval100ns{0};
void SetMinUpdateInterval100ns(long long interval100ns) {
    if (interval100ns < 0) interval100ns = 0;
    g_minUpdateInterval100ns.store(interval100ns);
}

// Texture pool for copy surfaces (per resolution)
static std::vector<winrt::com_ptr<ID3D11Texture2D>> g_copyPool;
static std::vector<bool> g_poolInUse;
static std::mutex g_poolMutex;
static int g_poolNextIndex = 0;
static int g_poolW = 0, g_poolH = 0;
static int g_poolSize = 0;

static DXGI_FORMAT CoerceFormatForVideoProcessor(DXGI_FORMAT fmt) {
    switch (fmt) {
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: return DXGI_FORMAT_B8G8R8A8_UNORM;
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: return DXGI_FORMAT_R8G8B8A8_UNORM;
    default: return fmt;
    }
}

static void RecreateCopyPool(ID3D11Device* device, int width, int height, DXGI_FORMAT format) {
    std::lock_guard<std::mutex> lock(g_poolMutex);
    g_poolW = width & ~1; g_poolH = height & ~1;
    g_poolSize = static_cast<int>(std::max<size_t>(g_maxQueuedFrames, 12));
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
    desc.Format = CoerceFormatForVideoProcessor(format);
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    desc.CPUAccessFlags = 0;
    desc.MiscFlags = 0;
    for (int i = 0; i < g_poolSize; ++i) {
        g_poolInUse[i] = false;
        device->CreateTexture2D(&desc, nullptr, g_copyPool[i].put());
    }
    std::wcout << L"[WGC] Created texture pool with " << g_poolSize << L" slots for " << g_poolW << L"x" << g_poolH << std::endl;
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

void SetCopyPoolSize(int poolSize) {
    if (poolSize < 2) poolSize = 2;
    if (poolSize > 16) poolSize = 16;
    std::lock_guard<std::mutex> lock(g_poolMutex);
    g_poolSize = poolSize;
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

static std::atomic<int> g_framePoolBuffers{3};
void SetFramePoolBuffers(int bufferCount) {
    if (bufferCount < 1) bufferCount = 1;
    if (bufferCount > 16) bufferCount = 16;
    g_framePoolBuffers.store(bufferCount);
}

Direct3D11CaptureFramePool createFreeThreadedFramePool(
    Direct3D11::IDirect3DDevice d3dDevice,
    winrt::Windows::Graphics::SizeInt32 size)
{
    int numberOfBuffers = g_framePoolBuffers.load();
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
        session.IsCursorCaptureEnabled(g_cursorCaptureEnabled.load());
        // Best-effort: some OS versions expose IsBorderRequired
        try { session.IsBorderRequired(g_borderRequired.load()); } catch (...) {}
        // Configure MinUpdateInterval if requested
        try {
            auto val = g_minUpdateInterval100ns.load();
            if (val > 0) {
                session.MinUpdateInterval(winrt::Windows::Foundation::TimeSpan{ val });
                std::wcout << L"[WGC] MinUpdateInterval set to " << val << L" (100ns units)" << std::endl;
            }
        } catch (...) {}
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
                // Prefer WGC SystemRelativeTime (100ns units) for stable, system-aligned timestamps
                int64_t timestamp = 0;
                try {
                    auto srt = frame.SystemRelativeTime();
                    // Convert 100ns units to microseconds
                    timestamp = static_cast<int64_t>(srt.count() / 10);
                } catch (...) {}
                if (timestamp <= 0) {
                    // Fallback to steady_clock if SRT unavailable
                    timestamp = duration_cast<microseconds>(steady_clock::now().time_since_epoch()).count();
                }
                // --- Debug: capture fps ---
                {
                    static int capCount = 0;
                    static int64_t lastTs = 0;
                    capCount++;
                    if (lastTs == 0) lastTs = timestamp;
                    if (timestamp - lastTs >= 1000000) { // 1s
                        std::wcout << L"[WGC] frames/s: " << capCount << std::endl;
                        capCount = 0;
                        lastTs = timestamp;
                    }
                }
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
                // Ultra-light: defer copy to worker thread; push surface + timestamp
                {
                    std::lock_guard<std::mutex> lock(g_surfaceMutex);
                    if (g_surfaceQueue.size() >= g_maxQueuedFrames) {
                        g_surfaceQueue.pop();
                    }
                    FrameData fd{};
                    fd.sequenceNumber = sequenceNumber;
                    fd.timestamp = timestamp;
                    fd.surface = surface; // keep alive for worker copy
                    g_surfaceQueue.push(fd);
                }
                g_surfaceCV.notify_one();
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

    // Fixed frame pacing: use high-resolution steady clock for precise timing
    static auto nextEncodeTime = std::chrono::steady_clock::now();
    while (true) {
        FrameData frameData;

        {
            std::unique_lock<std::mutex> lock(g_surfaceMutex);
            // Wait for frames or shutdown
            g_surfaceCV.wait(lock, [&]() { return !g_surfaceQueue.empty() || !isCapturing.load(); });
            if (!isCapturing.load() && g_surfaceQueue.empty()) break;

            // Calculate precise frame interval
            auto now = std::chrono::steady_clock::now();
            auto targetFps = g_targetFps.load();
            if (targetFps <= 0) targetFps = 120; // safety fallback
            auto intervalUs = 1000000LL / targetFps; // microseconds per frame
            auto interval = std::chrono::microseconds(intervalUs);

            // Initialize next encode time if not set
            if (nextEncodeTime.time_since_epoch().count() == 0) {
                nextEncodeTime = now;
            }

            // If we're ahead of schedule, wait for the exact next slot
            if (now < nextEncodeTime) {
                // Keep only the most recent frame to reduce latency
                while (g_surfaceQueue.size() > 1) {
                    auto drop = g_surfaceQueue.top();
                    g_surfaceQueue.pop();
                    if (drop.poolIndex >= 0) {
                        ReleasePoolSlot(drop.poolIndex);
                    }
                }

                // Wait until it's time to encode
                auto waitUntil = nextEncodeTime;
                g_surfaceCV.wait_until(lock, waitUntil, [&]() {
                    return !g_surfaceQueue.empty() || !isCapturing.load();
                });
            }

            // Recalculate timing after potential wait
            now = std::chrono::steady_clock::now();

            // If we're at or past the encode time, process the frame
            if (now >= nextEncodeTime) {
                // Drop older frames to keep only the freshest
                while (g_surfaceQueue.size() > 1) {
                    auto drop = g_surfaceQueue.top();
                    g_surfaceQueue.pop();
                    if (drop.poolIndex >= 0) {
                        ReleasePoolSlot(drop.poolIndex);
                    }
                }

                if (!g_surfaceQueue.empty()) {
                    frameData = g_surfaceQueue.top();
                    g_surfaceQueue.pop();
                }

                // Schedule next frame precisely
                nextEncodeTime += interval;

                // If we've fallen behind, catch up by scheduling from now
                if (nextEncodeTime <= now) {
                    nextEncodeTime = now + interval;
                }
            }
        }

        if (frameData.sequenceNumber == -1) break;

        if (frameData.texture || frameData.surface) {
            // If we deferred the copy, perform it now into the pool
            if (frameData.surface && !frameData.texture) {
                auto device = GetD3DDevice();
                winrt::com_ptr<ID3D11DeviceContext> context;
                device->GetImmediateContext(context.put());

                auto srcTex = GetTextureFromSurface(frameData.surface);
                D3D11_TEXTURE2D_DESC srcDesc{};
                srcTex->GetDesc(&srcDesc);
                int evenW = static_cast<int>(srcDesc.Width & ~1U);
                int evenH = static_cast<int>(srcDesc.Height & ~1U);
                if (g_copyPool.empty() || g_poolW != evenW || g_poolH != evenH) {
                    RecreateCopyPool(device.get(), evenW, evenH, CoerceFormatForVideoProcessor(srcDesc.Format));
                    std::wcout << L"[WGC] Recreated copy pool for " << evenW << L"x" << evenH << std::endl;
                }
                int slot = AcquirePoolSlot();
                if (slot >= 0) {
                    // Use a deferred context to reduce immediate-context contention
                    static thread_local winrt::com_ptr<ID3D11DeviceContext> deferred;
                    if (!deferred) {
                        winrt::check_hresult(device->CreateDeferredContext(0, deferred.put()));
                    }
                    deferred->CopyResource(g_copyPool[slot].get(), srcTex.get());
                    winrt::com_ptr<ID3D11CommandList> commandList;
                    winrt::check_hresult(deferred->FinishCommandList(FALSE, commandList.put()));
                    context->ExecuteCommandList(commandList.get(), FALSE);

                    frameData.texture = g_copyPool[slot];
                    frameData.poolIndex = slot;
                } else {
                    // Cannot copy now; drop this frame quickly to preserve latency
                    continue;
                }
            }
            // --- Debug: encode submit rate ---
            static auto lastLog = std::chrono::steady_clock::now();
            static int submitCount = 0;
            submitCount++;
            auto nowDbg = std::chrono::steady_clock::now();
            if (std::chrono::duration_cast<std::chrono::seconds>(nowDbg - lastLog).count() >= 1) {
                std::wcout << L"[Encode] submits/s: " << submitCount << L" (Target: " << g_targetFps.load() << L" FPS)" << std::endl;
                submitCount = 0;
                lastLog = nowDbg;
            }
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
                    } else {
                        // Log when encoder is backlogged
                        static auto lastBacklogLog = std::chrono::steady_clock::now();
                        static int backlogCount = 0;
                        backlogCount++;
                        auto now = std::chrono::steady_clock::now();
                        if (std::chrono::duration_cast<std::chrono::seconds>(now - lastBacklogLog).count() >= 5) {
                            std::wcout << L"[Encoder] Backlogged - dropped " << backlogCount << L" frames in last 5s" << std::endl;
                            backlogCount = 0;
                            lastBacklogLog = now;
                        }
                    }
                }
            } else {
                if (!Encoder::IsBacklogged(g_dropWindowMs.load(), g_dropMinEvents.load())) {
                    Encoder::EncodeFrame(texture.get(), context.get(), desc.Width, desc.Height, frameData.timestamp);
                } else {
                    // Log when encoder is backlogged
                    static auto lastBacklogLog2 = std::chrono::steady_clock::now();
                    static int backlogCount2 = 0;
                    backlogCount2++;
                    auto now2 = std::chrono::steady_clock::now();
                    if (std::chrono::duration_cast<std::chrono::seconds>(now2 - lastBacklogLog2).count() >= 5) {
                        std::wcout << L"[Encoder] Backlogged - dropped " << backlogCount2 << L" frames in last 5s" << std::endl;
                        backlogCount2 = 0;
                        lastBacklogLog2 = now2;
                    }
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

    // Launch one processing thread
    workerThreads.emplace_back(ProcessFrames);
}

void StopCapture(winrt::event_token& token, winrt::Windows::Graphics::Capture::Direct3D11CaptureFramePool const& framePool) {
    isCapturing.store(false);
    g_surfaceCV.notify_all();
    g_encodeCV.notify_all();

    {
        std::lock_guard<std::mutex> lock(g_surfaceMutex);
        g_surfaceQueue.push({-1, nullptr, 0});
    }
    g_surfaceCV.notify_all();

    for (auto& thread : workerThreads) {
        if (thread.joinable()) thread.join();
    }
    workerThreads.clear();

    {
        std::lock_guard<std::mutex> l1(g_surfaceMutex);
        while (!g_surfaceQueue.empty()) g_surfaceQueue.pop();
    }
    {
        std::lock_guard<std::mutex> l2(g_encodeMutex);
        while (!g_encodeQueue.empty()) g_encodeQueue.pop();
    }

    framePool.FrameArrived(token);
    framePool.Close();
    Encoder::FinalizeEncoder();
}