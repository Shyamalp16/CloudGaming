#pragma once
//#define WIN32_LEAN_AND_MEAN
//#define NOMINMAX
#include <windows.h>
#include <cstdint>
#include <winrt/Windows.Graphics.Capture.h>
#include <tchar.h>
#include <psapi.h>
#include <string>
#include <vector>

#pragma comment(lib, "psapi.lib")

HWND fetchForegroundWindow();
uint64_t GetWindowIdFromHWND(HWND hwnd);

// Creates a GraphicsCaptureItem from an HWND
winrt::Windows::Graphics::Capture::GraphicsCaptureItem
CreateCaptureItemForWindow(HWND hwnd);
// Creates a GraphicsCaptureItem from a monitor handle
winrt::Windows::Graphics::Capture::GraphicsCaptureItem
CreateCaptureItemForMonitor(HMONITOR hmon);

std::wstring GetProcessNameFromHWND(HWND hwnd);
std::wstring GetWindowTitle(HWND hwnd);

struct WindowInfo {
	HWND hwnd;
	std::wstring title;
	std::wstring processName;
	DWORD processId;
};

//following function enumerates all windows and returns vector
std::vector<WindowInfo> EnumerateAllWindows();
std::vector<WindowInfo> FindWindowsByProcessName(const std::wstring& processName);
std::vector<WindowInfo> FindWindowByTitle(const std::wstring& title);