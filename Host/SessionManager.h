#pragma once

#include <chrono>
#include <mutex>
#include <string>

class SessionManager final {
public:
    enum class State { Idle, Pairing, Authorized, Connected, Reconnecting, Terminating, Failed };
    struct Status {
        State state = State::Idle;
        std::string sessionId;
        std::string failureReason;
    };

    static const char* StateName(State state) noexcept;
    bool Authorize(const std::string& sessionId);
    bool Accepts(const std::string& sessionId) const;
    void MarkConnected(const std::string& sessionId);
    void MarkReconnecting();
    void Terminate(const std::string& reason = {});
    Status GetStatus() const;

private:
    mutable std::mutex mutex_;
    State state_ = State::Idle;
    std::string sessionId_;
    std::string failureReason_;
};
