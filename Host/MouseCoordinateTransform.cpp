#include "MouseCoordinateTransform.h"
#include <iostream>
#include <ShellScalingApi.h>
#pragma comment(lib, "shcore.lib")

// Function pointer for GetDpiForMonitor (for compatibility with older Windows versions)
typedef HRESULT(WINAPI* PFN_GetDpiForMonitor)(HMONITOR, MONITOR_DPI_TYPE, UINT*, UINT*);
static PFN_GetDpiForMonitor GetDpiForMonitorPtr = nullptr;

// Initialize function pointer
static void InitializeDPISupport() {
    static bool initialized = false;
    if (!initialized) {
        HMODULE hModule = GetModuleHandle(TEXT("shcore.dll"));
        if (hModule) {
            GetDpiForMonitorPtr = (PFN_GetDpiForMonitor)GetProcAddress(hModule, "GetDpiForMonitor");
        }
        initialized = true;
    }
}

namespace MouseCoordinateTransform {

// Global transformer instance
CoordinateTransformer globalTransformer;

CoordinateTransformer::CoordinateTransformer(const TransformConfig& config)
    : config_(config) {
    // Initialize DPI support on first construction
    InitializeDPISupport();
}

TransformResult CoordinateTransformer::transformClientToAbsolute(
    int clientX, int clientY,
    HWND targetWindow,
    int clientViewWidth,
    int clientViewHeight
) {
    TransformResult result = {0, 0, 0, 0, false, false, ""};

    // Validate inputs
    if (!isValidWindow(targetWindow)) {
        result.errorMessage = "Invalid target window";
        return result;
    }

    if (clientViewWidth <= 0 || clientViewHeight <= 0) {
        result.errorMessage = "Invalid client view dimensions";
        return result;
    }

    // Get target window client rect in screen coordinates
    RECT targetClientRect;
    if (!getWindowClientRect(targetWindow, targetClientRect)) {
        result.errorMessage = "Failed to get target window client rect";
        return result;
    }

    // Transform client coordinates to target window coordinates
    int targetX, targetY;
    if (!clientToTargetWindow(clientX, clientY, clientViewWidth, clientViewHeight,
                             targetClientRect, targetX, targetY, config_)) {
        result.errorMessage = "Failed to transform client to target coordinates";
        return result;
    }

    // Clip to target window if enabled
    bool wasClipped = false;
    if (config_.enableClipping) {
        int originalX = targetX, originalY = targetY;
        clipToTargetWindow(targetX, targetY, targetClientRect);
        wasClipped = (targetX != originalX || targetY != originalY);
    }

    // Convert to virtual desktop absolute coordinates
    auto absoluteResult = targetToVirtualDesktopAbsolute(targetX, targetY, targetClientRect, config_.enableClipping);
    if (!absoluteResult.isValid) {
        result.errorMessage = absoluteResult.errorMessage;
        return result;
    }

    // Update result
    result.absoluteX = absoluteResult.absoluteX;
    result.absoluteY = absoluteResult.absoluteY;
    result.virtualDesktopX = targetX;
    result.virtualDesktopY = targetY;
    result.wasClipped = wasClipped || absoluteResult.wasClipped;
    result.isValid = true;

    return result;
}

void CoordinateTransformer::updateConfig(const TransformConfig& config) {
    config_ = config;
}

RECT CoordinateTransformer::getTargetWindowRect(HWND targetWindow) {
    RECT rect = {0, 0, 0, 0};
    if (isValidWindow(targetWindow)) {
        GetWindowRect(targetWindow, &rect);
    }
    return rect;
}

bool CoordinateTransformer::clientToTargetWindow(
    int clientX, int clientY,
    int clientViewWidth, int clientViewHeight,
    const RECT& targetClientRect,
    int& outTargetX, int& outTargetY,
    const TransformConfig& config
) {
    // Client coordinates are relative to the remote view
    // We need to map them to the target window's client area

    // Calculate scaling factors if capture scaling is enabled
    double scaleX = config.accountForScaling ? config.captureScaleX : 1.0;
    double scaleY = config.accountForScaling ? config.captureScaleY : 1.0;

    // If no scaling info is available, assume 1:1 mapping
    if (scaleX <= 0 || scaleY <= 0) {
        scaleX = scaleY = 1.0;
    }

    // Transform client coordinates to target window coordinates
    // Client (0,0) -> Target window client area top-left
    // Client (clientViewWidth, clientViewHeight) -> Target window client area bottom-right

    int targetClientWidth = targetClientRect.right - targetClientRect.left;
    int targetClientHeight = targetClientRect.bottom - targetClientRect.top;

    // Apply scaling and mapping
    outTargetX = targetClientRect.left +
                static_cast<int>((clientX / static_cast<double>(clientViewWidth)) * targetClientWidth * scaleX);

    outTargetY = targetClientRect.top +
                static_cast<int>((clientY / static_cast<double>(clientViewHeight)) * targetClientHeight * scaleY);

    return true;
}

TransformResult CoordinateTransformer::targetToVirtualDesktopAbsolute(
    int targetX, int targetY,
    const RECT& targetClientRect,
    bool enableClipping
) {
    TransformResult result = {0, 0, targetX, targetY, false, false, ""};

    // Get virtual desktop metrics
    int virtualScreenX = GetSystemMetrics(SM_XVIRTUALSCREEN);
    int virtualScreenY = GetSystemMetrics(SM_YVIRTUALSCREEN);
    int virtualScreenWidth = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    int virtualScreenHeight = GetSystemMetrics(SM_CYVIRTUALSCREEN);

    // Safety fallback
    if (virtualScreenWidth <= 0 || virtualScreenHeight <= 0) {
        virtualScreenX = 0;
        virtualScreenY = 0;
        virtualScreenWidth = GetSystemMetrics(SM_CXSCREEN);
        virtualScreenHeight = GetSystemMetrics(SM_CYSCREEN);
    }

    if (virtualScreenWidth <= 0 || virtualScreenHeight <= 0) {
        result.errorMessage = "Invalid virtual screen dimensions";
        return result;
    }

    // Clip to target window if enabled
    if (enableClipping) {
        int originalX = targetX, originalY = targetY;
        clipToTargetWindow(targetX, targetY, targetClientRect);
        result.wasClipped = (targetX != originalX || targetY != originalY);
    }

    // Convert to absolute coordinates (0-65535 range)
    result.absoluteX = static_cast<LONG>(((static_cast<double>(targetX - virtualScreenX) /
                                        virtualScreenWidth) * 65535.0));
    result.absoluteY = static_cast<LONG>(((static_cast<double>(targetY - virtualScreenY) /
                                        virtualScreenHeight) * 65535.0));

    // Clamp to valid range
    if (result.absoluteX < 0) result.absoluteX = 0;
    if (result.absoluteX > 65535) result.absoluteX = 65535;
    if (result.absoluteY < 0) result.absoluteY = 0;
    if (result.absoluteY > 65535) result.absoluteY = 65535;

    result.isValid = true;
    return result;
}

void CoordinateTransformer::clipToTargetWindow(int& x, int& y, const RECT& targetRect) {
    if (x < targetRect.left) x = targetRect.left;
    if (x >= targetRect.right) x = targetRect.right - 1;
    if (y < targetRect.top) y = targetRect.top;
    if (y >= targetRect.bottom) y = targetRect.bottom - 1;
}

void CoordinateTransformer::getDPIScaling(HWND targetWindow, double& scaleX, double& scaleY) {
    scaleX = getDPIScaleX(targetWindow);
    scaleY = getDPIScaleY(targetWindow);
}

bool CoordinateTransformer::setCursorClip(HWND targetWindow, bool enable) {
    if (!isValidWindow(targetWindow)) {
        return false;
    }

    if (enable) {
        RECT clipRect;
        if (GetWindowRect(targetWindow, &clipRect)) {
            return ClipCursor(&clipRect) != 0;
        }
    } else {
        // Remove cursor clipping
        return ClipCursor(NULL) != 0;
    }

    return false;
}

bool CoordinateTransformer::getWindowClientRect(HWND hwnd, RECT& rect) {
    if (!GetClientRect(hwnd, &rect)) {
        return false;
    }

    // Convert client coordinates to screen coordinates
    POINT pt = {rect.left, rect.top};
    if (!ClientToScreen(hwnd, &pt)) {
        return false;
    }

    rect.left = pt.x;
    rect.top = pt.y;

    pt.x = rect.right;
    pt.y = rect.bottom;
    if (!ClientToScreen(hwnd, &pt)) {
        return false;
    }

    rect.right = pt.x;
    rect.bottom = pt.y;

    return true;
}

bool CoordinateTransformer::isValidWindow(HWND hwnd) {
    return hwnd != NULL && IsWindow(hwnd);
}

double CoordinateTransformer::getDPIScaleX(HWND hwnd) {
    // Get DPI for the monitor containing the window
    HMONITOR hMonitor = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
    if (hMonitor && GetDpiForMonitorPtr) {
        UINT dpiX, dpiY;
        if (GetDpiForMonitorPtr(hMonitor, MDT_EFFECTIVE_DPI, &dpiX, &dpiY) == S_OK) {
            return dpiX / 96.0; // 96 DPI is the default
        }
    }
    return 1.0; // Fallback to no scaling
}

double CoordinateTransformer::getDPIScaleY(HWND hwnd) {
    // Get DPI for the monitor containing the window
    HMONITOR hMonitor = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
    if (hMonitor && GetDpiForMonitorPtr) {
        UINT dpiX, dpiY;
        if (GetDpiForMonitorPtr(hMonitor, MDT_EFFECTIVE_DPI, &dpiX, &dpiY) == S_OK) {
            return dpiY / 96.0; // 96 DPI is the default
        }
    }
    return 1.0; // Fallback to no scaling
}

void updateGlobalConfig(const TransformConfig& config) {
    globalTransformer.updateConfig(config);
}

} // namespace MouseCoordinateTransform
