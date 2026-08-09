#pragma once

#include <string>

#include <nlohmann/json.hpp>

namespace UpdateManager {
enum class Status { Disabled, UpToDate, Available, Error };

struct Result {
    Status status = Status::Error;
    std::string message;
    std::string version;
    std::string downloadUrl;
    std::string sha256;
};

Result Check(const nlohmann::json& config);
bool DownloadVerifyAndLaunch(const Result& update, std::string& error);
}
