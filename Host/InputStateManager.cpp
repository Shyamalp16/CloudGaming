#include "InputStateManager.h"
#include <iostream>
#include <sstream>
#include <algorithm>
#include <nlohmann/json.hpp>

namespace InputStateManager {

// Global instance
std::unique_ptr<Manager> globalStateManager;

// Modifier key definitions (JavaScript key codes)
const std::unordered_set<std::string> MODIFIER_KEYS = {
    "ShiftLeft", "ShiftRight", "ControlLeft", "ControlRight",
    "AltLeft", "AltRight", "MetaLeft", "MetaRight", "OSLeft", "OSRight"
};

// StateStats implementation
std::string StateStats::toString() const {
    std::stringstream ss;
    ss << "StateStats{";
    ss << "keys:" << keysProcessed;
    ss << ", mouse:" << mouseEventsProcessed;
    ss << ", stuckDetected:" << stuckKeysDetected;
    ss << ", stuckRecovered:" << stuckKeysRecovered;
    ss << ", seqGaps:" << sequenceGapsDetected;
    ss << ", invalidTrans:" << invalidTransitions;
    ss << ", coordTransforms:" << coordinateTransforms;
    ss << ", coordErrors:" << coordinateTransformErrors;
    ss << "}";
    return ss.str();
}

// InputStateManager implementation
bool Manager::initialize(InputEventCallback callback) {
    if (running.load()) {
        LOG_WARNING(ErrorUtils::ErrorCategory::INPUT, "State manager already running");
        return false;
    }

    eventCallback = std::move(callback);
    if (!eventCallback) {
        LOG_SYSTEM_ERROR("Invalid event callback provided");
        return false;
    }

    logStateEvent("initialized", "State manager initialized successfully");
    return true;
}

bool Manager::start() {
    if (running.load()) {
        LOG_WARNING(ErrorUtils::ErrorCategory::INPUT, "State manager already running");
        return true;
    }

    if (!eventCallback) {
        LOG_SYSTEM_ERROR("State manager not initialized");
        return false;
    }

    shouldStop.store(false);
    running.store(true);

    // Start recovery thread
    recoveryThread = std::thread(&Manager::recoveryLoop, this);

    logStateEvent("started", "State manager started successfully");
    return true;
}

void Manager::stop() {
    if (!running.load()) {
        return;
    }

    logStateEvent("stopping", "Stopping state manager...");

    shouldStop.store(true);

    // Join recovery thread
    if (recoveryThread.joinable()) {
        recoveryThread.join();
    }

    // Emergency release all keys on shutdown
    if (config.releaseAllOnDisconnect) {
        emergencyReleaseAllKeys("shutdown");
    }

    // Clear state
    {
        std::lock_guard<std::mutex> lock(stateMutex);
        keyStates.clear();
        mouseButtonStates.clear();
        currentMousePosition = MousePosition();
    }

    running.store(false);
    logStateEvent("stopped", "State manager stopped and cleaned up");
}

void Manager::processInputMessage(const InputTransportLayer::InputMessage& message) {
    try {
        // Parse JSON data
        nlohmann::json eventData = nlohmann::json::parse(message.data);

        // Process based on message type
        if (message.type == "pion_data") {
            // Extract actual event type from data
            if (eventData.contains("type")) {
                std::string eventType = eventData["type"];
                if (eventType == "keydown" || eventType == "keyup") {
                    if (processKeyboardEvent(eventType, eventData)) {
                        updateMetrics("keysProcessed");
                    }
                } else if (eventType == "mousedown" || eventType == "mouseup" ||
                          eventType == "mousemove" || eventType == "wheel") {
                    if (processMouseEvent(eventType, eventData)) {
                        updateMetrics("mouseEventsProcessed");
                    }
                }
            }
        }

    } catch (const std::exception& e) {
        LOG_INPUT_ERROR("Failed to process input message: " + std::string(e.what()), message.data);
        updateMetrics("invalidTransitions");
    }
}

KeyInfo Manager::getKeyState(const std::string& jsCode) const {
    std::lock_guard<std::mutex> lock(stateMutex);
    auto it = keyStates.find(jsCode);
    if (it != keyStates.end()) {
        return it->second;
    }
    return KeyInfo(); // Default UP state
}

MouseButtonInfo Manager::getMouseButtonState(int button) const {
    std::lock_guard<std::mutex> lock(stateMutex);
    auto it = mouseButtonStates.find(button);
    if (it != mouseButtonStates.end()) {
        return it->second;
    }
    return MouseButtonInfo(); // Default UP state
}

MousePosition Manager::getMousePosition() const {
    std::lock_guard<std::mutex> lock(stateMutex);
    return currentMousePosition;
}

void Manager::emergencyReleaseAllKeys(const std::string& reason) {
    std::lock_guard<std::mutex> lock(stateMutex);

    size_t releasedCount = 0;
    for (const auto& [jsCode, keyInfo] : keyStates) {
        if (keyInfo.state == KeyState::Pressed || keyInfo.state == KeyState::Stuck) {
            updateKeyState(jsCode, KeyState::Released, std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count());

            // Create synthetic keyup event
            nlohmann::json syntheticEvent = {
                {"type", "keyup"},
                {"code", jsCode},
                {"key", jsCode}, // Simplified mapping
                {"timestamp", std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::system_clock::now().time_since_epoch()).count()},
                {"emergency", true},
                {"reason", reason}
            };

            if (eventCallback) {
                eventCallback("emergency_keyup", syntheticEvent.dump());
            }

            releasedCount++;
        }
    }

