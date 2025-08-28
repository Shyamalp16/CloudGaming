#pragma once
#include <string>
#include <nlohmann/json.hpp>
#include <chrono>

namespace InputConfig {

/**
 * @brief Key repeat policy enumeration
 */
enum class KeyRepeatPolicy {
    OS_DEFAULT,     // Use OS default repeat behavior
    DISABLED,       // Disable key repeats entirely
    CONTROLLED,     // Use controlled repeat with custom timing
    THROTTLED       // Throttle repeats to prevent flooding
};

/**
 * @brief Input injection policy enumeration
 */
enum class InjectionPolicy {
    ALWAYS_INJECT,      // Always attempt injection regardless of focus
    REQUIRE_FOREGROUND, // Only inject if target window is foreground
    SKIP_IF_BLOCKED,    // Skip injection if window state prevents it
    FOCUS_AND_INJECT    // Attempt to bring to foreground then inject
};

/**
 * @brief Mouse coordinate handling policy
 */
enum class MouseCoordinatePolicy {
    VIRTUAL_DESKTOP,    // Use full virtual desktop coordinates
    CLIP_TO_TARGET,     // Clip coordinates to target window bounds
    RELATIVE_TO_TARGET, // Use coordinates relative to target window
    DPI_AWARE          // Apply DPI scaling to coordinates
};

/**
 * @brief Comprehensive input configuration structure
 */
struct InputConfiguration {
    // Core input blocking policies
    bool blockWinKeys = true;                    // Block LWIN/RWIN key events
    bool blockSystemKeys = false;                // Block other system keys (Alt+Tab, etc.)
    bool releaseAllOnDisconnect = true;          // Release all keys when client disconnects

    // Focus and window policies
    InjectionPolicy injectionPolicy = InjectionPolicy::REQUIRE_FOREGROUND;
    bool allowFocusSteal = false;                // Allow stealing focus from other windows

    // Mouse handling
    MouseCoordinatePolicy mousePolicy = MouseCoordinatePolicy::DPI_AWARE;
    bool clipCursorToTarget = false;             // Clip cursor to target window bounds
    bool coalesceMouseMoves = true;              // Combine rapid mouse moves
    int maxMouseMovesPerFrame = 10;             // Max mouse moves to process per frame

    // Keyboard handling
    KeyRepeatPolicy repeatPolicy = KeyRepeatPolicy::THROTTLED;
    int maxKeyRepeatHz = 30;                    // Max key repeat rate
    std::chrono::milliseconds keyRepeatDelay = std::chrono::milliseconds(250);

    // Input timing and throttling
    int maxInjectHz = 1000;                     // Max input injection rate (Hz)
    std::chrono::milliseconds inputQueueTimeout = std::chrono::milliseconds(100);
    std::chrono::milliseconds stuckKeyTimeout = std::chrono::milliseconds(2000);

    // Recovery and safety
    bool enableStuckKeyRecovery = true;         // Enable automatic stuck key recovery
    bool enableSequenceRecovery = true;         // Enable sequence-based desynchronization recovery
    bool enableMouseSequencing = false;         // Enable sequence processing for mouse events
    int maxRecoveryAttempts = 3;                // Max recovery attempts before giving up

    // Logging and debugging
    bool enablePerEventLogging = false;         // Log every input event (performance impact)
    bool enableAggregatedLogging = true;        // Log aggregated statistics
    std::chrono::milliseconds logInterval = std::chrono::milliseconds(100); // Stats log interval

    // Transport layer configuration
    bool usePionDataChannels = true;            // Use Pion data channels (vs legacy WS)
    bool enableLegacyWebSocket = false;         // Allow legacy WebSocket input (for compatibility)
    int maxPendingMessages = 100;               // Max pending input messages

    // Error handling
    bool strictErrorHandling = false;           // Treat injection errors as fatal
    bool logInjectionErrors = true;             // Log SendInput failures
    int maxConsecutiveErrors = 10;              // Max consecutive errors before throttling
};

/**
 * @brief Global input configuration instance
 */
extern InputConfiguration globalInputConfig;

/**
 * @brief Load input configuration from JSON
 * @param jsonConfig JSON object containing input configuration
 * @return true if loaded successfully, false otherwise
 */
bool loadFromJson(const nlohmann::json& jsonConfig);

/**
 * @brief Save current configuration to JSON
 * @return JSON object with current configuration
 */
nlohmann::json saveToJson();

/**
 * @brief Validate configuration for consistency
 * @return true if configuration is valid, false otherwise
 */
bool validateConfiguration();

/**
 * @brief Reset configuration to defaults
 */
void resetToDefaults();

/**
 * @brief Get configuration summary as string
 * @return Human-readable configuration summary
 */
std::string getConfigurationSummary();

} // namespace InputConfig
