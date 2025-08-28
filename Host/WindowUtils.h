#pragma once

#include <string>
#include <windows.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <nlohmann/json.hpp>

namespace WindowUtils {
    // Find first window by process name; logs matches; returns HWND and processId
    bool PickWindowByProcessName(const std::wstring& processName, HWND& outHwnd, DWORD& outProcessId);

    // Optionally resize client area from config
    void MaybeResizeClientArea(HWND hwnd, const nlohmann::json& config);

    // Create capture item for window
    winrt::Windows::Graphics::Capture::GraphicsCaptureItem CreateItem(HWND hwnd);

    // Target window management for input focus
    void SetTargetWindow(HWND hwnd);
    HWND GetTargetWindow();
}