    if (releasedCount > 0) {
        logStateEvent("emergency_release", "Released " + std::to_string(releasedCount) +
                     " keys due to: " + reason);
    }
}

bool Manager::isKeyStuck(const std::string& jsCode) const {
    std::lock_guard<std::mutex> lock(stateMutex);
    auto it = keyStates.find(jsCode);
    return it != keyStates.end() && it->second.state == KeyState::Stuck;
}

size_t Manager::getStuckKeyCount() const {
    std::lock_guard<std::mutex> lock(stateMutex);
    return std::count_if(keyStates.begin(), keyStates.end(),
                        [](const auto& pair) { return pair.second.state == KeyState::Stuck; });
}

const StateStats& Manager::getStats() const {
    std::lock_guard<std::mutex> lock(statsMutex);
    return stats;
}

void Manager::resetStats() {
    std::lock_guard<std::mutex> lock(statsMutex);
    stats.reset();
    logStateEvent("stats_reset", "State statistics reset");
}

bool Manager::isRunning() const {
    return running.load();
}

// Private methods
void Manager::recoveryLoop() {
    logStateEvent("recovery_loop_started", "Recovery loop starting");

    while (running.load() && !shouldStop.load()) {
        try {
            checkForStuckKeys();

            // Sleep for recovery check interval (use config value)
            std::this_thread::sleep_for(std::chrono::milliseconds(100));

        } catch (const std::exception& e) {
            LOG_INPUT_ERROR("Exception in recovery loop: " + std::string(e.what()), "");
            std::this_thread::sleep_for(std::chrono::milliseconds(1000)); // Longer sleep on error
        }
    }

    logStateEvent("recovery_loop_stopped", "Recovery loop stopped");
}

void Manager::checkForStuckKeys() {
    if (!config.enableStuckKeyRecovery) {
        return;
    }

    std::lock_guard<std::mutex> lock(stateMutex);
    uint64_t currentTime = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();

    for (auto& [jsCode, keyInfo] : keyStates) {
        if (keyInfo.state == KeyState::Pressed) {
            auto timeout = getKeyTimeout(jsCode);
            uint64_t timeoutMs = std::chrono::duration_cast<std::chrono::milliseconds>(timeout).count();

            if (currentTime - keyInfo.lastEventTime > timeoutMs) {
                // Mark as stuck and initiate recovery
                updateMetrics("stuckKeysDetected");
                recoverStuckKey(jsCode);
            }
        }
    }
}

void Manager::recoverStuckKey(const std::string& jsCode) {
    // Mark key as stuck
    {
        std::lock_guard<std::mutex> lock(stateMutex);
        if (keyStates.find(jsCode) != keyStates.end()) {
            keyStates[jsCode].state = KeyState::Stuck;
        }
    }

    // Create recovery event
    nlohmann::json recoveryEvent = {
        {"type", "stuck_key_recovery"},
        {"code", jsCode},
        {"timestamp", std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count()},
        {"action", "synthetic_keyup"}
    };

    if (eventCallback) {
        eventCallback("stuck_key_recovery", recoveryEvent.dump());
    }

    // Update key state to UP
    uint64_t currentTime = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    updateKeyState(jsCode, KeyState::Released, currentTime);

    updateMetrics("stuckKeysRecovered");
    logStateEvent("stuck_key_recovered", "Recovered stuck key: " + jsCode);
}

