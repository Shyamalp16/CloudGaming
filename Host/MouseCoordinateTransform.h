#pragma once

#include <windows.h>
#include <string>

namespace MouseCoordinateTransform {

struct TransformResult {
    LONG absoluteX = 0;
    LONG absoluteY = 0;
    int virtualDesktopX = 0;
    int virtualDesktopY = 0;
    bool wasClipped = false;
    bool isValid = false;
    std::string errorMessage;
};

TransformResult transformClientToAbsolute(
    int clientX,
    int clientY,
    HWND targetWindow,
    int clientViewWidth,
    int clientViewHeight);

} // namespace MouseCoordinateTransform
