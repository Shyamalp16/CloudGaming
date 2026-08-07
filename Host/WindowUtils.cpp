#include "pch.h"
#include "WindowUtils.h"
#include "WindowHelpers.h"
#include <iostream>
#include <nlohmann/json.hpp>
#include <mutex>
#include <algorithm>
#include <chrono>
#include <thread>
#include <cwctype>

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

static std::wstring Lowercase(std::wstring value)
{
    std::transform(value.begin(), value.end(), value.begin(),
        [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
    return value;
}

static long long WindowScore(const WindowInfo& window, const std::wstring& preferredTitle)
{
    long long score = static_cast<long long>(window.clientWidth) * window.clientHeight;
    if (!window.minimized) score += 2'000'000'000LL;
    if (!window.owned) score += 1'000'000'000LL;
    if (!window.title.empty()) score += 100'000'000LL;
    if (window.hwnd == GetForegroundWindow()) score += 4'000'000'000LL;
    if (!preferredTitle.empty() &&
        Lowercase(window.title).find(Lowercase(preferredTitle)) != std::wstring::npos) {
        score += 8'000'000'000LL;
    }
    return score;
}

bool PickWindowByProcessName(const std::wstring& processName, HWND& outHwnd, DWORD& outProcessId,
                             const std::wstring& preferredTitle, bool logCandidates)
{
    auto matches = FindWindowsByProcessName(processName);
    std::stable_sort(matches.begin(), matches.end(), [&](const WindowInfo& left, const WindowInfo& right) {
        return WindowScore(left, preferredTitle) > WindowScore(right, preferredTitle);
    });
    if (logCandidates) {
        std::wcout << L"[window] Found " << matches.size() << L" viable windows for process '" << processName << L"'." << std::endl;
        for (auto& w : matches) {
            std::wcout << L"[window] HWND=" << w.hwnd << L" title='" << w.title << L"' pid=" << w.processId
                       << L" client=" << w.clientWidth << L"x" << w.clientHeight
                       << L" minimized=" << (w.minimized ? L"yes" : L"no")
                       << L" owned=" << (w.owned ? L"yes" : L"no")
                       << L" score=" << WindowScore(w, preferredTitle) << std::endl;
        }
    }
    if (matches.empty()) return false;
    outHwnd = matches[0].hwnd;
    outProcessId = matches[0].processId;
    SetTargetWindow(outHwnd);
    return outHwnd != nullptr;
}

bool WaitForWindowByProcessName(const std::wstring& processName, HWND& outHwnd,
                                DWORD& outProcessId, int timeoutMs, int pollIntervalMs,
                                const std::wstring& preferredTitle)
{
    timeoutMs = std::max(0, timeoutMs);
    pollIntervalMs = std::clamp(pollIntervalMs, 100, 5000);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    bool announced = false;
    do {
        if (PickWindowByProcessName(processName, outHwnd, outProcessId, preferredTitle, false)) {
            PickWindowByProcessName(processName, outHwnd, outProcessId, preferredTitle, true);
            return true;
        }
        if (!announced) {
            std::wcout << L"[window] Waiting up to " << timeoutMs << L"ms for process window '"
                       << processName << L"'..." << std::endl;
            announced = true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(pollIntervalMs));
    } while (std::chrono::steady_clock::now() < deadline);
    return false;
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
    if (resizeClient && (cW != targetW || cH != targetH)) {
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


