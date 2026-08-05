#include "InputStateMachine.h"
#include <utility>

namespace InputStateMachine {

void KeyStateFSM::initialize(std::function<void(const std::string& jsCode, bool isDown)> injectCallback) {
    injectCallback_ = std::move(injectCallback);
}

TransitionResult KeyStateFSM::processKeyEvent(const std::string& jsCode, bool isDown) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto& state = keyStates_[jsCode];
    if (!isValidTransition(state, isDown)) {
        return TransitionResult::IGNORED_INVALID;
    }
    state = isDown ? KeyState::DOWN : KeyState::UP;
    return TransitionResult::ACCEPTED;
}

void KeyStateFSM::releaseAllKeys() {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& [jsCode, state] : keyStates_) {
        if (state == KeyState::DOWN && injectCallback_) {
            injectCallback_(jsCode, false);
        }
        state = KeyState::UP;
    }
}

bool KeyStateFSM::isValidTransition(KeyState currentState, bool requestedDown) {
    return requestedDown ? currentState == KeyState::UP : currentState == KeyState::DOWN;
}

} // namespace InputStateMachine
