#pragma once

#include <string>
#include <unordered_map>
#include <atomic>
#include <mutex>
#include <chrono>
#include <functional>
#include "Metrics.h"

namespace InputStateMachine {

// FSM States for each key
enum class KeyState {
    UP,           // Key is up
    DOWN,         // Key is down
    STUCK_RECOVERY // Key is being recovered from stuck state
};

// State transition result
enum class TransitionResult {
    ACCEPTED,           // Transition was valid and accepted
    IGNORED_INVALID,    // Transition was invalid (e.g., down→down)
    IGNORED_STALE,      // Transition was stale/outdated
    RECOVERED           // Key was recovered from stuck state
};

// Key state tracking structure
struct KeyStateInfo {
    KeyState state = KeyState::UP;
    std::chrono::steady_clock::time_point lastSeen;
    uint64_t transitionCount = 0;
    uint64_t invalidTransitionCount = 0;
    uint64_t recoveryCount = 0;

    KeyStateInfo() : lastSeen(std::chrono::steady_clock::now()) {}
};

// Configuration for the state machine
struct FSMConfig {
    // General settings
    std::chrono::milliseconds watchdogInterval = std::chrono::milliseconds(500); // Check every 500ms
    bool enableRecovery = true;
    bool enableStaleDetection = true;
    std::chrono::milliseconds staleThreshold = std::chrono::milliseconds(100); // 100ms

    // Timeout settings - separate for modifiers vs regular keys
    std::chrono::milliseconds modifierKeyTimeout = std::chrono::milliseconds(5000); // 5 seconds for modifiers
    std::chrono::milliseconds regularKeyTimeout = std::chrono::milliseconds(30000); // 30 seconds for regular keys
    bool enableRegularKeyTimeout = false; // Disabled by default for regular keys

    // Modifier key detection
    bool onlyRecoverModifiers = true; // Only apply timeout recovery to modifier keys
};

// Main FSM class
class KeyStateFSM {
private:
    std::unordered_map<std::string, KeyStateInfo> keyStates_;
    std::mutex mutex_;
    FSMConfig config_;
    std::atomic<bool> running_{false};
    std::thread watchdogThread_;
    std::function<void(const std::string& jsCode, bool isDown)> injectCallback_;

    // Recovery metrics
    std::atomic<uint64_t> totalRecoveries_{0};
    std::atomic<uint64_t> invalidTransitions_{0};
    std::atomic<uint64_t> staleTransitions_{0};

public:
    KeyStateFSM(const FSMConfig& config = FSMConfig());
    ~KeyStateFSM();

    // Initialize with injection callback
    void initialize(std::function<void(const std::string& jsCode, bool isDown)> injectCallback);

    // Process a key event - returns whether injection should proceed
    TransitionResult processKeyEvent(const std::string& jsCode, bool isDown,
                                   std::chrono::steady_clock::time_point eventTime = std::chrono::steady_clock::now());

    // Emergency recovery - release all keys
    void releaseAllKeys();

    // Get current state (for debugging/testing)
    KeyStateInfo getKeyState(const std::string& jsCode);

    // Get metrics
    uint64_t getTotalRecoveries() const { return totalRecoveries_.load(); }
    uint64_t getInvalidTransitions() const { return invalidTransitions_.load(); }
    uint64_t getStaleTransitions() const { return staleTransitions_.load(); }

    // Start/stop watchdog
    void startWatchdog();
    void stopWatchdog();

private:
    // Check for stuck keys and recover them
    void watchdogLoop();

    // Check if a transition is valid
    bool isValidTransition(KeyState currentState, bool requestedDown);

    // Check if an event is stale
    bool isStaleEvent(const KeyStateInfo& stateInfo, std::chrono::steady_clock::time_point eventTime);

    // Check if a key is a modifier key that should be monitored for stuck state
    bool isModifierKey(const std::string& jsCode);

    // Get the appropriate timeout for a key based on its type
    std::chrono::milliseconds getKeyTimeout(const std::string& jsCode);

    // Perform recovery for a stuck key
    void recoverStuckKey(const std::string& jsCode);

    // Update metrics
    void updateMetrics(TransitionResult result);
};

} // namespace InputStateMachine
