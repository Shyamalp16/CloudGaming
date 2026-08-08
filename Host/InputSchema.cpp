#include "InputSchema.h"

#include <cmath>
#include <limits>
#include <unordered_set>

namespace InputSchema {
namespace {
bool CheckShape(const nlohmann::json& value, std::size_t depth, std::string& error) {
    if (depth > kMaxNestingDepth) { error = "JSON nesting limit exceeded"; return false; }
    if (value.is_array()) { error = "Input arrays are not allowed"; return false; }
    if (value.is_object()) {
        if (value.size() > kMaxObjectMembers) { error = "Too many input fields"; return false; }
        for (auto it = value.begin(); it != value.end(); ++it) {
            if (!CheckShape(it.value(), depth + 1, error)) return false;
        }
    }
    return true;
}

bool HasOnly(const nlohmann::json& value, const std::unordered_set<std::string>& allowed,
             std::string& error) {
    for (auto it = value.begin(); it != value.end(); ++it) {
        if (allowed.find(it.key()) == allowed.end()) {
            error = "Unknown input field: " + it.key();
            return false;
        }
    }
    return true;
}

bool IsFiniteNumber(const nlohmann::json& value) {
    return value.is_number() && std::isfinite(value.get<double>());
}

bool ValidateCommon(const nlohmann::json& value, std::string& error) {
    if (value.contains("sequenceId") &&
        (!value["sequenceId"].is_number_unsigned() ||
         value["sequenceId"].get<std::uint64_t>() > std::numeric_limits<std::uint32_t>::max())) {
        error = "Invalid sequenceId"; return false;
    }
    if (value.contains(kClientSendTime) &&
        (!value[kClientSendTime].is_number_integer() || value[kClientSendTime].get<std::int64_t>() < 0)) {
        error = "Invalid client_send_time"; return false;
    }
    if (value.contains("sessionId") &&
        (!value["sessionId"].is_string() || value["sessionId"].get_ref<const std::string&>().size() > 128)) {
        error = "Invalid sessionId"; return false;
    }
    return true;
}

bool ValidateCoordinates(const nlohmann::json& value, std::string& error) {
    if (!value.contains(kX) || !value.contains(kY) ||
        !value[kX].is_number_integer() || !value[kY].is_number_integer()) {
        error = "Mouse coordinates must be integers"; return false;
    }
    const auto x = value[kX].get<std::int64_t>();
    const auto y = value[kY].get<std::int64_t>();
    if (x < 0 || y < 0 || x > kMaxCoordinate || y > kMaxCoordinate) {
        error = "Mouse coordinates are out of range"; return false;
    }
    return true;
}
} // namespace

ValidationResult Validate(const std::string& payload) {
    ValidationResult result;
    if (payload.empty() || payload.size() > kMaxPayloadBytes) {
        result.error = "Input payload size is invalid"; return result;
    }
    result.value = nlohmann::json::parse(payload, nullptr, false);
    if (result.value.is_discarded() || !result.value.is_object()) {
        result.error = "Malformed input JSON"; return result;
    }
    if (!CheckShape(result.value, 1, result.error)) return result;
    if (!result.value.contains(kType) || !result.value[kType].is_string()) {
        result.error = "Missing input type"; return result;
    }
    result.eventType = result.value[kType].get<std::string>();
    if (result.eventType.size() > 32 || !ValidateCommon(result.value, result.error)) return result;

    const std::unordered_set<std::string> common = {
        kType, kClientSendTime, "sequenceId", "sessionId", "reason"
    };
    if (result.eventType == kKeyDown || result.eventType == kKeyUp) {
        auto allowed = common; allowed.insert(kCode); allowed.insert("key");
        if (!HasOnly(result.value, allowed, result.error)) return result;
        if (!result.value.contains(kCode) || !result.value[kCode].is_string()) {
            result.error = "Missing key code"; return result;
        }
        const auto& code = result.value[kCode].get_ref<const std::string&>();
        if (code.empty() || code.size() > 32) { result.error = "Invalid key code"; return result; }
        if (result.value.contains("key") &&
            (!result.value["key"].is_string() || result.value["key"].get_ref<const std::string&>().size() > 32)) {
            result.error = "Invalid key value"; return result;
        }
    } else if (result.eventType == kMouseMove || result.eventType == kMouseDown ||
               result.eventType == kMouseUp || result.eventType == "wheel" || result.eventType == "hwheel") {
        auto allowed = common;
        for (const char* field : {kX, kY, kButton, "deltaX", "deltaY"}) allowed.insert(field);
        if (!HasOnly(result.value, allowed, result.error) || !ValidateCoordinates(result.value, result.error)) return result;
        if (result.eventType == kMouseDown || result.eventType == kMouseUp) {
            if (!result.value.contains(kButton) || !result.value[kButton].is_number_integer()) {
                result.error = "Missing mouse button"; return result;
            }
            const int button = result.value[kButton].get<int>();
            if (button < 0 || button > 4) { result.error = "Invalid mouse button"; return result; }
        } else if (result.value.contains(kButton)) {
            result.error = "Mouse button is not valid for this event"; return result;
        }
        if (result.eventType == "wheel" || result.eventType == "hwheel") {
            if ((!result.value.contains("deltaX") && !result.value.contains("deltaY")) ||
                (result.value.contains("deltaX") && (!IsFiniteNumber(result.value["deltaX"]) ||
                 std::abs(result.value["deltaX"].get<double>()) > kMaxWheelDelta)) ||
                (result.value.contains("deltaY") && (!IsFiniteNumber(result.value["deltaY"]) ||
                 std::abs(result.value["deltaY"].get<double>()) > kMaxWheelDelta))) {
                result.error = "Invalid wheel delta"; return result;
            }
        }
    } else if (result.eventType == "input_reset") {
        if (!HasOnly(result.value, common, result.error)) return result;
        if (result.value.contains("reason") && (!result.value["reason"].is_string() ||
            result.value["reason"].get_ref<const std::string&>().size() > 128)) {
            result.error = "Invalid reset reason"; return result;
        }
    } else if (result.eventType == "stream_config") {
        const std::unordered_set<std::string> allowed = {kType, "width", "height", "fps", "bitrate", "sessionId"};
        if (!HasOnly(result.value, allowed, result.error)) return result;
        for (const char* field : {"width", "height", "fps"}) {
            if (!result.value.contains(field) || !result.value[field].is_number_integer()) {
                result.error = std::string("Invalid stream profile field: ") + field; return result;
            }
        }
        if (result.value.contains("bitrate") && !result.value["bitrate"].is_number_integer()) {
            result.error = "Invalid stream profile bitrate"; return result;
        }
    } else {
        result.error = "Unknown input event type"; return result;
    }
    result.valid = true;
    return result;
}

bool IsReleaseEvent(const std::string& eventType) noexcept {
    return eventType == kKeyUp || eventType == kMouseUp || eventType == "input_reset";
}
bool IsMouseMove(const std::string& eventType) noexcept { return eventType == kMouseMove; }
} // namespace InputSchema
