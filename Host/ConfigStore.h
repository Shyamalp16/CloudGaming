#pragma once

#include <filesystem>
#include <string>

#include <nlohmann/json.hpp>

namespace ConfigStore {
constexpr int kCurrentSchemaVersion = 1;

struct LoadResult {
    bool success = false;
    bool firstRun = false;
    bool migrated = false;
    std::filesystem::path sourcePath;
    std::string error;
};

LoadResult Load(nlohmann::json& config);
bool Save(const nlohmann::json& config, std::string& error);
bool EnsureUserConfig(nlohmann::json& config, std::string& error);
bool ValidateAndMigrate(nlohmann::json& config, bool& migrated, std::string& error);
std::filesystem::path Path();
}
