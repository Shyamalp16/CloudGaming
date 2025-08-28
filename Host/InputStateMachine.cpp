#include "InputStateMachine.h"
#include "InputStats.h"
#include <iostream>
#include <algorithm>
#include <unordered_set>

namespace InputStateMachine {

KeyStateFSM::KeyStateFSM(const FSMConfig& config)
    : config_(config) {
}

KeyStateFSM::~KeyStateFSM() {
    stopWatchdog();
}

void KeyStateFSM::initialize(std::function<void(const std::string& jsCode, bool isDown)> injectCallback) {
    injectCallback_ = injectCallback;
}

TransitionResult KeyStateFSM::processKeyEvent(const std::string& jsCode, bool isDown,
                                             std::chrono::steady_clock::time_point eventTime) {
    std::lock_guard<std::mutex> lock(mutex_);

    // Get or create state info for this key
    auto& stateInfo = keyStates_[jsCode];

    // Check if event is stale (too old)
    if (config_.enableStaleDetection && isStaleEvent(stateInfo, eventTime)) {
        updateMetrics(TransitionResult::IGNORED_STALE);
        staleTransitions_.fetch_add(1);
        return TransitionResult::IGNORED_STALE;
    }

    // Check if transition is valid
    if (!isValidTransition(stateInfo.state, isDown)) {
        updateMetrics(TransitionResult::IGNORED_INVALID);
        invalidTransitions_.fetch_add(1);
        stateInfo.invalidTransitionCount++;
        return TransitionResult::IGNORED_INVALID;
    }

    // Accept the transition
    stateInfo.state = isDown ? KeyState::DOWN : KeyState::UP;
    stateInfo.lastSeen = eventTime;
    stateInfo.transitionCount++;

    // If this is a key-up for a recovered key, mark as recovered
    if (!isDown && stateInfo.state == KeyState::STUCK_RECOVERY) {
        stateInfo.state = KeyState::UP;
        updateMetrics(TransitionResult::RECOVERED);
        totalRecoveries_.fetch_add(1);
        stateInfo.recoveryCount++;
        return TransitionResult::RECOVERED;
    }

    updateMetrics(TransitionResult::ACCEPTED);
    return TransitionResult::ACCEPTED;
}

void KeyStateFSM::releaseAllKeys() {
    std::lock_guard<std::mutex> lock(mutex_);

    std::cout << "[InputStateMachine] Emergency release of all keys" << std::endl;

    for (const auto& [jsCode, stateInfo] : keyStates_) {
        if (stateInfo.state == KeyState::DOWN || stateInfo.state == KeyState::STUCK_RECOVERY) {
            if (injectCallback_) {
                injectCallback_(jsCode, false); // Inject key-up
            }
            std::cout << "[InputStateMachine] Released stuck key: " << jsCode << std::endl;
            totalRecoveries_.fetch_add(1);
        }
    }

    // Reset all states to UP
    for (auto& [jsCode, stateInfo] : keyStates_) {
        stateInfo.state = KeyState::UP;
        stateInfo.lastSeen = std::chrono::steady_clock::now();
    }
}

KeyStateInfo KeyStateFSM::getKeyState(const std::string& jsCode) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = keyStates_.find(jsCode);
    if (it != keyStates_.end()) {
        return it->second;
    }
    return KeyStateInfo{}; // Return default state
}

void KeyStateFSM::startWatchdog() {
    if (running_.load()) return;

    running_.store(true);
    watchdogThread_ = std::thread(&KeyStateFSM::watchdogLoop, this);
}

void KeyStateFSM::stopWatchdog() {
    if (!running_.load()) return;

    running_.store(false);
    if (watchdogThread_.joinable()) {
        watchdogThread_.join();
    }
}

void KeyStateFSM::watchdogLoop() {
    while (running_.load()) {
        std::this_thread::sleep_for(config_.watchdogInterval);

        auto now = std::chrono::steady_clock::now();

        std::lock_guard<std::mutex> lock(mutex_);

        // Check for stuck keys
        for (auto& [jsCode, stateInfo] : keyStates_) {
            if ((stateInfo.state == KeyState::DOWN || stateInfo.state == KeyState::STUCK_RECOVERY) &&
                config_.enableRecovery) {

                // Skip recovery for regular keys if disabled, or if only recovering modifiers
                if (config_.onlyRecoverModifiers && !isModifierKey(jsCode)) {
                    continue;
                }

                // Skip recovery for regular keys if regular key timeout is disabled
                if (!isModifierKey(jsCode) && !config_.enableRegularKeyTimeout) {
                    continue;
                }

                auto timeSinceLastSeen = std::chrono::duration_cast<std::chrono::milliseconds>(
                    now - stateInfo.lastSeen);

                auto keyTimeout = getKeyTimeout(jsCode);

                if (timeSinceLastSeen > keyTimeout) {
                    recoverStuckKey(jsCode);
                }
            }
        }
    }
}

