#include "pch.h"
#include "AppInit.h"
#include "Encoder.h"
#include "pion_webrtc.h"

// PLI callback implemented in Encoder.cpp
extern "C" void OnPLI();

namespace {
    void onRTCP(double packetLoss, double rtt, double jitter) {
        Encoder::OnRtcpFeedback(packetLoss, rtt, jitter);
    }
}

namespace AppInit {

void InitializeProcess()
{
    if (!SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS)) {
        std::wcerr << L"[AppInit] Warning: Failed to set HIGH_PRIORITY_CLASS" << std::endl;
    } else {
        std::wcout << L"[AppInit] Process priority set to HIGH" << std::endl;
    }

    if (!SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2)) {
        SetProcessDPIAware();
    }
    winrt::init_apartment(winrt::apartment_type::multi_threaded);
    std::wcout << L"[AppInit] Apartment initialized." << std::endl;
}

void InitializeRtcBindings()
{
    initGo();
    SetRTCPCallback(onRTCP);
    SetPLICallback(OnPLI);
}

}


