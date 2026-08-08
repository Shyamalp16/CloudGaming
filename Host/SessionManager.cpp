#include "SessionManager.h"

#include <algorithm>
#include <cctype>

const char* SessionManager::StateName(State state) noexcept {
    switch (state) {
    case State::Idle: return "Idle";
    case State::Pairing: return "Pairing";
    case State::Authorized: return "Authorized";
    case State::Connected: return "Connected";
    case State::Reconnecting: return "Reconnecting";
    case State::Terminating: return "Terminating";
    case State::Failed: return "Failed";
    }
    return "Unknown";
}

bool SessionManager::Authorize(const std::string& sessionId) {
    if (sessionId.size() < 16 || sessionId.size() > 128 ||
        !std::all_of(sessionId.begin(), sessionId.end(), [](unsigned char c) {
            return std::isalnum(c) || c == '-' || c == '_';
        })) return false;
    std::lock_guard<std::mutex> lock(mutex_);
    state_ = State::Authorized;
    sessionId_ = sessionId;
    failureReason_.clear();
    return true;
}

bool SessionManager::Accepts(const std::string& sessionId) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return !sessionId_.empty() && sessionId_ == sessionId && state_ != State::Terminating && state_ != State::Idle;
}

void SessionManager::MarkConnected(const std::string& sessionId) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (sessionId_ == sessionId) state_ = State::Connected;
}

void SessionManager::MarkReconnecting() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!sessionId_.empty()) state_ = State::Reconnecting;
}

void SessionManager::Terminate(const std::string& reason) {
    std::lock_guard<std::mutex> lock(mutex_);
    state_ = State::Idle;
    sessionId_.clear();
    failureReason_ = reason;
}

SessionManager::Status SessionManager::GetStatus() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return {state_, sessionId_, failureReason_};
}