bool Manager::processKeyboardEvent(const std::string& eventType, const nlohmann::json& eventData) {
    if (!eventData.contains("code")) {
        LOG_WARNING(ErrorUtils::ErrorCategory::INPUT, "Keyboard event missing 'code' field");
        return false;
    }

    std::string jsCode = eventData["code"];
    KeyState newState = (eventType == "keydown") ? KeyState::Pressed : KeyState::Released;

    uint64_t timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();

    uint32_t sequenceId = 0;
    if (eventData.contains("sequenceId")) {
        sequenceId = eventData["sequenceId"];
    }

    // Validate transition
    KeyInfo currentInfo = getKeyState(jsCode);
    if (!validateKeyTransition(jsCode, currentInfo.state, newState)) {
        updateMetrics("invalidTransitions");
        return false;
    }

    // Update state
    updateKeyState(jsCode, newState, timestamp, sequenceId);

    // Forward event to callback
    if (eventCallback) {
        eventCallback(eventType, eventData.dump());
    }

    return true;
}

bool Manager::processMouseEvent(const std::string& eventType, const nlohmann::json& eventData) {
    if (eventType == "mousemove") {
        MousePosition newPosition = transformMouseCoordinates(eventData);
        updateMousePosition(newPosition);

        if (eventCallback) {
            eventCallback(eventType, eventData.dump());
        }
        return true;
    }

    // Handle button events
    if ((eventType == "mousedown" || eventType == "mouseup") && eventData.contains("button")) {
        int button = eventData["button"];
        MouseButtonState newState = (eventType == "mousedown") ? MouseButtonState::Pressed : MouseButtonState::Released;

        uint64_t timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();

        uint32_t sequenceId = 0;
        if (eventData.contains("sequenceId")) {
            sequenceId = eventData["sequenceId"];
        }

        // Validate transition
        MouseButtonInfo currentInfo = getMouseButtonState(button);
        if (!validateMouseTransition(button, currentInfo.state, newState)) {
            updateMetrics("invalidTransitions");
            return false;
        }

        // Update state
        updateMouseButtonState(button, newState, timestamp, sequenceId);

        // Forward event to callback
        if (eventCallback) {
            eventCallback(eventType, eventData.dump());
        }

        return true;
    }

    if (eventType == "wheel") {
        // Wheel events don't change state, just forward them
        if (eventCallback) {
            eventCallback(eventType, eventData.dump());
        }
        return true;
    }

    LOG_WARNING(ErrorUtils::ErrorCategory::INPUT, "Unknown mouse event type: " + eventType);
    return false;
}

MousePosition Manager::transformMouseCoordinates(const nlohmann::json& eventData) {
    updateMetrics("coordinateTransforms");

    try {
        int clientX = eventData.value("x", 0);
        int clientY = eventData.value("y", 0);
        int deltaX = eventData.value("deltaX", 0);
        int deltaY = eventData.value("deltaY", 0);

        uint64_t timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();

        // Apply coordinate transformation based on config
        int transformedX = clientX;
        int transformedY = clientY;

        if (config.mousePolicy == InputConfig::MouseCoordinatePolicy::DPI_AWARE) {
            // TODO: Implement DPI-aware transformation
            // For now, use simple scaling
            transformedX = static_cast<int>(clientX * 1.0f);
            transformedY = static_cast<int>(clientY * 1.0f);
        } else if (config.mousePolicy == InputConfig::MouseCoordinatePolicy::CLIP_TO_TARGET) {
            // TODO: Implement clipping to target window
            transformedX = std::clamp(clientX, 0, 1920); // Default bounds
            transformedY = std::clamp(clientY, 0, 1080);
        }

        return MousePosition(transformedX, transformedY, deltaX, deltaY, timestamp, true);

    } catch (const std::exception& e) {
        LOG_INPUT_ERROR("Coordinate transformation failed: " + std::string(e.what()), eventData.dump());
        updateMetrics("coordinateTransformErrors");
        return MousePosition();
    }
}

bool Manager::isModifierKey(const std::string& jsCode) const {
    return MODIFIER_KEYS.find(jsCode) != MODIFIER_KEYS.end();
}

std::chrono::milliseconds Manager::getKeyTimeout(const std::string& jsCode) const {
    if (isModifierKey(jsCode)) {
        return config.stuckKeyTimeout; // Longer timeout for modifiers
    }
    return config.stuckKeyTimeout / 2; // Shorter timeout for regular keys
}

