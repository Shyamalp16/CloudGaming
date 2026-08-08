#pragma once

#include <Windows.h>
#include <atomic>
#include <chrono>
#include <mutex>
#include <string>

#include <nlohmann/json.hpp>

#include "AudioCapturer.h"
#include "ConfigUtils.h"
#include "GraphicsAndCapture.h"

namespace Runtime {

enum class HostState {
    Stopped,
    Initializing,
    WaitingForTarget,
    Ready,
    Streaming,
    Reconnecting,
    Stopping,
    Failed
};

const char* ToString(HostState state) noexcept;

struct HostStatus {
    HostState state = HostState::Stopped;
    std::string failureReason;
    std::string hostId;
    std::string roomId;
    std::string targetProcessName;
    DWORD targetPid = 0;
    HWND targetWindow = nullptr;
    int peerConnectionState = 0;
};

// The single owner of the native host core. The future tray application should
// call Start/Stop/Restart and GetStatus rather than directly owning subsystems.
class HostRuntime final {
public:
    HostRuntime();
    ~HostRuntime();

    HostRuntime(const HostRuntime&) = delete;
    HostRuntime& operator=(const HostRuntime&) = delete;

    bool Start();
    void Run();
    void RequestStop() noexcept;
    void Stop() noexcept;
    bool Restart();

    HostStatus GetStatus() const;
    bool IsStopRequested() const noexcept;

private:
    bool AcquireInstanceLock();
    bool LoadAndValidateConfiguration();
    bool StartCoreServices();
    bool TryAttachTarget();
    void DetachTarget() noexcept;
    void Tick();
    void SetState(HostState state, std::string failureReason = {});
    void ReleaseInstanceLock() noexcept;

    mutable std::mutex mutex_;
    std::atomic<bool> stopRequested_{false};
    HostState state_ = HostState::Stopped;
    std::string failureReason_;

    HANDLE instanceMutex_ = nullptr;
    bool rtcStarted_ = false;
    bool inputHandlersStarted_ = false;
    bool inputIntegrationStarted_ = false;
    bool websocketStarted_ = false;
    bool matchmakerStarted_ = false;
    bool d3dInitialized_ = false;
    bool captureStarted_ = false;
    bool audioStarted_ = false;

    nlohmann::json config_;
    ConfigUtils::NetworkEndpoints endpoints_;
    std::string hostId_;
    std::string roomId_;
    std::string targetProcessName_;
    std::wstring wideTargetProcessName_;
    std::wstring preferredWindowTitle_;
    std::string hostSecret_;
    int heartbeatIntervalMs_ = 25000;
    int windowPollIntervalMs_ = 500;
    int windowReattachGraceMs_ = 1000;
    bool windowReattachEnabled_ = true;
    bool matchmakerEnabled_ = false;

    HWND targetWindow_ = nullptr;
    DWORD targetPid_ = 0;
    int lastPeerState_ = -1;
    std::chrono::steady_clock::time_point nextTargetPoll_{};
    std::chrono::steady_clock::time_point invalidWindowSince_{};

    GraphicsAndCapture::D3DContext d3d_;
    GraphicsAndCapture::CaptureContext capture_;
    AudioCapturer audio_;
};

void PrintBanner(const std::string& roomId);

} // namespace Runtime
