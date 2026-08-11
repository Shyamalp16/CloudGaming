#pragma once

#include <Windows.h>
#include <atomic>
#include <chrono>
#include <deque>
#include <mutex>
#include <string>
#include <filesystem>
#include <functional>
#include <optional>

#include <nlohmann/json.hpp>

#include "AudioCapturer.h"
#include "ConfigUtils.h"
#include "GraphicsAndCapture.h"
#include "GameLauncher.h"
#include "MarketplaceControl.h"
#include "SessionManager.h"
#include "StreamProfileManager.h"

namespace Runtime {

enum class HostState {
    Stopped,
    Initializing,
    Idle,
    Preparing,
    WaitingForTarget,
    Ready,
    Streaming,
    Reconnecting,
    Cleaning,
    Stopping,
    Failed
};

const char* ToString(HostState state) noexcept;

struct HostStatus {
    HostState state = HostState::Stopped;
    std::string failureReason;
    std::string hostId;
    std::string roomId;
	std::string pairingCode;
    std::string targetProcessName;
    std::string sessionId;
    std::string gameId;
    DWORD targetPid = 0;
    HWND targetWindow = nullptr;
    int peerConnectionState = 0;
};

// The single owner of the native host core. The future tray application should
// call Start/Stop/Restart and GetStatus rather than directly owning subsystems.
class HostRuntime final {
public:
    using StatusCallback = std::function<void(const HostStatus&)>;
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
    nlohmann::json GetHealthSnapshot() const;
    bool CreateSupportBundle(std::filesystem::path& outputDirectory, std::string& error) const;
    bool QueueTargetSelection(std::string processName, std::wstring preferredTitle, std::string& error);
    void RequestPresenceRefresh() noexcept;
    void SetStatusCallback(StatusCallback callback);
    bool IsStopRequested() const noexcept;

private:
    bool AcquireInstanceLock();
    bool LoadAndValidateConfiguration();
    bool StartCoreServices();
    bool TryAttachTarget();
    bool AttachTarget(HWND window, DWORD pid, const std::string& processName);
    void DetachTarget() noexcept;
    void Tick();
    void ApplyPendingTargetSelection();
    void HandleMarketplaceCommand(const nlohmann::json& command);
    void ApplyMarketplaceCommand();
    void CleanupSession(const std::string& terminalEvent, const std::string& reason) noexcept;
    nlohmann::json MarketplacePresence() const;
    void NotifyStatus();
    void SetState(HostState state, std::string failureReason = {});
    void ReleaseInstanceLock() noexcept;

    mutable std::mutex mutex_;
    mutable std::mutex callbackMutex_;
    mutable std::mutex commandMutex_;
    std::atomic<bool> stopRequested_{false};
    std::atomic<bool> presenceRefreshRequested_{false};
    HostState state_ = HostState::Stopped;
    std::string failureReason_;

    HANDLE instanceMutex_ = nullptr;
    bool rtcStarted_ = false;
    bool inputHandlersStarted_ = false;
    bool inputIntegrationStarted_ = false;
    bool websocketStarted_ = false;
    bool matchmakerStarted_ = false;
    bool marketplaceStarted_ = false;
    bool d3dInitialized_ = false;
    bool captureStarted_ = false;
    bool audioStarted_ = false;

    nlohmann::json config_;
    ConfigUtils::NetworkEndpoints endpoints_;
    std::string hostId_;
    std::string roomId_;
	std::string pairingCode_;
    std::string targetProcessName_;
    std::string activeSessionId_;
    std::string activeGameId_;
    std::string activeCommandId_;
    std::wstring wideTargetProcessName_;
    std::wstring preferredWindowTitle_;
    std::string hostSecret_;
    int heartbeatIntervalMs_ = 25000;
    int windowPollIntervalMs_ = 500;
    int windowReattachGraceMs_ = 1000;
    bool windowReattachEnabled_ = true;
    bool matchmakerEnabled_ = false;

    std::atomic<HWND> targetWindow_{nullptr};
    std::atomic<DWORD> targetPid_{0};
    std::atomic<int> lastPeerState_{-1};
    struct PendingTarget { std::string processName; std::wstring preferredTitle; };
    std::optional<PendingTarget> pendingTarget_;
    std::deque<nlohmann::json> pendingMarketplaceCommands_;
    StatusCallback statusCallback_;
    std::chrono::steady_clock::time_point nextTargetPoll_{};
    std::chrono::steady_clock::time_point invalidWindowSince_{};
    std::chrono::steady_clock::time_point launchDeadline_{};
    std::chrono::steady_clock::time_point sessionDeadline_{};
    int sessionDurationSeconds_ = 0;
    bool sessionConnectedReported_ = false;

    GraphicsAndCapture::D3DContext d3d_;
    GraphicsAndCapture::CaptureContext capture_;
    AudioCapturer audio_;
    SessionManager session_;
    StreamProfileManager streamProfiles_;
    GameLauncher gameLauncher_;
    MarketplaceControl marketplace_;
};

void PrintBanner();

} // namespace Runtime
