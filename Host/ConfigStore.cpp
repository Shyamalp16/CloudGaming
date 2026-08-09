#include "ConfigStore.h"

#include <Windows.h>

#include <fstream>
#include <vector>

#include "AppPaths.h"
#include "WindowsSecurity.h"

namespace ConfigStore {
namespace {
void MergeMissing(nlohmann::json& target, const nlohmann::json& defaults) {
    if (!target.is_object() || !defaults.is_object()) return;
    for (auto it = defaults.begin(); it != defaults.end(); ++it) {
        if (!target.contains(it.key())) target[it.key()] = it.value();
        else if (target[it.key()].is_object() && it.value().is_object()) MergeMissing(target[it.key()], it.value());
    }
}

bool ReadJson(const std::filesystem::path& path, nlohmann::json& value, std::string& error) {
    try {
        std::ifstream stream(path, std::ios::binary);
        if (!stream) { error = "Cannot open " + path.string(); return false; }
        stream >> value;
        if (!value.is_object()) { error = "Configuration root must be an object"; return false; }
        return true;
    } catch (const std::exception& ex) {
        error = "Cannot parse " + path.string() + ": " + ex.what();
        return false;
    }
}

std::vector<std::filesystem::path> TemplateCandidates() {
    std::vector<std::filesystem::path> paths;
    wchar_t explicitPath[32768]{};
    const DWORD length = GetEnvironmentVariableW(L"CLOUDGAMING_CONFIG_PATH", explicitPath, 32768);
    if (length > 0 && length < 32768) paths.emplace_back(explicitPath);
    paths.push_back(std::filesystem::current_path() / L"config.json");
    const auto executable = AppPaths::ExecutableDirectory();
    paths.push_back(executable / L"config.json");
    paths.push_back(executable / L".." / L"config.json");
    paths.push_back(executable / L".." / L".." / L"config.json");
    return paths;
}
}

std::filesystem::path Path() { return AppPaths::UserConfigPath(); }

bool ValidateAndMigrate(nlohmann::json& config, bool& migrated, std::string& error) {
    migrated = false;
    if (!config.is_object()) { error = "Configuration root must be an object"; return false; }
    int version = config.value("schemaVersion", 0);
    if (version > kCurrentSchemaVersion) {
        error = "Configuration was created by a newer host version";
        return false;
    }
    if (version == 0) {
        config["schemaVersion"] = 1;
        migrated = true;
        version = 1;
    }
    if (version != kCurrentSchemaVersion || !config.contains("host") || !config["host"].is_object()) {
        error = "Configuration is missing a valid host object";
        return false;
    }
    auto& host = config["host"];
    if (!host.contains("targetProcessName") || !host["targetProcessName"].is_string()) {
        error = "host.targetProcessName must be a string";
        return false;
    }
    if (!host.contains("video") || !host["video"].is_object()) {
        error = "host.video must be an object";
        return false;
    }
    if (!config.contains("network")) {
        config["network"] = {{"mode", "local"}, {"ports", {{"matchmaker", 3000}, {"signaling", 3002}}},
            {"production", {{"matchmakerUrl", ""}, {"signalingUrl", ""}}}};
        migrated = true;
    }
    if (!config["network"].is_object()) { error = "network must be an object"; return false; }
    const auto mode = config["network"].value("mode", std::string{"local"});
    if (mode != "local" && mode != "production") { error = "network.mode must be local or production"; return false; }
    if (mode == "production") {
        const auto production = config["network"].value("production", nlohmann::json::object());
        const auto signaling = production.value("signalingUrl", std::string{});
        const auto matchmaker = production.value("matchmakerUrl", std::string{});
        if (signaling.rfind("wss://", 0) != 0 || matchmaker.rfind("https://", 0) != 0) {
            error = "production endpoints require wss:// signaling and https:// matchmaking";
            return false;
        }
    }
    if (config.contains("update") && !config["update"].is_object()) {
        error = "update must be an object";
        return false;
    }
    return true;
}

LoadResult Load(nlohmann::json& config) {
    LoadResult result;
    std::string error;
    const auto userPath = Path();
    std::error_code existsError;
    if (std::filesystem::exists(userPath, existsError)) {
        if (!ReadJson(userPath, config, result.error)) return result;
        result.sourcePath = userPath;
    } else {
        result.firstRun = true;
        bool found = false;
        for (const auto& candidate : TemplateCandidates()) {
            if (std::filesystem::exists(candidate, existsError) && ReadJson(candidate, config, error)) {
                result.sourcePath = std::filesystem::absolute(candidate).lexically_normal();
                found = true;
                break;
            }
        }
        if (!found) { result.error = error.empty() ? "No configuration template was found" : error; return result; }
    }

    nlohmann::json defaults;
    for (const auto& candidate : TemplateCandidates()) {
        if (std::filesystem::exists(candidate, existsError) && ReadJson(candidate, defaults, error)) break;
    }
    if (defaults.is_object() && result.sourcePath == userPath) MergeMissing(config, defaults);

    bool migrated = false;
    if (!ValidateAndMigrate(config, migrated, result.error)) return result;
    result.migrated = migrated;
    result.success = true;
    return result;
}

bool Save(const nlohmann::json& config, std::string& error) {
    nlohmann::json validated = config;
    bool migrated = false;
    if (!ValidateAndMigrate(validated, migrated, error)) return false;
    try {
        const auto target = Path();
        std::filesystem::create_directories(target.parent_path());
        const auto temporary = target.wstring() + L".tmp";
        {
            std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
            if (!stream) { error = "Cannot create temporary configuration"; return false; }
            stream << validated.dump(2) << '\n';
            stream.flush();
            if (!stream) { error = "Cannot flush temporary configuration"; return false; }
        }
        if (!MoveFileExW(temporary.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            error = "Atomic configuration replacement failed: " + std::to_string(GetLastError());
            DeleteFileW(temporary.c_str());
            return false;
        }
        std::string aclError;
        WindowsSecurity::ProtectForCurrentUserAndSystem(target.parent_path(), aclError);
        if (!WindowsSecurity::ProtectForCurrentUserAndSystem(target, aclError)) { error = aclError; return false; }
        return true;
    } catch (const std::exception& ex) {
        error = ex.what();
        return false;
    }
}

bool EnsureUserConfig(nlohmann::json& config, std::string& error) {
    auto loaded = Load(config);
    if (!loaded.success) { error = loaded.error; return false; }
    if (loaded.firstRun || loaded.migrated || loaded.sourcePath != Path()) return Save(config, error);
    return true;
}
}
