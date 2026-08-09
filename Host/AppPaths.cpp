#include "AppPaths.h"

#include <Windows.h>

namespace AppPaths {
std::filesystem::path ExecutableDirectory() {
    std::wstring path(32768, L'\0');
    const DWORD length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    if (length == 0 || length >= path.size()) return std::filesystem::current_path();
    path.resize(length);
    return std::filesystem::path(path).parent_path();
}

std::filesystem::path UserDataDirectory() {
    std::wstring value(32768, L'\0');
    const DWORD length = GetEnvironmentVariableW(L"LOCALAPPDATA", value.data(), static_cast<DWORD>(value.size()));
    if (length > 0 && length < value.size()) {
        value.resize(length);
        return std::filesystem::path(value) / L"CloudGamingHost";
    }
    return std::filesystem::temp_directory_path() / L"CloudGamingHost";
}

std::filesystem::path UserConfigPath() { return UserDataDirectory() / L"config.json"; }
std::filesystem::path UserSecretsPath() { return UserDataDirectory() / L"secrets.dat"; }
}
