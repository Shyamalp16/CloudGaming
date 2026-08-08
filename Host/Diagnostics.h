#pragma once

#include <cstdint>
#include <filesystem>
#include <mutex>
#include <string>

#include <nlohmann/json.hpp>

namespace Diagnostics {

struct Status {
    bool initialized = false;
    std::filesystem::path logDirectory;
    std::filesystem::path activeLog;
    std::uint64_t recordsWritten = 0;
    std::uint64_t writeFailures = 0;
};

bool Initialize();
void Shutdown() noexcept;
void InstallCrashHandler();
void Log(const std::string& severity, const std::string& category,
         const std::string& message, const std::string& details = {});
std::string Redact(std::string value);
Status GetStatus();

bool CreateSupportBundle(const nlohmann::json& health, const nlohmann::json& config,
                         std::filesystem::path& outputDirectory, std::string& error);

} // namespace Diagnostics
