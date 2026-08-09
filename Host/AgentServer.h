#pragma once

#include <Windows.h>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>

#include <nlohmann/json.hpp>

#include "HostController.h"

class AgentServer final {
public:
    static constexpr int kProtocolVersion = 1;

    explicit AgentServer(std::string pipeName);
    ~AgentServer();

    AgentServer(const AgentServer&) = delete;
    AgentServer& operator=(const AgentServer&) = delete;

    int Run();

private:
    HANDLE CreatePipe(std::string& error) const;
    void HandleConnection(HANDLE pipe);
    nlohmann::json Dispatch(const nlohmann::json& request);
    nlohmann::json Snapshot() const;
    void PublishStatus();
    void EventLoop();
    bool Send(HANDLE pipe, const nlohmann::json& message);

    std::string pipeName_;
    std::wstring pipePath_;
    HostController controller_;
    std::atomic<bool> shutdownRequested_{false};
    std::atomic<std::uint64_t> eventSequence_{0};
    mutable std::mutex connectionMutex_;
    std::mutex writeMutex_;
    std::mutex eventMutex_;
    std::condition_variable eventCondition_;
    std::thread eventThread_;
    bool statusDirty_ = false;
    HANDLE connection_ = INVALID_HANDLE_VALUE;
};
