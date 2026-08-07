#pragma once

#include <string>
#include <windows.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <nlohmann/json.hpp>

namespace WindowUtils {
    // Ranks viable top-level windows by title preference, foreground state,
    // ownership, minimization, and client area.
    bool PickWindowByProcessName(const std::wstring& processName, HWND& outHwnd,
                                 DWORD& outProcessId,
                                 const std::wstring& preferredTitle = L"",
                                 bool logCandidates = true);

    // Waits for games that create their main window after the host starts.
    bool WaitForWindowByProcessName(const std::wstring& processName, HWND& outHwnd,
                                    DWORD& outProcessId, int timeoutMs, int pollIntervalMs,
                                    const std::wstring& preferredTitle = L"");

    // Optionally resize client area from config
    void MaybeResizeClientArea(HWND hwnd, const nlohmann::json& config);

    // Create capture item for window
    winrt::Windows::Graphics::Capture::GraphicsCaptureItem CreateItem(HWND hwnd);

    // Target window management for input focus
    void SetTargetWindow(HWND hwnd);
    HWND GetTargetWindow();
}


