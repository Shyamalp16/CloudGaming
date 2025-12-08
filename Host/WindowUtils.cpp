#include "pch.h"
#include "WindowUtils.h"
#include "WindowHelpers.h"
#include <iostream>
#include <nlohmann/json.hpp>
#include <mutex>

namespace WindowUtils {

static HWND g_targetWindow = nullptr;
static std::mutex g_targetWindowMutex;

void SetTargetWindow(HWND hwnd) {
    std::lock_guard<std::mutex> lock(g_targetWindowMutex);
    g_targetWindow = hwnd;
}

HWND GetTargetWindow() {
    std::lock_guard<std::mutex> lock(g_targetWindowMutex);
    return g_targetWindow;
}

bool PickWindowByProcessName(const std::wstring& processName, HWND& outHwnd, DWORD& outProcessId)
{
    auto matches = FindWindowsByProcessName(processName);
    std::wcout << L"[window] Found " << matches.size() << L" windows for process '" << processName << L"'." << std::endl;
    for (auto& w : matches) {
        std::wcout << L"[window] HWND=" << w.hwnd << L" title='" << w.title << L"' pid=" << w.processId << std::endl;
    }
    if (matches.empty()) return false;
    outHwnd = matches[0].hwnd;
    outProcessId = matches[0].processId;
    SetTargetWindow(outHwnd);
    return outHwnd != nullptr;
}

void MaybeResizeClientArea(HWND hwnd, const nlohmann::json& config)
{
    int cW = 0, cH = 0;
    if (GetClientAreaSize(hwnd, cW, cH)) {
        std::wcout << L"[window] Initial client area: " << cW << L"x" << cH << std::endl;
    }
    int targetW = 1920;
    int targetH = 1080;
    bool resizeClient = true;
    if (config.contains("host") && config["host"].contains("window")) {
        const auto& wcfg = config["host"]["window"];
        if (wcfg.contains("targetWidth")) targetW = wcfg["targetWidth"].get<int>();
        if (wcfg.contains("targetHeight")) targetH = wcfg["targetHeight"].get<int>();
        if (wcfg.contains("resizeClientArea")) resizeClient = wcfg["resizeClientArea"].get<bool>();
    }
    if (resizeClient && (cW < targetW || cH < targetH)) {
        if (SetWindowClientAreaSize(hwnd, targetW, targetH)) {
            std::wcout << L"[window] Resized window client area to " << targetW << L"x" << targetH << std::endl;
            if (GetClientAreaSize(hwnd, cW, cH)) {
                std::wcout << L"[window] New client area: " << cW << L"x" << cH << std::endl;
            }
        } else {
            std::wcout << L"[window] Failed to resize window client area." << std::endl;
        }
    }
}

winrt::Windows::Graphics::Capture::GraphicsCaptureItem CreateItem(HWND hwnd)
{
    return CreateCaptureItemForWindow(hwnd);
}

}


