#pragma once

#include <Windows.h>

#include <string>
#include <vector>

namespace ProcessDiscovery {
struct Target {
    HWND window = nullptr;
    DWORD processId = 0;
    std::wstring processName;
    std::wstring title;
    int clientWidth = 0;
    int clientHeight = 0;
    bool minimized = false;
};

std::vector<Target> EnumerateTargets();
}
