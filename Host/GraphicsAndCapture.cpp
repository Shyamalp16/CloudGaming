#include "pch.h"
#include "GraphicsAndCapture.h"
#include "D3DHelpers.h"
#include "CaptureHelpers.h"
#include <iostream>

namespace GraphicsAndCapture {

bool InitializeDevice(D3DContext& out)
{
    bool okD3D = SetupD3D(out.device, out.context, out.featureLevel);
    bool okDXGI = SetupDXGI(out.device, out.dxgiDevice);
    if (!okD3D || !okDXGI) {
        std::wcerr << L"[gfx] Failed to init D3D or DXGI." << std::endl;
        return false;
    }
    out.winrtDevice = createIDirect3DDevice(out.dxgiDevice);
    if (!out.winrtDevice) {
        std::wcerr << L"[gfx] Failed to create IDirect3DDevice." << std::endl;
        return false;
    }
    return true;
}

bool InitializeCapture(CaptureContext& cap, const D3DContext& d3d, winrt::Windows::Graphics::Capture::GraphicsCaptureItem item)
{
    cap.item = item;
    auto size = item.Size();
    cap.framePool = createFreeThreadedFramePool(d3d.winrtDevice, size);
    if (!cap.framePool) {
        std::wcerr << L"[gfx] Could not create free-threaded frame pool." << std::endl;
        return false;
    }
    cap.session = createCaptureSession(item, cap.framePool);
    if (!cap.session) {
        std::wcerr << L"[gfx] Could not create capture session." << std::endl;
        return false;
    }
    cap.frameArrivedToken = FrameArrivedEventRegistration(cap.framePool);
    return true;
}

void Start(CaptureContext& cap)
{
    cap.session.StartCapture();
}

void Stop(CaptureContext& cap)
{
    StopCapture(cap.frameArrivedToken, cap.framePool);
    cap.session.Close();
}

}


