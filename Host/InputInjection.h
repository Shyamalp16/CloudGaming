#pragma once

#include <windows.h>
#include <string>
#include "Metrics.h"
#include "WindowUtils.h"

namespace InputInjection {

// Injection precondition policy configuration
struct InjectionPolicy {
    bool requireForeground = true;  // Whether target window must be foreground
    bool requireVisible = true;     // Whether target window must be visible
    bool requireEnabled = true;     // Whether target window must be enabled
    bool autoFocusOnSkip = false;   // Whether to attempt SetForegroundWindow on skip
};

// Default policy - strict confinement to foreground window only
inline const InjectionPolicy& getDefaultPolicy() {
    static const InjectionPolicy policy = {
        true,   // requireForeground
        true,   // requireVisible
        true,   // requireEnabled
        false   // autoFocusOnSkip
    };
    return policy;
}

// Enumeration of skip reasons for observability
enum class SkipReason {
    WindowNotFound,
    WindowNotVisible,
    WindowNotEnabled,
    WindowNotForeground,
    PolicyViolation
};

// Convert skip reason to string for logging
inline std::string skipReasonToString(SkipReason reason) {
    switch (reason) {
        case SkipReason::WindowNotFound: return "target_window_not_found";
        case SkipReason::WindowNotVisible: return "target_window_not_visible";
        case SkipReason::WindowNotEnabled: return "target_window_not_enabled";
        case SkipReason::WindowNotForeground: return "target_window_not_foreground";
        case SkipReason::PolicyViolation: return "policy_violation";
        default: return "unknown";
    }
}

// Check if a window handle is valid and meets basic criteria
inline bool isWindowValid(HWND hwnd) {
    return hwnd != nullptr && IsWindow(hwnd);
}

// Check if window is visible (not minimized and visible flag set)
inline bool isWindowVisible(HWND hwnd) {
    return IsWindowVisible(hwnd) && !IsIconic(hwnd);
}

// Check if window is enabled for input
inline bool isWindowEnabled(HWND hwnd) {
    return IsWindowEnabled(hwnd);
}

// Check if window is the foreground window
inline bool isWindowForeground(HWND hwnd) {
    return GetForegroundWindow() == hwnd;
}

// Comprehensive window state check against policy
inline bool checkWindowState(HWND targetHwnd, const InjectionPolicy& policy, SkipReason* outReason = nullptr) {
    if (!isWindowValid(targetHwnd)) {
        if (outReason) *outReason = SkipReason::WindowNotFound;
        return false;
    }

    if (policy.requireVisible && !isWindowVisible(targetHwnd)) {
        if (outReason) *outReason = SkipReason::WindowNotVisible;
        return false;
    }

    if (policy.requireEnabled && !isWindowEnabled(targetHwnd)) {
        if (outReason) *outReason = SkipReason::WindowNotEnabled;
        return false;
    }

    if (policy.requireForeground && !isWindowForeground(targetHwnd)) {
        if (outReason) *outReason = SkipReason::WindowNotForeground;
        return false;
    }

    return true;
}

// Attempt to bring window to foreground (fallback behavior)
inline bool attemptFocusSteal(HWND targetHwnd) {
    if (!isWindowValid(targetHwnd)) {
        return false;
    }

    // Try multiple strategies to bring window to foreground
    bool success = false;

    // Strategy 1: SetForegroundWindow
    success = SetForegroundWindow(targetHwnd);

    if (!success) {
        // Strategy 2: SetFocus after ensuring thread attachment
        DWORD targetThreadId = GetWindowThreadProcessId(targetHwnd, nullptr);
        DWORD currentThreadId = GetCurrentThreadId();

        if (targetThreadId != currentThreadId) {
            AttachThreadInput(currentThreadId, targetThreadId, TRUE);
            success = (SetFocus(targetHwnd) != nullptr);
            AttachThreadInput(currentThreadId, targetThreadId, FALSE);
        } else {
            success = (SetFocus(targetHwnd) != nullptr);
        }
    }

    return success;
}

// Main injection precondition check with fallback behavior
// Returns true if injection should proceed, false if it should be skipped
inline bool shouldInjectInput(const InjectionPolicy& policy = getDefaultPolicy(), const std::string& eventType = "unknown") {
    InputMetrics::inc(InputMetrics::injectionAttempts());

    HWND targetHwnd = WindowUtils::GetTargetWindow();
    SkipReason skipReason;

    if (!checkWindowState(targetHwnd, policy, &skipReason)) {
        // Count the skip reason
        switch (skipReason) {
            case SkipReason::WindowNotForeground:
                InputMetrics::inc(InputMetrics::skippedDueToForeground());
                break;
            case SkipReason::WindowNotVisible:
            case SkipReason::WindowNotEnabled:
                InputMetrics::inc(InputMetrics::skippedDueToWindowState());
                break;
            default:
                // Other reasons don't have specific counters yet
                break;
        }

        // Optional: attempt to bring window to foreground
        if (policy.autoFocusOnSkip) {
            if (attemptFocusSteal(targetHwnd)) {
                // Retry the check after focus attempt
                if (checkWindowState(targetHwnd, policy)) {
                    return true; // Focus steal succeeded, proceed with injection
                }
            }
        }

        // Log the skip for debugging (could be made configurable)
        std::cout << "[InputInjection] Skipping " << eventType << " injection due to: "
                  << skipReasonToString(skipReason) << std::endl;

        return false; // Skip injection
    }

    return true; // Proceed with injection
}

// Mark injection as successful (call after SendInput succeeds)
inline void markInjectionSuccess() {
    InputMetrics::inc(InputMetrics::injectionSuccesses());
}

} // namespace InputInjection
