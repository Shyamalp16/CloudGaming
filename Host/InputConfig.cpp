#include "InputConfig.h"
#include "Environment.h"

#include <algorithm>
#include <iostream>
#include <sstream>

namespace InputConfig {

InputConfiguration globalInputConfig;

namespace {
int parseWheelScale() {
    constexpr int defaultScale = 120;
    const auto value = Environment::read("INPUT_WHEEL_SCALE");
    if (!value) return defaultScale;
    try {
        return std::max(1, std::stoi(*value));
    } catch (...) {
        return defaultScale;
    }
}
} // namespace

bool loadFromJson(const nlohmann::json& jsonConfig) {
    try {
        globalInputConfig.releaseAllOnDisconnect =
            jsonConfig.value("releaseAllOnDisconnect", globalInputConfig.releaseAllOnDisconnect);
        globalInputConfig.stuckKeyTimeout = std::chrono::milliseconds(
            jsonConfig.value("stuckKeyTimeoutMs", static_cast<int>(globalInputConfig.stuckKeyTimeout.count())));
        globalInputConfig.enableStuckKeyRecovery =
            jsonConfig.value("enableStuckKeyRecovery", globalInputConfig.enableStuckKeyRecovery);
        globalInputConfig.enableMouseSequencing =
            jsonConfig.value("enableMouseSequencing", globalInputConfig.enableMouseSequencing);
        globalInputConfig.enablePerEventLogging =
            jsonConfig.value("enablePerEventLogging", globalInputConfig.enablePerEventLogging);
        globalInputConfig.enableAggregatedLogging =
            jsonConfig.value("enableAggregatedLogging", globalInputConfig.enableAggregatedLogging);
        globalInputConfig.maxPendingMessages =
            jsonConfig.value("maxPendingMessages", globalInputConfig.maxPendingMessages);
        return validateConfiguration();
    } catch (const std::exception& e) {
        std::cerr << "[InputConfig] Error loading configuration: " << e.what() << std::endl;
        return false;
    }
}

bool validateConfiguration() {
    if (globalInputConfig.stuckKeyTimeout.count() <= 0) {
        std::cerr << "[InputConfig] stuckKeyTimeoutMs must be positive" << std::endl;
        return false;
    }
    if (globalInputConfig.maxPendingMessages < 1 || globalInputConfig.maxPendingMessages > 10000) {
        std::cerr << "[InputConfig] maxPendingMessages must be between 1 and 10000" << std::endl;
        return false;
    }
    return true;
}

void resetToDefaults() {
    globalInputConfig = InputConfiguration{};
}

std::string getConfigurationSummary() {
    std::ostringstream ss;
    ss << "Input: queue=" << globalInputConfig.maxPendingMessages
       << ", stuck-key recovery=" << (globalInputConfig.enableStuckKeyRecovery ? "on" : "off")
       << ", mouse sequencing=" << (globalInputConfig.enableMouseSequencing ? "on" : "off")
       << ", event logging=" << (globalInputConfig.enablePerEventLogging ? "on" : "off");
    return ss.str();
}

int getWheelScale() {
    static const int wheelScale = parseWheelScale();
    return wheelScale;
}

} // namespace InputConfig
