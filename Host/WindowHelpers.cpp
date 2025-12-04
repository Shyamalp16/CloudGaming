#include "WindowHelpers.h"
#include <iostream>
#include <stdexcept>
#include <winrt/Windows.Foundation.h>
#include <windows.graphics.capture.interop.h>
#include <winrt/Windows.Graphics.Display.h>
#include <ShellScalingAPI.h>
#pragma comment(lib, "Shcore.lib")

HWND fetchForegroundWindow()
{
    try
    {
        HWND foreground = ::GetForegroundWindow();
        if (!foreground)
        {
            std::wcerr << L"[fetchForegroundWindow] Returned NULL.\n";
        }
        return foreground;
    }
    catch (...)
    {
        std::wcerr << L"[fetchForegroundWindow] Exception.\n";
        return nullptr;
    }
}

uint64_t GetWindowIdFromHWND(HWND hwnd)
{
    if (!hwnd)
    {
        std::wcerr << L"[GetWindowIdFromHWND] Invalid.\n";
        return 0;
    }
    return reinterpret_cast<uint64_t>(hwnd);
}

winrt::Windows::Graphics::Capture::GraphicsCaptureItem
CreateCaptureItemForWindow(HWND hwnd)
{
    using namespace winrt::Windows::Graphics::Capture;

    if (!hwnd || !::IsWindow(hwnd))
    {
        throw std::invalid_argument("Invalid HWND passed to CreateCaptureItemForWindow.");
    }

    auto interopFactory = winrt::get_activation_factory<
        GraphicsCaptureItem,
        IGraphicsCaptureItemInterop>();

    GraphicsCaptureItem item{ nullptr };

    HRESULT hr = interopFactory->CreateForWindow(
        hwnd,
        winrt::guid_of<GraphicsCaptureItem>(),
        winrt::put_abi(item)
    );

    if (FAILED(hr))
    {
        std::wcerr << L"[CreateCaptureItemForWindow] Failed. HRESULT=0x"
            << std::hex << hr << std::endl;
        return nullptr;
    }

    return item;
}

winrt::Windows::Graphics::Capture::GraphicsCaptureItem
CreateCaptureItemForMonitor(HMONITOR hmon)
{
    using namespace winrt::Windows::Graphics::Capture;
    if (!hmon) return nullptr;
    auto interopFactory = winrt::get_activation_factory<
        GraphicsCaptureItem,
        IGraphicsCaptureItemInterop>();

    GraphicsCaptureItem item{ nullptr };
    HRESULT hr = interopFactory->CreateForMonitor(
        hmon,
        winrt::guid_of<GraphicsCaptureItem>(),
        winrt::put_abi(item));
    if (FAILED(hr)) {
        std::wcerr << L"[CreateCaptureItemForMonitor] Failed. HRESULT=0x" << std::hex << hr << std::endl;
        return nullptr;
    }
    return item;
}

bool GetClientAreaSize(HWND hwnd, int& outWidth, int& outHeight)
{
    outWidth = outHeight = 0;
    if (!hwnd || !::IsWindow(hwnd)) return false;
    RECT rc{};
    if (!::GetClientRect(hwnd, &rc)) return false;
    outWidth = rc.right - rc.left;
    outHeight = rc.bottom - rc.top;
    return true;
}

static BOOL GetWindowDpi(HWND hwnd, UINT& dpiOut)
{
    dpiOut = 96;
    // Try Per-Monitor V2
    if (auto pGetDpiForWindow = (UINT (WINAPI*)(HWND))GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetDpiForWindow")) {
        dpiOut = pGetDpiForWindow(hwnd);
        return TRUE;
    }
    // Fallback to system DPI
    UINT dpiX = 96, dpiY = 96;
    if (SUCCEEDED(GetDpiForMonitor(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), MDT_EFFECTIVE_DPI, &dpiX, &dpiY))) {
        dpiOut = dpiX;
        return TRUE;
    }
    return FALSE;
}

