#pragma once

#include <functional>
#include <mutex>
#include <string>
#include <unordered_map>

namespace InputStateMachine {

enum class KeyState {
    UP,
    DOWN
};

enum class TransitionResult {
    ACCEPTED,
    IGNORED_INVALID
};

class KeyStateFSM {
public:
    void initialize(std::function<void(const std::string& jsCode, bool isDown)> injectCallback);
    TransitionResult processKeyEvent(const std::string& jsCode, bool isDown);
    void releaseAllKeys();

private:
    static bool isValidTransition(KeyState currentState, bool requestedDown);

    std::unordered_map<std::string, KeyState> keyStates_;
    std::mutex mutex_;
    std::function<void(const std::string& jsCode, bool isDown)> injectCallback_;
};

} // namespace InputStateMachine
