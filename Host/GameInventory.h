#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace GameInventory {
struct Game {
    std::string id;
    std::string source;
    std::string title;
    std::string localManifestId;
    std::filesystem::path launchTarget;
    bool enabled = false;
    bool installed = false;
};

std::vector<Game> List(std::string& error);
std::optional<Game> Find(const std::string& localManifestId, std::string& error);
bool SetEnabled(const std::string& id, bool enabled, std::string& error);
bool AddManual(const std::string& title, const std::filesystem::path& executable,
               Game& result, std::string& error);
nlohmann::json PublicJson(const Game& game);
}
