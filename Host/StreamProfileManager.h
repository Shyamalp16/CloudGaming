#pragma once

#include <cstdint>
#include <mutex>
#include <optional>
#include <string>

#include <nlohmann/json.hpp>

class StreamProfileManager final {
public:
    struct Profile {
        int width = 1920;
        int height = 1080;
        int fps = 60;
        int bitrate = 8000000;
        bool operator==(const Profile& other) const noexcept;
        bool operator!=(const Profile& other) const noexcept { return !(*this == other); }
    };
    struct ClientCapabilities {
        int maxWidth = 0;
        int maxHeight = 0;
        int maxFps = 0;
        int maxBitrate = 0;
        bool h264 = false;
    };
    enum class State { Configured, Pending, Active, Rejected };
    struct Status {
        State state = State::Configured;
        Profile operatorDefault;
        std::optional<Profile> requested;
        std::optional<Profile> active;
        std::string rejectionReason;
        std::uint64_t generation = 0;
    };

    static const char* StateName(State state) noexcept;
    bool Configure(const nlohmann::json& videoConfig, std::string& error);
    bool Request(const std::string& sessionId, const Profile& profile,
                 const ClientCapabilities& capabilities, std::string& error);
    void RequestOperatorDefault();
    std::optional<Profile> TakePending();
    void MarkApplied(const Profile& profile);
    void MarkRejected(const Profile& profile, std::string reason);
    void ClearSession(const std::string& sessionId);
    Status GetStatus() const;

private:
    bool IsSupported(const Profile& profile, std::string& error) const;
    mutable std::mutex mutex_;
    bool allow120Fps_ = false;
    int minBitrate_ = 500000;
    int maxBitrate_ = 50000000;
    Profile operatorDefault_{};
    std::optional<Profile> requested_;
    std::optional<Profile> pending_;
    std::optional<Profile> active_;
    std::string requestedBySession_;
    std::string rejectionReason_;
    State state_ = State::Configured;
    std::uint64_t generation_ = 0;
};
