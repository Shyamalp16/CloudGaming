#include "FrameCaptureThread.h"
#include "CaptureHelpers.h"
#include "Encoder.h"
#include "D3DHelpers.h"
#include "WindowHelpers.h"
#include "ShutdownManager.h"
#include <iostream>
#include <chrono>
#include <thread>
#include <vector>
#include <d3d11.h>
#include <wrl.h>
#include <memory>

using namespace Microsoft::WRL;

std::atomic<bool> stopCapture(false);
std::thread captureThread;

// D3D11 resources
ComPtr<ID3D11Device> d3d11Device;
ComPtr<ID3D11DeviceContext> d3d11DeviceContext;
ComPtr<IDXGIOutputDuplication> deskDupl;
ComPtr<ID3D11Texture2D> acquiredDesktopImage;
DXGI_OUTPUT_DESC outputDesc;

// Frame data
int frameWidth = 0;
int frameHeight = 0;

void InitializeD3D11() {
    d3d11Device = GetD3DDevice().get();
    if (!d3d11Device) {
        std::wcerr << L"Failed to get D3D11 device." << std::endl;
        return;
    }
    d3d11Device->GetImmediateContext(&d3d11DeviceContext);

    ComPtr<IDXGIDevice> dxgiDevice;
    d3d11Device.As(&dxgiDevice);

    ComPtr<IDXGIAdapter> dxgiAdapter;
    dxgiDevice->GetAdapter(&dxgiAdapter);

    ComPtr<IDXGIOutput> dxgiOutput;
    dxgiAdapter->EnumOutputs(0, &dxgiOutput);

    ComPtr<IDXGIOutput1> dxgiOutput1;
    dxgiOutput.As(&dxgiOutput1);

    dxgiOutput1->GetDesc(&outputDesc);
    frameWidth = (outputDesc.DesktopCoordinates.right - outputDesc.DesktopCoordinates.left) & ~1;
    frameHeight = (outputDesc.DesktopCoordinates.bottom - outputDesc.DesktopCoordinates.top) & ~1;

    HRESULT hr = dxgiOutput1->DuplicateOutput(d3d11Device.Get(), &deskDupl);
    if (FAILED(hr)) {
        std::wcerr << L"Failed to duplicate output. HRESULT: " << hr << std::endl;
        return;
    }

    std::wcout << L"D3D11 initialized for capture." << std::endl;
}

void CleanupD3D11() {
    if (deskDupl) {
        deskDupl->ReleaseFrame();
    }
    deskDupl.Reset();
    acquiredDesktopImage.Reset();
    d3d11DeviceContext.Reset();
    d3d11Device.Reset();
    std::wcout << L"D3D11 resources cleaned up." << std::endl;
}

void CaptureAndEncodeLoop() {
    if (!deskDupl) {
        std::wcerr << L"DeskDupl not initialized." << std::endl;
        return;
    }

    Encoder::InitializeEncoder("output.mp4", frameWidth, frameHeight, 60);

    auto frameInterval = std::chrono::microseconds(1000000 / 60);
    auto nextFrameTime = std::chrono::steady_clock::now();

    while (!ShutdownManager::IsShutdown()) {
        auto startTime = std::chrono::steady_clock::now();

        ComPtr<IDXGIResource> desktopResource;
        DXGI_OUTDUPL_FRAME_INFO frameInfo;

        HRESULT hr = deskDupl->AcquireNextFrame(500, &frameInfo, &desktopResource);

        if (SUCCEEDED(hr) && desktopResource) {
            desktopResource.As(&acquiredDesktopImage);

            Encoder::EncodeFrame(acquiredDesktopImage.Get(), d3d11DeviceContext.Get(), frameWidth, frameHeight);

            deskDupl->ReleaseFrame();
        }
        else if (hr == DXGI_ERROR_WAIT_TIMEOUT) {
            //std::wcout << L"AcquireNextFrame timeout." << std::endl;
            continue;
        }
        else {
            std::wcerr << L"Failed to acquire next frame. HRESULT: " << std::hex << hr << std::endl;
            // Consider re-initializing duplication here
            break;
        }

        auto endTime = std::chrono::steady_clock::now();
        auto elapsedTime = std::chrono::duration_cast<std::chrono::microseconds>(endTime - startTime);

        nextFrameTime += frameInterval;
        auto sleepTime = nextFrameTime - endTime;
        if (sleepTime > std::chrono::microseconds(0)) {
            std::this_thread::sleep_for(sleepTime);
        }
    }

    Encoder::FlushEncoder();
    Encoder::FinalizeEncoder();
}

void StartDesktopCapture() {
    stopCapture = false;
    InitializeD3D11();
    captureThread = std::thread(CaptureAndEncodeLoop);
}

void StopDesktopCapture() {
    stopCapture = true;
    if (captureThread.joinable()) {
        captureThread.join();
    }
    CleanupD3D11();
}
