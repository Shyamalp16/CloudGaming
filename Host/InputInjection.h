#pragma once

#include <windows.h>
#include <string>
#include "WindowUtils.h"

namespace InputInjection {

inline bool shouldInjectInput(const std::string&) {
    const HWND target = WindowUtils::GetTargetWindow();
    return target != nullptr &&
           IsWindow(target) &&
           IsWindowVisible(target) &&
           !IsIconic(target) &&
           IsWindowEnabled(target) &&
           GetForegroundWindow() == target;
}

} // namespace InputInjection
