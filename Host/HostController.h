#pragma once

#include <functional>
#include <mutex>
#include <string>
#include <thread>

#include <nlohmann/json.hpp>

#include "Runtime.h"

class HostController final {
public:
    using StatusCallback = std::function<void()>;

    HostController();
    ~HostController();

    HostController(const HostController&) = delete;
    HostController& operator=(const HostController&) = delete;

    bool StartAsync();
    void StopAsync() noexcept;
    void WaitForStop() noexcept;
    bool SelectTarget(const std::string& processName, const std::wstring& preferredTitle, std::string& error);
    void RequestPresenceRefresh() noexcept;
    Runtime::HostStatus GetStatus() const;
    nlohmann::json GetHealthSnapshot() const;
    void SetStatusCallback(StatusCallback callback);

private:
    void Notify();

    mutable std::mutex mutex_;
    Runtime::HostRuntime runtime_;
    std::thread worker_;
    bool workerActive_ = false;
    StatusCallback callback_;
};
