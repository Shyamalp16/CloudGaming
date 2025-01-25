#include "WindowHelpers.h"
#include <iostream>
#include <stdexcept>
#include <winrt/Windows.Foundation.h>
#include <windows.graphics.capture.interop.h> // IGraphicsCaptureItemInterop

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

std::wstring GetProcessNameFromHWND(HWND hwnd) {
    if (!::IsWindow(hwnd)) {
		return L""; //invalid window
    }

    //Get process ID
	DWORD processId = 0;
	::GetWindowThreadProcessId(hwnd, &processId);
    if (!processId) {
		return L""; //invalid process ID
    }

    //Open process with "Query rights"
    HANDLE hProcess = ::OpenProcess(
        PROCESS_QUERY_INFORMATION | PROCESS_VM_READ,
        FALSE,
        processId
    );

    if (!hProcess) {
		return L""; //invalid process handle
    }

	WCHAR pathBuffer[MAX_PATH] = { 0 };
	if (::GetModuleFileNameExW(hProcess, nullptr, pathBuffer, MAX_PATH) == 0) {
		::CloseHandle(hProcess);
		return L""; //failed to get module name
	}
	::CloseHandle(hProcess);

    //path buffer can be for ex C:\programfiles\steamapps\CS2.exe etc.
	std::wstring fullPath(pathBuffer);

	size_t lastSlash = fullPath.find_last_of(L"\\");
	if (lastSlash == std::wstring::npos) {
		return L""; //no slash found
	}
	//return only the process name
	std::wstring processName = fullPath.substr(lastSlash + 1);
	return processName; //file name or empty
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

