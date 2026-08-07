#include "InputStateManager.h"
#include <iostream>
#include <sstream>
#include <algorithm>
#include <vector>
#include <nlohmann/json.hpp>
#include "CaptureHelpers.h"

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

    // Held keys are legitimate in games; time alone cannot distinguish a held
    // key from a lost key-up. Recovery is driven by channel reset/sequence
    // recovery unless the operator explicitly enables the time-based timeout.
    if (config.enableStuckKeyRecovery) {
        recoveryThread = std::thread(&Manager::recoveryLoop, this);
    }

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

        const std::string eventType = eventData.value("type", std::string());
        if (eventType == "input_reset") {
            emergencyReleaseAllKeys(eventData.value("reason", std::string("transport_reset")));
            return;
        }
        if (eventType == "stream_config") {
            const int width = eventData.value("width", 0);
            const int height = eventData.value("height", 0);
            const int fps = eventData.value("fps", 0);
            const bool applied = ApplyStreamProfile(width, height, fps);
            if (!applied) {
                LOG_WARNING(ErrorUtils::ErrorCategory::INPUT,
                            "Rejected unsupported stream profile");
            }
            return;
        }

        // Process based on message type
        if (message.type == "pion_data") {
            if (!eventType.empty()) {
                if (eventType == "keydown" || eventType == "keyup") {
                    if (processKeyboardEvent(eventType, eventData, message.data)) {
                        updateMetrics("keysProcessed");
                    }
                } else if (eventType == "mousedown" || eventType == "mouseup" ||
                          eventType == "mousemove" || eventType == "wheel" || eventType == "hwheel") {
                    if (processMouseEvent(eventType, eventData, message.data)) {
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

void Manager::emergencyReleaseAllKeys(const std::string& reason) {
    std::vector<std::pair<std::string, std::string>> eventsToFire;
    {
        std::lock_guard<std::mutex> lock(stateMutex);
        uint64_t ts = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        size_t releasedCount = 0;
        for (const auto& [jsCode, keyInfo] : keyStates) {
            if (keyInfo.state == KeyState::Pressed || keyInfo.state == KeyState::Stuck) {
                updateKeyStateLocked(jsCode, KeyState::Released, ts);

                // Create synthetic keyup event
                nlohmann::json syntheticEvent = {
                    {"type", "keyup"},
                    {"code", jsCode},
                    {"key", jsCode},
                    {"timestamp", ts},
                    {"emergency", true},
                    {"reason", reason}
                };
                eventsToFire.emplace_back("emergency_keyup", syntheticEvent.dump());
                releasedCount++;
            }
        }

        for (auto& [button, buttonInfo] : mouseButtonStates) {
            if (buttonInfo.state == MouseButtonState::Pressed ||
                buttonInfo.state == MouseButtonState::DoubleClick) {
                buttonInfo.state = MouseButtonState::Released;
                buttonInfo.lastEventTime = ts;
                nlohmann::json syntheticEvent = {
                    {"type", "mouseup"},
                    {"button", button},
                    {"x", currentMousePosition.x},
                    {"y", currentMousePosition.y},
                    {"client_send_time", ts},
                    {"emergency", true},
                    {"reason", reason}
                };
                eventsToFire.emplace_back("mouseup", syntheticEvent.dump());
            }
        }

        if (releasedCount > 0) {
            logStateEvent("emergency_release", "Released " + std::to_string(releasedCount) +
                         " keys due to: " + reason);
        }
    }
    for (const auto& [eventType, eventData] : eventsToFire) {
        if (eventCallback) eventCallback(eventType, eventData);
    }
}

StateStats Manager::getStats() const {
    std::lock_guard<std::mutex> lock(statsMutex);
    return stats;
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

    std::vector<std::string> eventsToFire;
    {
        std::lock_guard<std::mutex> lock(stateMutex);
        uint64_t currentTime = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();

        for (auto& [jsCode, keyInfo] : keyStates) {
            if (keyInfo.state == KeyState::Pressed) {
                auto timeout = getKeyTimeout(jsCode);
                uint64_t timeoutMs = std::chrono::duration_cast<std::chrono::milliseconds>(timeout).count();

                if (currentTime - keyInfo.lastEventTime > timeoutMs) {
                    updateMetrics("stuckKeysDetected");
                    std::string ev = recoverStuckKeyLocked(jsCode);
                    if (!ev.empty()) eventsToFire.push_back(std::move(ev));
                }
            }
        }
    }
    // Fire callbacks after releasing lock (avoids re-entrancy / "resource deadlock would occur")
    for (const auto& ev : eventsToFire) {
        if (eventCallback) eventCallback("stuck_key_recovery", ev);
    }
}

std::string Manager::recoverStuckKeyLocked(const std::string& jsCode) {
    // Caller must hold stateMutex. Returns recovery event JSON to fire after lock release.
    auto it = keyStates.find(jsCode);
    if (it == keyStates.end()) return {};

    it->second.state = KeyState::Stuck;

    uint64_t currentTime = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    updateKeyStateLocked(jsCode, KeyState::Released, currentTime);

    updateMetrics("stuckKeysRecovered");
    logStateEvent("stuck_key_recovered", "Recovered stuck key: " + jsCode);

    nlohmann::json recoveryEvent = {
        {"type", "keyup"},
        {"code", jsCode},
        {"timestamp", currentTime},
        {"action", "synthetic_keyup"}
    };
    return recoveryEvent.dump();
}

bool Manager::processKeyboardEvent(const std::string& eventType, const nlohmann::json& eventData,
                                   const std::string& rawData) {
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
        eventCallback(eventType, rawData);
    }

    return true;
}

bool Manager::processMouseEvent(const std::string& eventType, const nlohmann::json& eventData,
                                const std::string& rawData) {
    if (eventType == "mousemove") {
        MousePosition newPosition = transformMouseCoordinates(eventData);
        updateMousePosition(newPosition);

        if (eventCallback) {
            eventCallback(eventType, rawData);
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
            eventCallback(eventType, rawData);
        }

        return true;
    }

    if (eventType == "wheel" || eventType == "hwheel") {
        // Wheel events don't change state, just forward them
        if (eventCallback) {
            eventCallback(eventType, rawData);
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

        // This manager tracks client-space state only. MouseInputHandler performs
        // the single authoritative DPI/window/virtual-desktop transformation at
        // injection time, using the current target window and streamed size.
        return MousePosition(clientX, clientY, deltaX, deltaY, timestamp, true);

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
    updateKeyStateLocked(jsCode, newState, timestamp, sequenceId);
}

void Manager::updateKeyStateLocked(const std::string& jsCode, KeyState newState,
                                   uint64_t timestamp, uint32_t sequenceId) {
    // Caller must hold stateMutex
    KeyInfo& keyInfo = keyStates[jsCode];
    keyInfo.state = newState;
    keyInfo.lastEventTime = timestamp;
    keyInfo.sequenceId = sequenceId;

    if (newState == KeyState::Pressed) {
        keyInfo.downTime = timestamp;
    }

    keyInfo.isModifier = isModifierKey(jsCode);

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

bool Manager::validateKeyTransition(const std::string&,
                                            KeyState oldState, KeyState newState) {
    // Repeated key-down/up messages add injection work and can clog an ordered
    // channel. The current state already represents them, so discard them.
    if (oldState == newState) {
        return false;
    }

    // Special handling for stuck keys
    if (oldState == KeyState::Stuck && newState == KeyState::Released) {
        return true; // Allow recovery transitions
    }

    return true;
}

bool Manager::validateMouseTransition(int,
                                              MouseButtonState oldState, MouseButtonState newState) {
    // Duplicate button transitions only add ordered-channel/injection work and
    // can accidentally generate double-click behavior after retransmission.
    return oldState != newState;
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
