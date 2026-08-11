#include "GameInventory.h"

#include <Windows.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cwctype>
#include <fstream>
#include <iomanip>
#include <mutex>
#include <regex>
#include <sstream>
#include <unordered_set>

#include "AppPaths.h"
#include "IdGenerator.h"
#include "WindowsSecurity.h"

namespace GameInventory {
namespace {
std::mutex mutex;

std::string ReadFile(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary | std::ios::ate);
    if (!stream || stream.tellg() < 0 || stream.tellg() > 16 * 1024 * 1024) return {};
    std::string value(static_cast<size_t>(stream.tellg()), '\0');
    stream.seekg(0);
    stream.read(value.data(), value.size());
    return stream ? value : std::string{};
}

std::string VdfValue(const std::string& text, const char* key) {
    std::smatch match;
    const std::regex pattern("\\\"" + std::string(key) + "\\\"\\s*\\\"([^\\\"]*)\\\"",
                             std::regex::icase);
    return std::regex_search(text, match, pattern) ? match[1].str() : std::string{};
}

std::filesystem::path SteamRoot() {
    wchar_t value[32768]{};
    DWORD bytes = sizeof(value);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Valve\\Steam", L"SteamPath",
                     RRF_RT_REG_SZ, nullptr, value, &bytes) == ERROR_SUCCESS) return value;
    return {};
}

std::string UnescapePath(std::string value) {
    for (size_t at = 0; (at = value.find("\\\\", at)) != std::string::npos; ++at)
        value.replace(at, 2, "\\");
    return value;
}

std::string ManualGameId(const std::string& title) {
    std::string slug;
    std::uint64_t hash = 1469598103934665603ull;
    for (unsigned char byte : title) {
        const auto lowered = static_cast<unsigned char>(std::tolower(byte));
        hash = (hash ^ lowered) * 1099511628211ull;
        if (std::isalnum(lowered)) slug.push_back(static_cast<char>(lowered));
        else if (!slug.empty() && slug.back() != '-') slug.push_back('-');
    }
    while (!slug.empty() && slug.back() == '-') slug.pop_back();
    if (slug.empty()) slug = "game";
    if (slug.size() > 40) slug.resize(40);
    std::ostringstream suffix;
    suffix << std::hex << std::setfill('0') << std::setw(16) << hash;
    return "manual:" + slug + "-" + suffix.str();
}

