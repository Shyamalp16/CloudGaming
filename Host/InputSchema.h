#pragma once

#include <cstddef>
#include <string>
#include <nlohmann/json.hpp>

namespace InputSchema {
constexpr const char* kType = "type";
constexpr const char* kCode = "code";
constexpr const char* kClientSendTime = "client_send_time";
constexpr const char* kX = "x";
constexpr const char* kY = "y";
constexpr const char* kButton = "button";
constexpr const char* kKeyDown = "keydown";
constexpr const char* kKeyUp = "keyup";
constexpr const char* kMouseMove = "mousemove";
constexpr const char* kMouseDown = "mousedown";
constexpr const char* kMouseUp = "mouseup";

constexpr std::size_t kMaxPayloadBytes = 4096;
constexpr std::size_t kMaxNestingDepth = 4;
constexpr std::size_t kMaxObjectMembers = 16;
constexpr int kMaxCoordinate = 16384;
constexpr double kMaxWheelDelta = 10000.0;

struct ValidationResult {
    bool valid = false;
    std::string eventType;
    std::string error;
    nlohmann::json value;
};

ValidationResult Validate(const std::string& payload);
bool IsReleaseEvent(const std::string& eventType) noexcept;
bool IsMouseMove(const std::string& eventType) noexcept;
} // namespace InputSchema