bool SetWindowClientAreaSize(HWND hwnd, int targetWidth, int targetHeight)
{
    if (!hwnd || !::IsWindow(hwnd)) return false;
    // Compute required outer size for desired client size
    DWORD style = (DWORD)::GetWindowLongPtr(hwnd, GWL_STYLE);
    DWORD exStyle = (DWORD)::GetWindowLongPtr(hwnd, GWL_EXSTYLE);
    RECT rc{ 0, 0, targetWidth, targetHeight };
    UINT dpi = 96;
    GetWindowDpi(hwnd, dpi);
    auto pAdjustForDpi = (BOOL (WINAPI*)(LPRECT,DWORD,BOOL,DWORD,UINT))GetProcAddress(GetModuleHandleW(L"user32.dll"), "AdjustWindowRectExForDpi");
    BOOL ok = FALSE;
    if (pAdjustForDpi) {
        ok = pAdjustForDpi(&rc, style, FALSE, exStyle, dpi);
    } else {
        ok = ::AdjustWindowRectEx(&rc, style, FALSE, exStyle);
    }
    if (!ok) return false;
    int outerW = rc.right - rc.left;
    int outerH = rc.bottom - rc.top;
    // Position unchanged; resize only
    return ::SetWindowPos(hwnd, nullptr, 0, 0, outerW, outerH, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
}

std::wstring GetProcessNameFromHWND(HWND hwnd) {
	if (!::IsWindow(hwnd)) {
		return L""; // invalid window
    }

    // Get process ID
	DWORD processId = 0;
	::GetWindowThreadProcessId(hwnd, &processId);
    if (!processId) {
		return L""; // invalid process ID
    }

    // Try to open the process with minimal rights first. Many games (or
    // protected processes) may deny PROCESS_VM_READ, but allow LIMITED_INFORMATION,
    // which is enough for QueryFullProcessImageNameW.
    HANDLE hProcess = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, processId);
    if (!hProcess) {
        // Fallback to older flag set for compatibility on systems where
        // PROCESS_QUERY_LIMITED_INFORMATION is not sufficient / available.
        hProcess = ::OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, processId);
    }

    if (!hProcess) {
		return L""; // invalid process handle or access denied
    }

	WCHAR pathBuffer[MAX_PATH] = { 0 };
    DWORD size = static_cast<DWORD>(std::size(pathBuffer));

    // Prefer QueryFullProcessImageNameW (works with LIMITED_INFORMATION)
    BOOL ok = ::QueryFullProcessImageNameW(hProcess, 0, pathBuffer, &size);
    if (!ok || size == 0) {
        // Fallback to legacy GetModuleFileNameExW if available
        if (::GetModuleFileNameExW(hProcess, nullptr, pathBuffer, static_cast<DWORD>(std::size(pathBuffer))) == 0) {
            ::CloseHandle(hProcess);
            return L""; // failed to get module name
        }
    }
	::CloseHandle(hProcess);

    // path buffer can be for ex C:\programfiles\steamapps\CS2.exe etc.
	std::wstring fullPath(pathBuffer);

	size_t lastSlash = fullPath.find_last_of(L"\\");
	if (lastSlash == std::wstring::npos) {
		return L""; // no slash found
	}
	// return only the process name
	std::wstring processName = fullPath.substr(lastSlash + 1);
	return processName; // file name or empty
}

std::wstring GetWindowTitle(HWND hwnd) {
    if (!::IsWindow(hwnd)) {
        return L""; //invalid window
    }
	const int length = ::GetWindowTextLength(hwnd);
	if (length == 0) {
		return L""; //no title
	}
	std::wstring titleBuffer(length, L'\0');
	::GetWindowText(hwnd, &titleBuffer[0], length + 1);
    return titleBuffer;
}

struct EnumData {
	std::vector<WindowInfo> results;
	/*std::wstring targetProcessName;
    std::wstring titleContains;*/
};

static BOOL CALLBACK EnumWindowProc(HWND hwnd, LPARAM lParam) {
    auto data = reinterpret_cast<EnumData*>(lParam);
    if (!data || !::IsWindow(hwnd)) {
        return TRUE;
    }

	if (!::IsWindowVisible(hwnd)) {
		return TRUE; //Skip invisible windows
	}

    WindowInfo info;
    info.hwnd = hwnd;
    info.title = GetWindowTitle(hwnd);
    info.processName = GetProcessNameFromHWND(hwnd);
    DWORD processId = 0;
    GetWindowThreadProcessId(hwnd, &processId);
    info.processId = processId;
	data->results.push_back(info);
    return TRUE;
}

std::vector<WindowInfo> EnumerateAllWindows() {
	EnumData data;
	::EnumWindows(EnumWindowProc, reinterpret_cast<LPARAM>(&data));
	return data.results;
}

std::vector<WindowInfo> FindWindowsByProcessName(const std::wstring& processName) {
	auto allWindows = EnumerateAllWindows();
	std::vector<WindowInfo> matched;
    
    for (auto& w : allWindows) {
        if (!_wcsicmp(w.processName.c_str(), processName.c_str())) {
			matched.push_back(w);
        }
    }
    return matched;
}

std::vector<WindowInfo> FindWindowByTitle(const std::wstring& title) {
	auto allWindows = EnumerateAllWindows();
    std::vector<WindowInfo> matched;

    for (auto& w : allWindows) {
        if (!_wcsicmp(w.title.c_str(), title.c_str())) {
            matched.push_back(w);
        }
    }
    return matched;
}

