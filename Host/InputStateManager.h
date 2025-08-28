#pragma once
#include <unordered_map>
#include <unordered_set>
#include <mutex>
#include <atomic>
#include <thread>
#include <chrono>
#include <functional>
#include <memory>
#include <string>

#include "InputConfig.h"
#include "InputTransportLayer.h"
#include "ErrorUtils.h"
#include "Metrics.h"
#include <nlohmann/json.hpp>

namespace InputStateManager {

/**
 * @brief Key state enumeration
 */
enum class KeyState {
    Released,   // Key is released
    Pressed,    // Key is pressed
    Stuck       // Key is detected as stuck and being recovered
};

/**
 * @brief Mouse button state enumeration
 */
enum class MouseButtonState {
    Released,   // Button is released
    Pressed,    // Button is pressed
    DoubleClick // Button is in double-click state (for future use)
};

/**
 * @brief Key information structure
 */
struct KeyInfo {
    KeyState state = KeyState::Released;
    uint64_t lastEventTime = 0;     // Timestamp of last state change
    uint64_t downTime = 0;          // Timestamp when key was pressed
    uint32_t sequenceId = 0;        // Last sequence ID for this key
    bool isModifier = false;        // Whether this is a modifier key

    KeyInfo() = default;
    KeyInfo(KeyState s, uint64_t time, uint32_t seq = 0, bool mod = false)
        : state(s), lastEventTime(time), sequenceId(seq), isModifier(mod) {}
};

/**
 * @brief Mouse button information structure
 */
struct MouseButtonInfo {
    MouseButtonState state = MouseButtonState::Released;
    uint64_t lastEventTime = 0;     // Timestamp of last state change
    uint64_t downTime = 0;          // Timestamp when button was pressed
    uint32_t sequenceId = 0;        // Last sequence ID for this button

    MouseButtonInfo() = default;
    MouseButtonInfo(MouseButtonState s, uint64_t time, uint32_t seq = 0)
        : state(s), lastEventTime(time), sequenceId(seq) {}
};

/**
 * @brief Mouse position and movement information
 */
struct MousePosition {
    int x = 0;
    int y = 0;
    int deltaX = 0;
    int deltaY = 0;
    uint64_t timestamp = 0;
    bool isAbsolute = false;        // Whether coordinates are absolute or relative

    MousePosition() = default;
    MousePosition(int px, int py, int dx = 0, int dy = 0, uint64_t ts = 0, bool abs = false)
        : x(px), y(py), deltaX(dx), deltaY(dy), timestamp(ts), isAbsolute(abs) {}
};

/**
 * @brief State manager statistics
 */
struct StateStats {
    uint64_t keysProcessed = 0;
    uint64_t mouseEventsProcessed = 0;
    uint64_t stuckKeysDetected = 0;
    uint64_t stuckKeysRecovered = 0;
    uint64_t sequenceGapsDetected = 0;
    uint64_t invalidTransitions = 0;
    uint64_t coordinateTransforms = 0;
    uint64_t coordinateTransformErrors = 0;

    void reset() {
        keysProcessed = 0;
        mouseEventsProcessed = 0;
        stuckKeysDetected = 0;
        stuckKeysRecovered = 0;
        sequenceGapsDetected = 0;
        invalidTransitions = 0;
        coordinateTransforms = 0;
        coordinateTransformErrors = 0;
    }

    std::string toString() const;
};

/**
 * @brief Input state manager - manages keyboard/mouse state and recovery
 *
 * This component provides centralized state management for input handling:
 * - Tracks current state of all keys and mouse buttons
 * - Handles stuck key detection and recovery
 * - Manages sequence IDs for desynchronization recovery
 * - Provides mouse coordinate transformation
 * - Integrates with input configuration system
 */
class Manager {
public:
    /**
     * @brief Input event callback type
     */
    using InputEventCallback = std::function<void(const std::string& eventType, const std::string& eventData)>;

    /**
     * @brief Initialize the state manager
     * @param callback Function to call when processed input events are ready
     * @return true if initialization successful, false otherwise
     */
    bool initialize(InputEventCallback callback);

    /**
     * @brief Start the state manager (begin processing and recovery)
     * @return true if started successfully, false otherwise
     */
    bool start();

