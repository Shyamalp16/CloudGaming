#pragma once

#include <windows.h>
#include <winrt/Windows.Foundation.h>

namespace AppInit {
    // Initialize process priority, DPI awareness, and WinRT apartment
    void InitializeProcess();

    // Initialize Go/WebRTC bindings and register callbacks
    void InitializeRtcBindings();
}


