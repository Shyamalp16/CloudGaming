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

winrt::Windows::Graphics::Capture::GraphicsCaptureItem
CreateCaptureItemForWindow(HWND hwnd);
winrt::Windows::Graphics::Capture::GraphicsCaptureItem
CreateCaptureItemForMonitor(HMONITOR hmon);

bool GetClientAreaSize(HWND hwnd, int& outWidth, int& outHeight);
bool SetWindowClientAreaSize(HWND hwnd, int targetWidth, int targetHeight);

std::wstring GetProcessNameFromHWND(HWND hwnd);
std::wstring GetWindowTitle(HWND hwnd);

struct WindowInfo {
	HWND hwnd;
	std::wstring title;
	std::wstring processName;
	DWORD processId;
};

std::vector<WindowInfo> EnumerateAllWindows();
std::vector<WindowInfo> FindWindowsByProcessName(const std::wstring& processName);
std::vector<WindowInfo> FindWindowByTitle(const std::wstring& title);