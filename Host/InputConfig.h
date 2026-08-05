#pragma once

#include <chrono>
#include <string>
#include <nlohmann/json.hpp>

namespace InputConfig {

struct InputConfiguration {
    bool releaseAllOnDisconnect = true;
    std::chrono::milliseconds stuckKeyTimeout{2000};
    bool enableStuckKeyRecovery = false;
    bool enableMouseSequencing = false;
    bool enablePerEventLogging = false;
    bool enableAggregatedLogging = true;
    int maxPendingMessages = 100;
};

extern InputConfiguration globalInputConfig;

bool loadFromJson(const nlohmann::json& jsonConfig);
bool validateConfiguration();
void resetToDefaults();
std::string getConfigurationSummary();
int getWheelScale();

} // namespace InputConfig
