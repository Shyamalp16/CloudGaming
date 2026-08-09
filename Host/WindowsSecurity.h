#pragma once

#include <filesystem>
#include <string>

namespace WindowsSecurity {
bool ProtectForCurrentUserAndSystem(const std::filesystem::path& path, std::string& error);
}
