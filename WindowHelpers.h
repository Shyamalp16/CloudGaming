#pragma once

#include <windows.h>
#include <cstdint>
#include <winrt/Windows.Graphics.Capture.h>

HWND fetchForegroundWindow();
uint64_t GetWindowIdFromHWND(HWND hwnd);

// Creates a GraphicsCaptureItem from an HWND
winrt::Windows::Graphics::Capture::GraphicsCaptureItem
CreateCaptureItemForWindow(HWND hwnd);
