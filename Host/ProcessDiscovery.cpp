#include "ProcessDiscovery.h"

#include <algorithm>
#include <cwctype>
#include <unordered_set>

#include "WindowHelpers.h"

namespace ProcessDiscovery {
namespace {
std::wstring Lower(std::wstring value) {
    std::transform(value.begin(), value.end(), value.begin(),
        [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
    return value;
}
}

std::vector<Target> EnumerateTargets() {
    std::vector<Target> targets;
    std::unordered_set<std::wstring> seen;
    for (const auto& window : EnumerateAllWindows()) {
        if (!window.hwnd || window.processId == GetCurrentProcessId() || window.processName.empty()) continue;
        const auto key = Lower(window.processName) + L"\n" + Lower(window.title);
        if (!seen.insert(key).second) continue;
        targets.push_back({window.hwnd, window.processId, window.processName, window.title,
                           window.clientWidth, window.clientHeight, window.minimized});
    }
    std::stable_sort(targets.begin(), targets.end(), [](const Target& left, const Target& right) {
        if (left.minimized != right.minimized) return !left.minimized;
        const auto leftArea = static_cast<long long>(left.clientWidth) * left.clientHeight;
        const auto rightArea = static_cast<long long>(right.clientWidth) * right.clientHeight;
        if (leftArea != rightArea) return leftArea > rightArea;
        return left.processName < right.processName;
    });
    return targets;
}
}