void Manager::updateKeyState(const std::string& jsCode, KeyState newState,
                                      uint64_t timestamp, uint32_t sequenceId) {
    std::lock_guard<std::mutex> lock(stateMutex);

    KeyInfo& keyInfo = keyStates[jsCode];
    KeyState oldState = keyInfo.state;

    keyInfo.state = newState;
    keyInfo.lastEventTime = timestamp;
    keyInfo.sequenceId = sequenceId;

    if (newState == KeyState::Pressed) {
        keyInfo.downTime = timestamp;
    }

    keyInfo.isModifier = isModifierKey(jsCode);

    if (config.enableAggregatedLogging) {
        logStateEvent("key_state_update", jsCode + ": " +
                     (oldState == KeyState::Released ? "UP" : "DOWN") + " -> " +
                     (newState == KeyState::Released ? "UP" : "DOWN"));
    }
}

void Manager::updateMouseButtonState(int button, MouseButtonState newState,
                                             uint64_t timestamp, uint32_t sequenceId) {
    std::lock_guard<std::mutex> lock(stateMutex);

    MouseButtonInfo& buttonInfo = mouseButtonStates[button];
    buttonInfo.state = newState;
    buttonInfo.lastEventTime = timestamp;
    buttonInfo.sequenceId = sequenceId;

    if (newState == MouseButtonState::Pressed) {
        buttonInfo.downTime = timestamp;
    }
}

void Manager::updateMousePosition(const MousePosition& newPosition) {
    std::lock_guard<std::mutex> lock(stateMutex);
    currentMousePosition = newPosition;
}

bool Manager::validateKeyTransition(const std::string& jsCode,
                                            KeyState oldState, KeyState newState) {
    // Allow all transitions for now, but log unusual ones
    if (oldState == newState) {
        if (config.enableAggregatedLogging) {
            LOG_WARNING(ErrorUtils::ErrorCategory::INPUT,
                       "Redundant key state transition for " + jsCode);
        }
        return true; // Allow redundant transitions
    }

    // Special handling for stuck keys
    if (oldState == KeyState::Stuck && newState == KeyState::Released) {
        return true; // Allow recovery transitions
    }

    return true;
}

bool Manager::validateMouseTransition(int button,
                                              MouseButtonState oldState, MouseButtonState newState) {
    // Allow all transitions for now
    return true;
}

void Manager::logStateEvent(const std::string& event, const std::string& details) {
    if (config.enableAggregatedLogging) {
        std::string message = "State event: " + event;
        if (!details.empty()) {
            message += " - " + details;
        }
        LOG_INFO(ErrorUtils::ErrorCategory::INPUT, message);
    }
}

void Manager::updateMetrics(const std::string& metricType, uint64_t value) {
    std::lock_guard<std::mutex> lock(statsMutex);

    if (metricType == "keysProcessed") {
        stats.keysProcessed += value;
    } else if (metricType == "mouseEventsProcessed") {
        stats.mouseEventsProcessed += value;
    } else if (metricType == "stuckKeysDetected") {
        stats.stuckKeysDetected += value;
    } else if (metricType == "stuckKeysRecovered") {
        stats.stuckKeysRecovered += value;
    } else if (metricType == "sequenceGapsDetected") {
        stats.sequenceGapsDetected += value;
    } else if (metricType == "invalidTransitions") {
        stats.invalidTransitions += value;
    } else if (metricType == "coordinateTransforms") {
        stats.coordinateTransforms += value;
    } else if (metricType == "coordinateTransformErrors") {
        stats.coordinateTransformErrors += value;
    }
}

// Global functions
bool initializeGlobalStateManager(Manager::InputEventCallback callback) {
    if (globalStateManager) {
        LOG_WARNING(ErrorUtils::ErrorCategory::INPUT, "Global state manager already initialized");
        return true;
    }

    globalStateManager = std::make_unique<Manager>();
    if (!globalStateManager->initialize(std::move(callback))) {
        LOG_SYSTEM_ERROR("Failed to initialize global state manager");
        globalStateManager.reset();
        return false;
    }

    LOG_INFO(ErrorUtils::ErrorCategory::INPUT, "Global state manager initialized successfully");
    return true;
}

bool startGlobalStateManager() {
    if (!globalStateManager) {
        LOG_SYSTEM_ERROR("Global state manager not initialized");
        return false;
    }

    return globalStateManager->start();
}

void stopGlobalStateManager() {
    if (globalStateManager) {
        globalStateManager->stop();
        LOG_INFO(ErrorUtils::ErrorCategory::INPUT, "Global state manager stopped");
    }
}

Manager* getGlobalStateManager() {
    return globalStateManager.get();
}

} // namespace InputStateManager