std::vector<Game> ScanSteam() {
    const auto root = SteamRoot();
    if (root.empty()) return {};
    std::vector<std::filesystem::path> libraries{root};
    const auto folders = ReadFile(root / L"steamapps" / L"libraryfolders.vdf");
    const std::regex pathPattern(R"vdf("path"\s*"([^"]+)")vdf", std::regex::icase);
    for (std::sregex_iterator it(folders.begin(), folders.end(), pathPattern), end; it != end; ++it)
        libraries.emplace_back(UnescapePath((*it)[1].str()));

    std::vector<Game> games;
    std::unordered_set<std::string> seen;
    for (const auto& library : libraries) {
        std::error_code error;
        const auto steamapps = library / L"steamapps";
        for (std::filesystem::directory_iterator it(steamapps, error), end; !error && it != end; it.increment(error)) {
            if (!it->is_regular_file() || it->path().filename().wstring().rfind(L"appmanifest_", 0) != 0) continue;
            const auto manifest = ReadFile(it->path());
            const auto appId = VdfValue(manifest, "appid");
            const auto title = VdfValue(manifest, "name");
            if (appId.empty() || title.empty() || !seen.insert(appId).second) continue;
            games.push_back({"steam:" + appId, "steam", title, "steam-" + appId, appId, false, true});
        }
    }
    return games;
}

nlohmann::json LoadState() {
    const auto raw = ReadFile(AppPaths::GameInventoryPath());
    if (raw.empty()) return {{"enabled", nlohmann::json::array()}, {"manual", nlohmann::json::array()}};
    auto state = nlohmann::json::parse(raw, nullptr, false);
    return state.is_object() ? state : nlohmann::json::object();
}

bool SaveState(const nlohmann::json& state, std::string& error) {
    try {
        const auto path = AppPaths::GameInventoryPath();
        std::filesystem::create_directories(path.parent_path());
        std::string aclError;
        if (!WindowsSecurity::ProtectForCurrentUserAndSystem(path.parent_path(), aclError)) {
            error = aclError; return false;
        }
        const auto temporary = path.wstring() + L".tmp";
        std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
        stream << state.dump(2) << '\n';
        stream.close();
        if (!stream || !MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            error = "Could not save game inventory"; DeleteFileW(temporary.c_str()); return false;
        }
        return WindowsSecurity::ProtectForCurrentUserAndSystem(path, error);
    } catch (const std::exception& ex) { error = ex.what(); return false; }
}

std::vector<Game> ListUnlocked(nlohmann::json& state) {
    auto games = ScanSteam();
    const auto enabled = state.value("enabled", std::vector<std::string>{});
    for (const auto& item : state.value("manual", nlohmann::json::array())) {
        if (!item.is_object()) continue;
        const auto path = std::filesystem::path(item.value("launchTarget", std::string{}));
        games.push_back({item.value("id", std::string{}), "manual", item.value("title", std::string{}),
                         item.value("localManifestId", std::string{}), path, false,
                         std::filesystem::is_regular_file(path)});
    }
    for (auto& game : games)
        game.enabled = std::find(enabled.begin(), enabled.end(), game.id) != enabled.end();
    return games;
}
}

std::vector<Game> List(std::string& error) {
    std::lock_guard lock(mutex);
    auto state = LoadState();
    auto games = ListUnlocked(state);
    if (games.empty() && SteamRoot().empty()) error = "Steam was not found; add a manual game instead";
    return games;
}

std::optional<Game> Find(const std::string& localManifestId, std::string& error) {
    for (const auto& game : List(error))
        if (game.localManifestId == localManifestId && game.enabled && game.installed) return game;
    if (error.empty()) error = "Game offering is disabled or unavailable";
    return std::nullopt;
}

bool SetEnabled(const std::string& id, bool enabled, std::string& error) {
    std::lock_guard lock(mutex);
    auto state = LoadState();
    auto games = ListUnlocked(state);
    const auto game = std::find_if(games.begin(), games.end(), [&](const Game& item) { return item.id == id; });
    if (game == games.end() || !game->installed) { error = "Game is not installed"; return false; }
    auto values = state.value("enabled", std::vector<std::string>{});
    values.erase(std::remove(values.begin(), values.end(), id), values.end());
    if (enabled) values.push_back(id);
    state["enabled"] = values;
    return SaveState(state, error);
}

bool AddManual(const std::string& title, const std::filesystem::path& executable,
               Game& result, std::string& error) {
    auto extension = executable.extension().wstring();
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   [](wchar_t value) { return static_cast<wchar_t>(std::towlower(value)); });
    if (title.empty() || title.size() > 160 || !executable.is_absolute() ||
        extension != L".exe" || !std::filesystem::is_regular_file(executable)) {
        error = "Manual games require a title and an existing absolute .exe path"; return false;
    }
    std::lock_guard lock(mutex);
    auto state = LoadState();
    const auto id = ManualGameId(title);
    for (const auto& item : state.value("manual", nlohmann::json::array())) {
        if (item.value("id", std::string{}) == id) {
            error = "A manual game with this title is already configured";
            return false;
        }
    }
    const auto suffix = generateRoomId().substr(0, 16);
    result = {id, "manual", title, "manual-" + suffix,
              std::filesystem::weakly_canonical(executable), false, true};
    auto manual = state.value("manual", nlohmann::json::array());
    manual.push_back({{"id", result.id}, {"title", result.title},
                      {"localManifestId", result.localManifestId},
                      {"launchTarget", result.launchTarget.string()}});
    state["manual"] = std::move(manual);
    return SaveState(state, error);
}

nlohmann::json PublicJson(const Game& game) {
    return {{"id", game.id}, {"source", game.source}, {"title", game.title},
            {"localManifestId", game.localManifestId}, {"enabled", game.enabled},
            {"installed", game.installed}};
}
}
