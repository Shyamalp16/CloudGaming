#pragma once
#ifndef INPUT_CONFIG_H
#define INPUT_CONFIG_H

#include <string>
#include <cstdlib>
#include <algorithm>

// Header-only configuration to avoid linker issues

namespace InputConfig {

inline int& refMinSleepMs() { static int v = 1; return v; }
inline int& refMouseMoveCoalesceHz() { static int v = 60; return v; }
inline int& refWheelScale() { static int v = 120; return v; }
inline bool& refEnableDebugLogs() { static bool v = false; return v; }
inline int& refKeyEventsPerSecond() { static int v = 200; return v; }
inline int& refMouseEventsPerSecond() { static int v = 500; return v; }
inline bool& refInited() { static bool inited = false; return inited; }

inline int parseIntEnv(const char* name, int defValue) {
    if (const char* v = std::getenv(name)) {
        try { return std::max(0, std::stoi(v)); } catch (...) { return defValue; }
    }
    return defValue;
}

inline bool parseBoolEnv(const char* name, bool defValue) {
    if (const char* v = std::getenv(name)) {
        std::string s(v);
        for (auto& c : s) c = (char)tolower(c);
        return (s == "1" || s == "true" || s == "yes" || s == "on");
    }
    return defValue;
}

inline std::string getEnv(const char* name, const std::string& defValue) {
    if (const char* v = std::getenv(name)) return std::string(v);
    return defValue;
}

inline void initialize() {
    if (refInited()) return;
    refMinSleepMs() = parseIntEnv("INPUT_MIN_SLEEP_MS", refMinSleepMs());
    refMouseMoveCoalesceHz() = parseIntEnv("INPUT_MOUSE_COALESCE_HZ", refMouseMoveCoalesceHz());
    refWheelScale() = parseIntEnv("INPUT_WHEEL_SCALE", refWheelScale());
    refEnableDebugLogs() = parseBoolEnv("INPUT_ENABLE_DEBUG_LOGS", refEnableDebugLogs());
    refKeyEventsPerSecond() = parseIntEnv("INPUT_KEY_EVENTS_PER_SEC", refKeyEventsPerSecond());
    refMouseEventsPerSecond() = parseIntEnv("INPUT_MOUSE_EVENTS_PER_SEC", refMouseEventsPerSecond());
    refInited() = true;
}

inline int getMinSleepMs() { return refMinSleepMs(); }
inline int getMouseMoveCoalesceHz() { return refMouseMoveCoalesceHz(); }
inline int getWheelScale() { return refWheelScale(); }
inline bool getEnableDebugLogs() { return refEnableDebugLogs(); }
inline int getKeyEventsPerSecond() { return refKeyEventsPerSecond(); }
inline int getMouseEventsPerSecond() { return refMouseEventsPerSecond(); }

} // namespace InputConfig

#endif // INPUT_CONFIG_H


