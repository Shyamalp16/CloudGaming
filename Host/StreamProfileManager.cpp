#include "StreamProfileManager.h"

#include <algorithm>

bool StreamProfileManager::Profile::operator==(const Profile& other) const noexcept {
    return width == other.width && height == other.height && fps == other.fps && bitrate == other.bitrate;
}

const char* StreamProfileManager::StateName(State state) noexcept {
    switch (state) {
    case State::Configured: return "Configured";
    case State::Pending: return "Pending";
    case State::Active: return "Active";
    case State::Rejected: return "Rejected";
    }
    return "Unknown";
}

bool StreamProfileManager::IsSupported(const Profile& profile, std::string& error) const {
    const bool supportedResolution =
        (profile.width == 1280 && profile.height == 720) ||
        (profile.width == 1920 && profile.height == 1080) ||
        (profile.width == 2560 && profile.height == 1440);
    const bool supportedFps = profile.fps == 30 || profile.fps == 60 ||
        (allow120Fps_ && profile.fps == 120);
    if (!supportedResolution) error = "unsupported resolution (allowed: 720p, 1080p, 1440p)";
    else if (!supportedFps) error = "unsupported frame rate (allowed: 30/60, or configured 120)";
    else if (profile.bitrate < minBitrate_ || profile.bitrate > maxBitrate_) error = "bitrate outside configured limits";
    else return true;
    return false;
}

bool StreamProfileManager::Configure(const nlohmann::json& videoConfig, std::string& error) {
    std::lock_guard<std::mutex> lock(mutex_);
    allow120Fps_ = videoConfig.value("allow120Fps", false);
    minBitrate_ = std::clamp(videoConfig.value("bitrateMin", 3000000), 500000, 50000000);
    maxBitrate_ = std::clamp(videoConfig.value("bitrateMax", 12000000), minBitrate_, 50000000);
    const auto profileConfig = videoConfig.contains("defaultProfile") && videoConfig["defaultProfile"].is_object()
        ? videoConfig["defaultProfile"] : nlohmann::json::object();
    operatorDefault_.width = profileConfig.value("width", videoConfig.value("encodeWidth", 1920));
    operatorDefault_.height = profileConfig.value("height", videoConfig.value("encodeHeight", 1080));
    operatorDefault_.fps = profileConfig.value("fps", videoConfig.value("fps", 60));
    operatorDefault_.bitrate = profileConfig.value("bitrate", videoConfig.value("bitrateStart", 8000000));
    if (!IsSupported(operatorDefault_, error)) return false;
    requested_ = operatorDefault_;
    pending_ = operatorDefault_;
    active_.reset();
    requestedBySession_.clear();
    rejectionReason_.clear();
    state_ = State::Pending;
    ++generation_;
    return true;
}

bool StreamProfileManager::Request(const std::string& sessionId, const Profile& profile,
                                   const ClientCapabilities& capabilities, std::string& error) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (sessionId.empty()) error = "missing authorized session";
    else if (!capabilities.h264) error = "client did not advertise H.264 support";
    else if (capabilities.maxWidth < profile.width || capabilities.maxHeight < profile.height ||
             capabilities.maxFps < profile.fps || capabilities.maxBitrate < profile.bitrate) {
        error = "requested profile exceeds client capabilities";
    } else if (!IsSupported(profile, error)) {
    } else {
        requested_ = profile;
        pending_ = profile;
        requestedBySession_ = sessionId;
        rejectionReason_.clear();
        state_ = State::Pending;
        ++generation_;
        return true;
    }
    requested_ = profile;
    pending_.reset();
    requestedBySession_ = sessionId;
    rejectionReason_ = error;
    state_ = State::Rejected;
    ++generation_;
    return false;
}

void StreamProfileManager::RequestOperatorDefault() {
    std::lock_guard<std::mutex> lock(mutex_);
    requested_ = operatorDefault_;
    pending_ = operatorDefault_;
    requestedBySession_.clear();
    rejectionReason_.clear();
    state_ = State::Pending;
    ++generation_;
}

std::optional<StreamProfileManager::Profile> StreamProfileManager::TakePending() {
    std::lock_guard<std::mutex> lock(mutex_);
    auto result = pending_;
    pending_.reset();
    return result;
}

void StreamProfileManager::MarkApplied(const Profile& profile) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!requested_ || *requested_ != profile) return;
    active_ = profile;
    rejectionReason_.clear();
    state_ = State::Active;
}

void StreamProfileManager::MarkRejected(const Profile& profile, std::string reason) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!requested_ || *requested_ != profile) return;
    rejectionReason_ = std::move(reason);
    state_ = State::Rejected;
}

void StreamProfileManager::ClearSession(const std::string& sessionId) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (requestedBySession_ != sessionId) return;
    requested_ = operatorDefault_;
    pending_ = operatorDefault_;
    requestedBySession_.clear();
    rejectionReason_.clear();
    state_ = State::Pending;
    ++generation_;
}

StreamProfileManager::Status StreamProfileManager::GetStatus() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return {state_, operatorDefault_, requested_, active_, rejectionReason_, generation_};
}
