#include "pch.h"
#include "AppInit.h"
#include "Encoder.h"
#include "AudioCapturer.h"
#include "pion_webrtc.h"
#include "WebRTCWrapper.h"
#include "RuntimeMetrics.h"
#include <iostream>

// PLI callback implemented in Encoder.cpp
extern "C" void OnPLI();

namespace {
    void onRTCP(double packetLoss, double rtt, double jitter) {
        RuntimeMetrics::UpdateBasic(packetLoss, rtt, jitter);
        // Video bitrate adaptation (existing)
        Encoder::OnRtcpFeedback(packetLoss, rtt, jitter);

        // Audio bitrate adaptation (new)
        AudioCapturer::OnRtcpFeedback(packetLoss, rtt, jitter);
    }
    void onEnhancedStats(double packetLoss, double rtt, double jitter,
                         uint32_t nackCount, uint32_t pliCount, uint32_t /*twccCount*/,
                         uint32_t pacerQueueLength, uint32_t sendBitrateKbps) {
        RuntimeMetrics::UpdateEnhanced(packetLoss, rtt, jitter, nackCount, pliCount,
                                      pacerQueueLength, sendBitrateKbps);
    }
}

namespace AppInit {

void InitializeProcess()
{
    if (!SetPriorityClass(GetCurrentProcess(), NORMAL_PRIORITY_CLASS)) {
        std::wcerr << L"[AppInit] Warning: Failed to set NORMAL_PRIORITY_CLASS" << std::endl;
    } else {
        std::wcout << L"[AppInit] Process priority set to NORMAL (avoids starving game)" << std::endl;
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
    WebRTCWrapper::setWebRTCStatsCallback(onEnhancedStats);
    SetPLICallback(OnPLI);
}

}