bool KeyStateFSM::isValidTransition(KeyState currentState, bool requestedDown) {
    // Define valid transitions:
    // UP → DOWN (press)
    // DOWN → UP (release)
    // STUCK_RECOVERY → UP (recovery completion)
    // Invalid: DOWN → DOWN, UP → UP

    switch (currentState) {
        case KeyState::UP:
            return requestedDown; // Only allow UP → DOWN

        case KeyState::DOWN:
            return !requestedDown; // Only allow DOWN → UP

        case KeyState::STUCK_RECOVERY:
            return !requestedDown; // Only allow STUCK_RECOVERY → UP

        default:
            return false;
    }
}

bool KeyStateFSM::isStaleEvent(const KeyStateInfo& stateInfo,
                               std::chrono::steady_clock::time_point eventTime) {
    auto timeDiff = std::chrono::duration_cast<std::chrono::milliseconds>(
        eventTime - stateInfo.lastSeen);

    // Event is stale if it's significantly older than the last seen event
    return timeDiff < -config_.staleThreshold;
}

bool KeyStateFSM::isModifierKey(const std::string& jsCode) {
    // Common modifier keys that can get stuck and cause issues
    static const std::unordered_set<std::string> modifierKeys = {
        // Control keys
        "ControlLeft", "ControlRight", "Control",

        // Shift keys
        "ShiftLeft", "ShiftRight", "Shift",

        // Alt keys
        "AltLeft", "AltRight", "Alt",

        // Windows/Meta keys
        "MetaLeft", "MetaRight", "Meta", "OSLeft", "OSRight",

        // Function keys that might be problematic
        "CapsLock", "NumLock", "ScrollLock"
    };

    return modifierKeys.find(jsCode) != modifierKeys.end();
}

std::chrono::milliseconds KeyStateFSM::getKeyTimeout(const std::string& jsCode) {
    if (isModifierKey(jsCode)) {
        return config_.modifierKeyTimeout;
    } else {
        return config_.regularKeyTimeout;
    }
}

void KeyStateFSM::recoverStuckKey(const std::string& jsCode) {
    auto& stateInfo = keyStates_[jsCode];
    auto timeHeld = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - stateInfo.lastSeen).count();

    bool isModifier = isModifierKey(jsCode);
    auto timeoutUsed = getKeyTimeout(jsCode);

    std::cout << "[InputStateMachine] Recovering stuck " << (isModifier ? "modifier" : "regular")
              << " key: " << jsCode << " (held " << timeHeld << "ms, timeout: "
              << timeoutUsed.count() << "ms)" << std::endl;

    // Inject key-up to release the stuck key
    if (injectCallback_) {
        injectCallback_(jsCode, false);
    }

    // Mark as being recovered
    stateInfo.state = KeyState::STUCK_RECOVERY;
    stateInfo.recoveryCount++;
    totalRecoveries_.fetch_add(1);

    // Update metrics with modifier information
    InputStats::Track::keyboardEventTimeoutReleased(isModifier);
}

void KeyStateFSM::updateMetrics(TransitionResult result) {
    // Update global metrics based on transition result
    switch (result) {
        case TransitionResult::ACCEPTED:
            // Could add specific metric here if needed
            break;
        case TransitionResult::IGNORED_INVALID:
            invalidTransitions_.fetch_add(1);
            InputMetrics::inc(InputMetrics::fsmInvalidTransitions());
            break;
        case TransitionResult::IGNORED_STALE:
            staleTransitions_.fetch_add(1);
            InputMetrics::inc(InputMetrics::fsmStaleTransitions());
            break;
        case TransitionResult::RECOVERED:
            totalRecoveries_.fetch_add(1);
            InputMetrics::inc(InputMetrics::fsmRecoveries());
            break;
    }
}

} // namespace InputStateMachine
