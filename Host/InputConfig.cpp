#include "InputConfig.h"
#include <iostream>
#include <sstream>

namespace InputConfig {

// Global configuration instance
InputConfiguration globalInputConfig;

/**
 * @brief Convert string to KeyRepeatPolicy enum
 */
KeyRepeatPolicy stringToKeyRepeatPolicy(const std::string& str) {
    if (str == "OS_DEFAULT") return KeyRepeatPolicy::OS_DEFAULT;
    if (str == "DISABLED") return KeyRepeatPolicy::DISABLED;
    if (str == "CONTROLLED") return KeyRepeatPolicy::CONTROLLED;
    if (str == "THROTTLED") return KeyRepeatPolicy::THROTTLED;
    return KeyRepeatPolicy::THROTTLED; // default
}

/**
 * @brief Convert KeyRepeatPolicy enum to string
 */
std::string keyRepeatPolicyToString(KeyRepeatPolicy policy) {
    switch (policy) {
        case KeyRepeatPolicy::OS_DEFAULT: return "OS_DEFAULT";
        case KeyRepeatPolicy::DISABLED: return "DISABLED";
        case KeyRepeatPolicy::CONTROLLED: return "CONTROLLED";
        case KeyRepeatPolicy::THROTTLED: return "THROTTLED";
        default: return "THROTTLED";
    }
}

/**
 * @brief Convert string to InjectionPolicy enum
 */
InjectionPolicy stringToInjectionPolicy(const std::string& str) {
    if (str == "ALWAYS_INJECT") return InjectionPolicy::ALWAYS_INJECT;
    if (str == "REQUIRE_FOREGROUND") return InjectionPolicy::REQUIRE_FOREGROUND;
    if (str == "SKIP_IF_BLOCKED") return InjectionPolicy::SKIP_IF_BLOCKED;
    if (str == "FOCUS_AND_INJECT") return InjectionPolicy::FOCUS_AND_INJECT;
    return InjectionPolicy::REQUIRE_FOREGROUND; // default
}

/**
 * @brief Convert InjectionPolicy enum to string
 */
std::string injectionPolicyToString(InjectionPolicy policy) {
    switch (policy) {
        case InjectionPolicy::ALWAYS_INJECT: return "ALWAYS_INJECT";
        case InjectionPolicy::REQUIRE_FOREGROUND: return "REQUIRE_FOREGROUND";
        case InjectionPolicy::SKIP_IF_BLOCKED: return "SKIP_IF_BLOCKED";
        case InjectionPolicy::FOCUS_AND_INJECT: return "FOCUS_AND_INJECT";
        default: return "REQUIRE_FOREGROUND";
    }
}

/**
 * @brief Convert string to MouseCoordinatePolicy enum
 */
MouseCoordinatePolicy stringToMouseCoordinatePolicy(const std::string& str) {
    if (str == "VIRTUAL_DESKTOP") return MouseCoordinatePolicy::VIRTUAL_DESKTOP;
    if (str == "CLIP_TO_TARGET") return MouseCoordinatePolicy::CLIP_TO_TARGET;
    if (str == "RELATIVE_TO_TARGET") return MouseCoordinatePolicy::RELATIVE_TO_TARGET;
    if (str == "DPI_AWARE") return MouseCoordinatePolicy::DPI_AWARE;
    return MouseCoordinatePolicy::DPI_AWARE; // default
}

/**
 * @brief Convert MouseCoordinatePolicy enum to string
 */
std::string mouseCoordinatePolicyToString(MouseCoordinatePolicy policy) {
    switch (policy) {
        case MouseCoordinatePolicy::VIRTUAL_DESKTOP: return "VIRTUAL_DESKTOP";
        case MouseCoordinatePolicy::CLIP_TO_TARGET: return "CLIP_TO_TARGET";
        case MouseCoordinatePolicy::RELATIVE_TO_TARGET: return "RELATIVE_TO_TARGET";
        case MouseCoordinatePolicy::DPI_AWARE: return "DPI_AWARE";
        default: return "DPI_AWARE";
    }
}

bool loadFromJson(const nlohmann::json& jsonConfig) {
    try {
        if (jsonConfig.contains("blockWinKeys")) {
            globalInputConfig.blockWinKeys = jsonConfig["blockWinKeys"];
        }
        if (jsonConfig.contains("blockSystemKeys")) {
            globalInputConfig.blockSystemKeys = jsonConfig["blockSystemKeys"];
        }
        if (jsonConfig.contains("releaseAllOnDisconnect")) {
            globalInputConfig.releaseAllOnDisconnect = jsonConfig["releaseAllOnDisconnect"];
        }

        // Focus and window policies
        if (jsonConfig.contains("injectionPolicy")) {
            globalInputConfig.injectionPolicy = stringToInjectionPolicy(jsonConfig["injectionPolicy"]);
        }
        if (jsonConfig.contains("allowFocusSteal")) {
            globalInputConfig.allowFocusSteal = jsonConfig["allowFocusSteal"];
        }

        // Mouse handling
        if (jsonConfig.contains("mousePolicy")) {
            globalInputConfig.mousePolicy = stringToMouseCoordinatePolicy(jsonConfig["mousePolicy"]);
        }
        if (jsonConfig.contains("clipCursorToTarget")) {
            globalInputConfig.clipCursorToTarget = jsonConfig["clipCursorToTarget"];
        }
        if (jsonConfig.contains("coalesceMouseMoves")) {
            globalInputConfig.coalesceMouseMoves = jsonConfig["coalesceMouseMoves"];
        }
        if (jsonConfig.contains("maxMouseMovesPerFrame")) {
            globalInputConfig.maxMouseMovesPerFrame = jsonConfig["maxMouseMovesPerFrame"];
        }

        // Keyboard handling
        if (jsonConfig.contains("repeatPolicy")) {
            globalInputConfig.repeatPolicy = stringToKeyRepeatPolicy(jsonConfig["repeatPolicy"]);
        }
        if (jsonConfig.contains("maxKeyRepeatHz")) {
            globalInputConfig.maxKeyRepeatHz = jsonConfig["maxKeyRepeatHz"];
        }
        if (jsonConfig.contains("keyRepeatDelayMs")) {
            globalInputConfig.keyRepeatDelay = std::chrono::milliseconds(jsonConfig["keyRepeatDelayMs"]);
        }

        // Input timing and throttling
        if (jsonConfig.contains("maxInjectHz")) {
            globalInputConfig.maxInjectHz = jsonConfig["maxInjectHz"];
        }
        if (jsonConfig.contains("inputQueueTimeoutMs")) {
            globalInputConfig.inputQueueTimeout = std::chrono::milliseconds(jsonConfig["inputQueueTimeoutMs"]);
        }
        if (jsonConfig.contains("stuckKeyTimeoutMs")) {
            globalInputConfig.stuckKeyTimeout = std::chrono::milliseconds(jsonConfig["stuckKeyTimeoutMs"]);
        }

        // Recovery and safety
        if (jsonConfig.contains("enableStuckKeyRecovery")) {
            globalInputConfig.enableStuckKeyRecovery = jsonConfig["enableStuckKeyRecovery"];
        }
        if (jsonConfig.contains("enableSequenceRecovery")) {
            globalInputConfig.enableSequenceRecovery = jsonConfig["enableSequenceRecovery"];
        }
        if (jsonConfig.contains("enableMouseSequencing")) {
            globalInputConfig.enableMouseSequencing = jsonConfig["enableMouseSequencing"];
        }
        if (jsonConfig.contains("maxRecoveryAttempts")) {
            globalInputConfig.maxRecoveryAttempts = jsonConfig["maxRecoveryAttempts"];
        }

        // Logging and debugging
        if (jsonConfig.contains("enablePerEventLogging")) {
            globalInputConfig.enablePerEventLogging = jsonConfig["enablePerEventLogging"];
        }
        if (jsonConfig.contains("enableAggregatedLogging")) {
            globalInputConfig.enableAggregatedLogging = jsonConfig["enableAggregatedLogging"];
        }
        if (jsonConfig.contains("logIntervalMs")) {
            globalInputConfig.logInterval = std::chrono::milliseconds(jsonConfig["logIntervalMs"]);
        }

        // Transport layer configuration
        if (jsonConfig.contains("usePionDataChannels")) {
            globalInputConfig.usePionDataChannels = jsonConfig["usePionDataChannels"];
        }
        if (jsonConfig.contains("enableLegacyWebSocket")) {
            globalInputConfig.enableLegacyWebSocket = jsonConfig["enableLegacyWebSocket"];
        }
        if (jsonConfig.contains("maxPendingMessages")) {
            globalInputConfig.maxPendingMessages = jsonConfig["maxPendingMessages"];
        }

        // Error handling
        if (jsonConfig.contains("strictErrorHandling")) {
            globalInputConfig.strictErrorHandling = jsonConfig["strictErrorHandling"];
        }
        if (jsonConfig.contains("logInjectionErrors")) {
            globalInputConfig.logInjectionErrors = jsonConfig["logInjectionErrors"];
        }
        if (jsonConfig.contains("maxConsecutiveErrors")) {
            globalInputConfig.maxConsecutiveErrors = jsonConfig["maxConsecutiveErrors"];
        }

        return validateConfiguration();
    } catch (const std::exception& e) {
        std::cerr << "[InputConfig] Error loading configuration: " << e.what() << std::endl;
        return false;
    }
}

nlohmann::json saveToJson() {
    nlohmann::json config;

    // Core input blocking policies
    config["blockWinKeys"] = globalInputConfig.blockWinKeys;
    config["blockSystemKeys"] = globalInputConfig.blockSystemKeys;
    config["releaseAllOnDisconnect"] = globalInputConfig.releaseAllOnDisconnect;

    // Focus and window policies
    config["injectionPolicy"] = injectionPolicyToString(globalInputConfig.injectionPolicy);
    config["allowFocusSteal"] = globalInputConfig.allowFocusSteal;

    // Mouse handling
    config["mousePolicy"] = mouseCoordinatePolicyToString(globalInputConfig.mousePolicy);
    config["clipCursorToTarget"] = globalInputConfig.clipCursorToTarget;
    config["coalesceMouseMoves"] = globalInputConfig.coalesceMouseMoves;
    config["maxMouseMovesPerFrame"] = globalInputConfig.maxMouseMovesPerFrame;

    // Keyboard handling
    config["repeatPolicy"] = keyRepeatPolicyToString(globalInputConfig.repeatPolicy);
    config["maxKeyRepeatHz"] = globalInputConfig.maxKeyRepeatHz;
    config["keyRepeatDelayMs"] = globalInputConfig.keyRepeatDelay.count();

    // Input timing and throttling
    config["maxInjectHz"] = globalInputConfig.maxInjectHz;
    config["inputQueueTimeoutMs"] = globalInputConfig.inputQueueTimeout.count();
    config["stuckKeyTimeoutMs"] = globalInputConfig.stuckKeyTimeout.count();

    // Recovery and safety
    config["enableStuckKeyRecovery"] = globalInputConfig.enableStuckKeyRecovery;
    config["enableSequenceRecovery"] = globalInputConfig.enableSequenceRecovery;
    config["enableMouseSequencing"] = globalInputConfig.enableMouseSequencing;
    config["maxRecoveryAttempts"] = globalInputConfig.maxRecoveryAttempts;

    // Logging and debugging
    config["enablePerEventLogging"] = globalInputConfig.enablePerEventLogging;
    config["enableAggregatedLogging"] = globalInputConfig.enableAggregatedLogging;
    config["logIntervalMs"] = globalInputConfig.logInterval.count();

    // Transport layer configuration
    config["usePionDataChannels"] = globalInputConfig.usePionDataChannels;
    config["enableLegacyWebSocket"] = globalInputConfig.enableLegacyWebSocket;
    config["maxPendingMessages"] = globalInputConfig.maxPendingMessages;

    // Error handling
    config["strictErrorHandling"] = globalInputConfig.strictErrorHandling;
    config["logInjectionErrors"] = globalInputConfig.logInjectionErrors;
    config["maxConsecutiveErrors"] = globalInputConfig.maxConsecutiveErrors;

    return config;
}

bool validateConfiguration() {
    // Basic validation checks
    if (globalInputConfig.maxInjectHz <= 0 || globalInputConfig.maxInjectHz > 10000) {
        std::cerr << "[InputConfig] Invalid maxInjectHz: " << globalInputConfig.maxInjectHz << std::endl;
        return false;
    }

    if (globalInputConfig.maxKeyRepeatHz < 0 || globalInputConfig.maxKeyRepeatHz > 1000) {
        std::cerr << "[InputConfig] Invalid maxKeyRepeatHz: " << globalInputConfig.maxKeyRepeatHz << std::endl;
        return false;
    }

    if (globalInputConfig.maxMouseMovesPerFrame < 1 || globalInputConfig.maxMouseMovesPerFrame > 100) {
        std::cerr << "[InputConfig] Invalid maxMouseMovesPerFrame: " << globalInputConfig.maxMouseMovesPerFrame << std::endl;
        return false;
    }

    if (globalInputConfig.logInterval.count() < 10 || globalInputConfig.logInterval.count() > 60000) {
        std::cerr << "[InputConfig] Invalid logIntervalMs: " << globalInputConfig.logInterval.count() << std::endl;
        return false;
    }

    // Check for conflicting settings
    if (globalInputConfig.usePionDataChannels && globalInputConfig.enableLegacyWebSocket) {
        std::cout << "[InputConfig] Warning: Both Pion and legacy WebSocket enabled - this may cause conflicts" << std::endl;
    }

    if (!globalInputConfig.usePionDataChannels && !globalInputConfig.enableLegacyWebSocket) {
        std::cerr << "[InputConfig] Error: Neither Pion nor legacy WebSocket enabled - no input will be processed" << std::endl;
        return false;
    }

    return true;
}

void resetToDefaults() {
    globalInputConfig = InputConfiguration(); // Uses default values from struct definition
    std::cout << "[InputConfig] Configuration reset to defaults" << std::endl;
}

std::string getConfigurationSummary() {
    std::stringstream ss;
    ss << "=== Input Configuration Summary ===\n";
    ss << "Core Policies:\n";
    ss << "  Block Win Keys: " << (globalInputConfig.blockWinKeys ? "YES" : "NO") << "\n";
    ss << "  Block System Keys: " << (globalInputConfig.blockSystemKeys ? "YES" : "NO") << "\n";
    ss << "  Release All on Disconnect: " << (globalInputConfig.releaseAllOnDisconnect ? "YES" : "NO") << "\n";
    ss << "\nInjection Policy: " << injectionPolicyToString(globalInputConfig.injectionPolicy) << "\n";
    ss << "Allow Focus Steal: " << (globalInputConfig.allowFocusSteal ? "YES" : "NO") << "\n";
    ss << "\nMouse Policy: " << mouseCoordinatePolicyToString(globalInputConfig.mousePolicy) << "\n";
    ss << "Coalesce Mouse Moves: " << (globalInputConfig.coalesceMouseMoves ? "YES" : "NO") << "\n";
    ss << "Max Mouse Moves/Frame: " << globalInputConfig.maxMouseMovesPerFrame << "\n";
    ss << "\nKeyboard Repeat Policy: " << keyRepeatPolicyToString(globalInputConfig.repeatPolicy) << "\n";
    ss << "Max Key Repeat Hz: " << globalInputConfig.maxKeyRepeatHz << "\n";
    ss << "\nPerformance Limits:\n";
    ss << "  Max Injection Hz: " << globalInputConfig.maxInjectHz << "\n";
    ss << "  Stuck Key Timeout: " << globalInputConfig.stuckKeyTimeout.count() << "ms\n";
    ss << "  Log Interval: " << globalInputConfig.logInterval.count() << "ms\n";
    ss << "\nTransport Layer:\n";
    ss << "  Use Pion Data Channels: " << (globalInputConfig.usePionDataChannels ? "YES" : "NO") << "\n";
    ss << "  Enable Legacy WebSocket: " << (globalInputConfig.enableLegacyWebSocket ? "YES" : "NO") << "\n";
    ss << "  Max Pending Messages: " << globalInputConfig.maxPendingMessages << "\n";
    ss << "\nSafety Features:\n";
    ss << "  Stuck Key Recovery: " << (globalInputConfig.enableStuckKeyRecovery ? "ENABLED" : "DISABLED") << "\n";
    ss << "  Sequence Recovery: " << (globalInputConfig.enableSequenceRecovery ? "ENABLED" : "DISABLED") << "\n";
    ss << "  Strict Error Handling: " << (globalInputConfig.strictErrorHandling ? "YES" : "NO") << "\n";

    return ss.str();
}

} // namespace InputConfig
