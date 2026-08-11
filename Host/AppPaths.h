#pragma once

#include <filesystem>

namespace AppPaths {
std::filesystem::path ExecutableDirectory();
std::filesystem::path UserDataDirectory();
std::filesystem::path UserConfigPath();
std::filesystem::path UserSecretsPath();
std::filesystem::path GameInventoryPath();
}
