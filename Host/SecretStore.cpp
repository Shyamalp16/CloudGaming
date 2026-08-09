#include "SecretStore.h"

#include <Windows.h>
#include <wincrypt.h>

#include <algorithm>
#include <fstream>
#include <vector>
#include <mutex>
#include <cctype>

#include <nlohmann/json.hpp>

#include "AppPaths.h"
#include "WindowsSecurity.h"

#pragma comment(lib, "Crypt32.lib")

namespace SecretStore {
namespace {
std::mutex g_storeMutex;
bool ValidName(const std::string& name) {
    return !name.empty() && name.size() <= 64 && std::all_of(name.begin(), name.end(), [](unsigned char c) {
        return std::isalnum(c) || c == '-' || c == '_';
    });
}

std::string Hex(const BYTE* data, DWORD size) {
    static constexpr char digits[] = "0123456789abcdef";
    std::string output(static_cast<size_t>(size) * 2, '0');
    for (DWORD i = 0; i < size; ++i) {
        output[i * 2] = digits[data[i] >> 4];
        output[i * 2 + 1] = digits[data[i] & 0x0f];
    }
    return output;
}

bool Unhex(const std::string& input, std::vector<BYTE>& output) {
    if (input.size() % 2 != 0) return false;
    output.resize(input.size() / 2);
    auto digit = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    for (size_t i = 0; i < output.size(); ++i) {
        const int high = digit(input[i * 2]);
        const int low = digit(input[i * 2 + 1]);
        if (high < 0 || low < 0) return false;
        output[i] = static_cast<BYTE>((high << 4) | low);
    }
    return true;
}

bool LoadFile(nlohmann::json& values, std::string& error) {
    const auto path = AppPaths::UserSecretsPath();
    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) { values = nlohmann::json::object(); return true; }
    try {
        std::ifstream stream(path, std::ios::binary);
        stream >> values;
        if (!values.is_object()) { error = "Secret store is corrupt"; return false; }
        return true;
    } catch (const std::exception& ex) { error = ex.what(); return false; }
}

bool SaveFile(const nlohmann::json& values, std::string& error) {
    try {
        const auto path = AppPaths::UserSecretsPath();
        std::filesystem::create_directories(path.parent_path());
        const auto temporary = path.wstring() + L".tmp";
        {
            std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
            stream << values.dump() << '\n';
            if (!stream) { error = "Could not write secret store"; return false; }
        }
        if (!MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            error = "Could not replace secret store: " + std::to_string(GetLastError());
            DeleteFileW(temporary.c_str());
            return false;
        }
        std::string aclError;
        WindowsSecurity::ProtectForCurrentUserAndSystem(path.parent_path(), aclError);
        if (!WindowsSecurity::ProtectForCurrentUserAndSystem(path, aclError)) { error = aclError; return false; }
        return true;
    } catch (const std::exception& ex) { error = ex.what(); return false; }
}
}

bool Set(const std::string& name, const std::string& value, std::string& error) {
    std::lock_guard<std::mutex> lock(g_storeMutex);
    if (!ValidName(name) || value.size() > 16384) { error = "Invalid secret name or value"; return false; }
    DATA_BLOB input{static_cast<DWORD>(value.size()), reinterpret_cast<BYTE*>(const_cast<char*>(value.data()))};
    DATA_BLOB output{};
    if (!CryptProtectData(&input, L"CloudGamingHost credential", nullptr, nullptr, nullptr,
                          CRYPTPROTECT_UI_FORBIDDEN, &output)) {
        error = "DPAPI encryption failed: " + std::to_string(GetLastError()); return false;
    }
    nlohmann::json values;
    const bool loaded = LoadFile(values, error);
    if (loaded) values[name] = Hex(output.pbData, output.cbData);
    SecureZeroMemory(output.pbData, output.cbData);
    LocalFree(output.pbData);
    return loaded && SaveFile(values, error);
}

std::optional<std::string> Get(const std::string& name, std::string& error) {
    std::lock_guard<std::mutex> lock(g_storeMutex);
    if (!ValidName(name)) { error = "Invalid secret name"; return std::nullopt; }
    nlohmann::json values;
    if (!LoadFile(values, error) || !values.contains(name) || !values[name].is_string()) return std::nullopt;
    std::vector<BYTE> encrypted;
    if (!Unhex(values[name].get<std::string>(), encrypted)) { error = "Secret encoding is corrupt"; return std::nullopt; }
    DATA_BLOB input{static_cast<DWORD>(encrypted.size()), encrypted.data()};
    DATA_BLOB output{};
    if (!CryptUnprotectData(&input, nullptr, nullptr, nullptr, nullptr,
                            CRYPTPROTECT_UI_FORBIDDEN, &output)) {
        error = "DPAPI decryption failed: " + std::to_string(GetLastError()); return std::nullopt;
    }
    std::string value(reinterpret_cast<char*>(output.pbData), output.cbData);
    SecureZeroMemory(output.pbData, output.cbData);
    LocalFree(output.pbData);
    return value;
}

bool Remove(const std::string& name, std::string& error) {
    std::lock_guard<std::mutex> lock(g_storeMutex);
    if (!ValidName(name)) { error = "Invalid secret name"; return false; }
    nlohmann::json values;
    if (!LoadFile(values, error)) return false;
    values.erase(name);
    return SaveFile(values, error);
}
}
