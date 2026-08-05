#include "MouseCoordinateTransform.h"

#include <algorithm>

namespace {

bool getWindowClientRect(HWND window, RECT& rect) {
    if (!window || !IsWindow(window) || !GetClientRect(window, &rect)) {
        return false;
    }

    POINT topLeft{rect.left, rect.top};
    POINT bottomRight{rect.right, rect.bottom};
    if (!ClientToScreen(window, &topLeft) || !ClientToScreen(window, &bottomRight)) {
        return false;
    }

    rect = {topLeft.x, topLeft.y, bottomRight.x, bottomRight.y};
    return rect.right > rect.left && rect.bottom > rect.top;
}

} // namespace

namespace MouseCoordinateTransform {

TransformResult transformClientToAbsolute(
    int clientX,
    int clientY,
    HWND targetWindow,
    int clientViewWidth,
    int clientViewHeight) {
    TransformResult result;
    if (clientViewWidth <= 0 || clientViewHeight <= 0) {
        result.errorMessage = "Invalid client view dimensions";
        return result;
    }

    RECT targetRect{};
    if (!getWindowClientRect(targetWindow, targetRect)) {
        result.errorMessage = "Invalid target window client area";
        return result;
    }

    const int targetWidth = targetRect.right - targetRect.left;
    const int targetHeight = targetRect.bottom - targetRect.top;
    const int unclippedX = targetRect.left + static_cast<int>(
        static_cast<double>(clientX) * targetWidth / clientViewWidth);
    const int unclippedY = targetRect.top + static_cast<int>(
        static_cast<double>(clientY) * targetHeight / clientViewHeight);
    result.virtualDesktopX = (std::clamp)(unclippedX, static_cast<int>(targetRect.left), static_cast<int>(targetRect.right - 1));
    result.virtualDesktopY = (std::clamp)(unclippedY, static_cast<int>(targetRect.top), static_cast<int>(targetRect.bottom - 1));
    result.wasClipped = result.virtualDesktopX != unclippedX || result.virtualDesktopY != unclippedY;

    int virtualX = GetSystemMetrics(SM_XVIRTUALSCREEN);
    int virtualY = GetSystemMetrics(SM_YVIRTUALSCREEN);
    int virtualWidth = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    int virtualHeight = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    if (virtualWidth <= 0 || virtualHeight <= 0) {
        virtualX = 0;
        virtualY = 0;
        virtualWidth = GetSystemMetrics(SM_CXSCREEN);
        virtualHeight = GetSystemMetrics(SM_CYSCREEN);
    }
    if (virtualWidth <= 0 || virtualHeight <= 0) {
        result.errorMessage = "Invalid virtual desktop dimensions";
        return result;
    }

    result.absoluteX = static_cast<LONG>((std::clamp)(
        (static_cast<double>(result.virtualDesktopX - virtualX) / virtualWidth) * 65535.0,
        0.0,
        65535.0));
    result.absoluteY = static_cast<LONG>((std::clamp)(
        (static_cast<double>(result.virtualDesktopY - virtualY) / virtualHeight) * 65535.0,
        0.0,
        65535.0));
    result.isValid = true;
    return result;
}

} // namespace MouseCoordinateTransform
