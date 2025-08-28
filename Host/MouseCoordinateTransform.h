#pragma once

/*
 * Mouse Coordinate Transformation System
 *
 * This system provides DPI-aware, accurate mouse coordinate transformation
 * for remote desktop streaming applications. It solves the problem of
 * mapping client-side mouse coordinates to the correct host window location.
 *
 * Key Features:
 * - DPI-aware coordinate transformation using GetDpiForMonitor
 * - Target window client area mapping with scaling support
 * - Optional cursor clipping to prevent input outside streamed area
 * - Virtual desktop coordinate space handling
 * - Comprehensive error handling and metrics
 *
 * Usage:
 * 1. Configure client view dimensions and scaling:
 *    MouseInputHandler::updateCoordinateTransformConfig(width, height, config);
 *
 * 2. Transform coordinates:
 *    auto result = MouseCoordinateTransform::globalTransformer.transformClientToAbsolute(
 *        clientX, clientY, targetWindow, clientViewWidth, clientViewHeight);
 *
 * 3. Check result and use absolute coordinates:
 *    if (result.isValid) {
 *        input.mi.dx = result.absoluteX;
 *        input.mi.dy = result.absoluteY;
 *    }
 */

#include <windows.h>
#include <string>
#include "Metrics.h"

namespace MouseCoordinateTransform {

// Configuration for coordinate transformation
struct TransformConfig {
    bool enableClipping = false;      // Whether to clip cursor to target window
    bool enableClipCursor = false;    // Whether to use ClipCursor API during streaming
    bool accountForScaling = true;    // Whether to account for capture scaling
    double captureScaleX = 1.0;       // Horizontal scaling factor (client view / host window)
    double captureScaleY = 1.0;       // Vertical scaling factor (client view / host window)
};

// Transformation result
struct TransformResult {
    LONG absoluteX;           // Final absolute coordinate X (0-65535)
    LONG absoluteY;           // Final absolute coordinate Y (0-65535)
    int virtualDesktopX;      // Virtual desktop coordinate X
    int virtualDesktopY;      // Virtual desktop coordinate Y
    bool wasClipped;          // Whether coordinates were clipped to target
    bool isValid;             // Whether transformation was successful
    std::string errorMessage; // Error description if !isValid
};

// DPI-aware coordinate transformation utility
class CoordinateTransformer {
private:
    TransformConfig config_;

public:
    CoordinateTransformer(const TransformConfig& config = TransformConfig());

    // Main transformation function
    TransformResult transformClientToAbsolute(
        int clientX, int clientY,    // Client coordinates (from remote view)
        HWND targetWindow,           // Target window handle
        int clientViewWidth,         // Width of client's view
        int clientViewHeight         // Height of client's view
    );

    // Update configuration
    void updateConfig(const TransformConfig& config);

    // Get target window client rect in virtual desktop coordinates
    static RECT getTargetWindowRect(HWND targetWindow);

    // Convert client coordinates to target window client coordinates
    static bool clientToTargetWindow(
        int clientX, int clientY,
        int clientViewWidth, int clientViewHeight,
        const RECT& targetClientRect,
        int& outTargetX, int& outTargetY,
        const TransformConfig& config
    );

    // Convert target window coordinates to virtual desktop absolute coordinates
    static TransformResult targetToVirtualDesktopAbsolute(
        int targetX, int targetY,
        const RECT& targetClientRect,
        bool enableClipping
    );

    // Clip coordinates to target window bounds
    static void clipToTargetWindow(int& x, int& y, const RECT& targetRect);

    // Get DPI scaling factors
    static void getDPIScaling(HWND targetWindow, double& scaleX, double& scaleY);

    // Enable/disable cursor clipping for target window
    static bool setCursorClip(HWND targetWindow, bool enable);

private:
    // Helper functions
    static bool getWindowClientRect(HWND hwnd, RECT& rect);
    static bool isValidWindow(HWND hwnd);
    static double getDPIScaleX(HWND hwnd);
    static double getDPIScaleY(HWND hwnd);
};

// Global transformer instance
extern CoordinateTransformer globalTransformer;

// Configuration update function
void updateGlobalConfig(const TransformConfig& config);

} // namespace MouseCoordinateTransform