    /**
     * @brief Stop the state manager and clean up resources
     */
    void stop();

    /**
     * @brief Process an input message from the transport layer
     * @param message The input message to process
     */
    void processInputMessage(const InputTransportLayer::InputMessage& message);

    /**
     * @brief Get current state of a specific key
     * @param jsCode JavaScript key code
     * @return Current key state information
     */
    KeyInfo getKeyState(const std::string& jsCode) const;

    /**
     * @brief Get current state of a mouse button
     * @param button Button identifier (0-4 for standard buttons, 5+ for extended)
     * @return Current button state information
     */
    MouseButtonInfo getMouseButtonState(int button) const;

    /**
     * @brief Get current mouse position
     * @return Current mouse position information
     */
    MousePosition getMousePosition() const;

    /**
     * @brief Force release all keys (emergency function)
     * @param reason Reason for the emergency release
     */
    void emergencyReleaseAllKeys(const std::string& reason = "emergency");

    /**
     * @brief Check if a specific key is currently stuck
     * @param jsCode JavaScript key code
     * @return true if key is stuck, false otherwise
     */
    bool isKeyStuck(const std::string& jsCode) const;

    /**
     * @brief Get the number of currently stuck keys
     * @return Number of stuck keys
     */
    size_t getStuckKeyCount() const;

    /**
     * @brief Get current statistics
     * @return Reference to current statistics
     */
    const StateStats& getStats() const;

    /**
     * @brief Reset statistics
     */
    void resetStats();

    /**
     * @brief Check if the state manager is running
     * @return true if running, false otherwise
     */
    bool isRunning() const;

private:
    // Configuration
    const InputConfig::InputConfiguration& config = InputConfig::globalInputConfig;

    // State tracking
    mutable std::mutex stateMutex;
    std::unordered_map<std::string, KeyInfo> keyStates;           // jsCode -> KeyInfo
    std::unordered_map<int, MouseButtonInfo> mouseButtonStates;   // button -> MouseButtonInfo
    MousePosition currentMousePosition;

    // Recovery and monitoring
    std::thread recoveryThread;
    std::atomic<bool> running{false};
    std::atomic<bool> shouldStop{false};
    mutable std::mutex statsMutex;
    StateStats stats;

    // Callbacks
    InputEventCallback eventCallback;

    // Private methods
    void recoveryLoop();
    void checkForStuckKeys();
    void recoverStuckKey(const std::string& jsCode);
    bool processKeyboardEvent(const std::string& eventType, const nlohmann::json& eventData);
    bool processMouseEvent(const std::string& eventType, const nlohmann::json& eventData);
    MousePosition transformMouseCoordinates(const nlohmann::json& eventData);
    bool isModifierKey(const std::string& jsCode) const;
    std::chrono::milliseconds getKeyTimeout(const std::string& jsCode) const;
    void updateKeyState(const std::string& jsCode, KeyState newState, uint64_t timestamp, uint32_t sequenceId = 0);
    void updateMouseButtonState(int button, MouseButtonState newState, uint64_t timestamp, uint32_t sequenceId = 0);
    void updateMousePosition(const MousePosition& newPosition);
    bool validateKeyTransition(const std::string& jsCode, KeyState oldState, KeyState newState);
    bool validateMouseTransition(int button, MouseButtonState oldState, MouseButtonState newState);
    void logStateEvent(const std::string& event, const std::string& details = "");
    void updateMetrics(const std::string& metricType, uint64_t value = 1);
};

/**
 * @brief Global state manager instance
 */
extern std::unique_ptr<Manager> globalStateManager;

/**
 * @brief Initialize the global state manager
 * @param callback Input event processing callback
 * @return true if initialization successful, false otherwise
 */
bool initializeGlobalStateManager(Manager::InputEventCallback callback);

/**
 * @brief Start the global state manager
 * @return true if started successfully, false otherwise
 */
bool startGlobalStateManager();

/**
 * @brief Stop the global state manager
 */
void stopGlobalStateManager();

/**
 * @brief Get the global state manager instance
 * @return Pointer to global instance (nullptr if not initialized)
 */
Manager* getGlobalStateManager();

} // namespace InputStateManager
