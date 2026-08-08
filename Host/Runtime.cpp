#include "pch.h"
#include "Runtime.h"

#include <algorithm>
#include <iostream>
#include <thread>
#include <utility>

#include "AppInit.h"
#include "CaptureHelpers.h"
#include "IdGenerator.h"
#include "InputConfig.h"
#include "InputIntegrationLayer.h"
#include "KeyInputHandler.h"
#include "MatchmakerClient.h"
#include "MouseInputHandler.h"
#include "ShutdownManager.h"
#include "Websocket.h"
#include "WindowUtils.h"
#include "pion_webrtc.h"

namespace Runtime {

namespace {
constexpr wchar_t kInstanceMutexName[] = L"Local\\CloudGaming.DisplayCaptureProject.Host";
}

const char* ToString(HostState state) noexcept {
    switch (state) {
    case HostState::Stopped: return "Stopped";
    case HostState::Initializing: return "Initializing";
    case HostState::WaitingForTarget: return "WaitingForTarget";
    case HostState::Ready: return "Ready";
    case HostState::Streaming: return "Streaming";
    case HostState::Reconnecting: return "Reconnecting";
    case HostState::Stopping: return "Stopping";
    case HostState::Failed: return "Failed";
    }
    return "Unknown";
}

HostRuntime::HostRuntime() = default;

HostRuntime::~HostRuntime() {
    Stop();
}

void HostRuntime::SetState(HostState state, std::string failureReason) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (state_ != state) {
        std::cout << "[runtime] " << ToString(state_) << " -> " << ToString(state) << std::endl;
    }
    state_ = state;
    failureReason_ = std::move(failureReason);
}

bool HostRuntime::AcquireInstanceLock() {
    instanceMutex_ = CreateMutexW(nullptr, TRUE, kInstanceMutexName);
    if (!instanceMutex_) {
        SetState(HostState::Failed, "Could not create the host instance mutex");
        return false;
    }
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        CloseHandle(instanceMutex_);
        instanceMutex_ = nullptr;
        SetState(HostState::Failed, "Another host instance is already running");
        return false;
    }
    return true;
}

void HostRuntime::ReleaseInstanceLock() noexcept {
    if (instanceMutex_) {
        ReleaseMutex(instanceMutex_);
        CloseHandle(instanceMutex_);
        instanceMutex_ = nullptr;
    }
}

bool HostRuntime::LoadAndValidateConfiguration() {
    if (!ConfigUtils::LoadConfig(config_)) return false;
    if (!ConfigUtils::GetTargetProcessName(config_, targetProcessName_) || targetProcessName_.empty()) {
        SetState(HostState::Failed, "Missing host.targetProcessName in config.json");
        return false;
    }
    wideTargetProcessName_.assign(targetProcessName_.begin(), targetProcessName_.end());

    if (config_.contains("host") && config_["host"].contains("input")) {
        if (!InputConfig::loadFromJson(config_["host"]["input"])) {
            SetState(HostState::Failed, "Invalid host.input configuration");
            return false;
        }
    } else {
        InputConfig::resetToDefaults();
    }

    if (config_.contains("host") && config_["host"].contains("window")) {
        const auto& window = config_["host"]["window"];
        windowPollIntervalMs_ = std::max(50, window.value("pollIntervalMs", windowPollIntervalMs_));
        windowReattachGraceMs_ = std::max(0, window.value("reattachGraceMs", windowReattachGraceMs_));
        windowReattachEnabled_ = window.value("reattach", windowReattachEnabled_);
        const auto title = window.value("preferredTitleContains", std::string{});
        preferredWindowTitle_.assign(title.begin(), title.end());
    }

    if (!ConfigUtils::LoadNetworkEndpoints(endpoints_)) return false;
    matchmakerEnabled_ = !endpoints_.matchmakerUrl.empty();
    if (config_.contains("host") && config_["host"].contains("matchmaker")) {
        const auto& matchmaker = config_["host"]["matchmaker"];
        hostSecret_ = matchmaker.value("hostSecret", std::string{});
        heartbeatIntervalMs_ = std::max(1000, matchmaker.value("heartbeatIntervalMs", heartbeatIntervalMs_));
    }

    const int configuredFps = config_.contains("host") && config_["host"].contains("video")
        ? config_["host"]["video"].value("fps", 60) : 60;
    ConfigUtils::ApplyVideoSettings(config_);
    ConfigUtils::ApplyCaptureSettings(config_, configuredFps);
    ConfigUtils::ApplyAudioSettings(config_);
    ConfigUtils::ApplyThreadPrioritySettings(config_);
    ConfigUtils::ApplyAdaptiveQualityControlSettings(config_);
    return true;
}

bool HostRuntime::StartCoreServices() {
    AppInit::InitializeRtcBindings();
    rtcStarted_ = true;

    initKeyInputHandler();
    initMouseInputHandler();
    inputHandlersStarted_ = true;
    if (!InputIntegrationLayer::initialize() || !InputIntegrationLayer::start()) {
        SetState(HostState::Failed, "Failed to start the input integration layer");
        return false;
    }
    inputIntegrationStarted_ = true;

    initWebsocket(roomId_, endpoints_.signalingUrl);
    websocketStarted_ = true;

    if (matchmakerEnabled_) {
        if (!MatchmakerClient::initialize(endpoints_.matchmakerUrl, hostSecret_)) {
            SetState(HostState::Failed, "Failed to initialize the matchmaker client");
            return false;
        }
        MatchmakerClient::sendHeartbeat(hostId_, roomId_);
        MatchmakerClient::startHeartbeatThread(hostId_, roomId_, heartbeatIntervalMs_);
        matchmakerStarted_ = true;
    }
    return true;
}

bool HostRuntime::Start() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (state_ != HostState::Stopped && state_ != HostState::Failed) return true;
    }

    stopRequested_.store(false, std::memory_order_release);
    ShutdownManager::SetShutdown(false);
    SetState(HostState::Initializing);

    if (!AcquireInstanceLock()) return false;
    try {
        if (!LoadAndValidateConfiguration()) {
            if (GetStatus().state != HostState::Failed) SetState(HostState::Failed, "Configuration failed");
            Stop();
            return false;
        }

        roomId_ = generateRoomId();
        hostId_ = generateHostId();
        PrintBanner(roomId_);
        if (!StartCoreServices()) {
            Stop();
            return false;
        }

        nextTargetPoll_ = std::chrono::steady_clock::now();
        SetState(HostState::WaitingForTarget);
        return true;
    } catch (const std::exception& ex) {
        SetState(HostState::Failed, ex.what());
    } catch (...) {
        SetState(HostState::Failed, "Unexpected exception during host startup");
    }
    Stop();
    return false;
}

bool HostRuntime::TryAttachTarget() {
    HWND window = nullptr;
    DWORD pid = 0;
    if (!WindowUtils::PickWindowByProcessName(
            wideTargetProcessName_, window, pid, preferredWindowTitle_, false) || !window) {
        return false;
    }

    WindowUtils::MaybeResizeClientArea(window, config_);
    if (!d3dInitialized_) {
        if (!GraphicsAndCapture::InitializeDevice(d3d_, window)) {
            SetState(HostState::Failed, "Failed to initialize D3D11");
            return false;
        }
        d3dInitialized_ = true;
    }

    auto item = WindowUtils::CreateItem(window);
    GraphicsAndCapture::CaptureContext replacement;
    if (!item || !GraphicsAndCapture::InitializeCapture(replacement, d3d_, item)) {
        return false;
    }

    capture_ = std::move(replacement);
    targetWindow_ = window;
    targetPid_ = pid;
    WindowUtils::SetTargetWindow(targetWindow_);
    StartCapture();
    GraphicsAndCapture::Start(capture_);
    captureStarted_ = true;

    audioStarted_ = audio_.StartCapture(targetPid_, targetProcessName_);
    if (!audioStarted_) {
        std::cerr << "[runtime] Process audio is unavailable; video remains ready" << std::endl;
    }
    invalidWindowSince_ = {};
    SetState(HostState::Ready);
    return true;
}

void HostRuntime::DetachTarget() noexcept {
    if (audioStarted_) {
        try { audio_.StopCapture(); } catch (...) {}
        audioStarted_ = false;
    }
    if (captureStarted_) {
        try { GraphicsAndCapture::Stop(capture_); } catch (...) {}
        captureStarted_ = false;
        capture_ = {};
    }
    WindowUtils::SetTargetWindow(nullptr);
    targetWindow_ = nullptr;
    targetPid_ = 0;
}

void HostRuntime::Tick() {
    const auto now = std::chrono::steady_clock::now();
    if (!captureStarted_) {
        if (now >= nextTargetPoll_) {
            TryAttachTarget();
            nextTargetPoll_ = now + std::chrono::milliseconds(windowPollIntervalMs_);
        }
        return;
    }

    if (!targetWindow_ || !IsWindow(targetWindow_)) {
        if (invalidWindowSince_ == std::chrono::steady_clock::time_point{}) {
            invalidWindowSince_ = now;
        }
        if (windowReattachEnabled_ &&
            now - invalidWindowSince_ >= std::chrono::milliseconds(windowReattachGraceMs_)) {
            DetachTarget();
            SetState(HostState::WaitingForTarget);
            nextTargetPoll_ = now;
        }
        return;
    }
    invalidWindowSince_ = {};

    const int peerState = getPeerConnectionState();
    if (peerState != lastPeerState_) {
        lastPeerState_ = peerState;
        if (peerState == 2 || peerState == 3) {
            SetState(HostState::Streaming);
        } else if (peerState == 1) {
            SetState(HostState::Reconnecting);
        } else if (peerState == 4 || peerState == 5 || peerState == 6) {
            SetState(HostState::Ready);
        }
    }
}

void HostRuntime::Run() {
    while (!IsStopRequested()) {
        Tick();
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    Stop();
}

void HostRuntime::RequestStop() noexcept {
    stopRequested_.store(true, std::memory_order_release);
    ShutdownManager::SetShutdown(true);
}

bool HostRuntime::IsStopRequested() const noexcept {
    return stopRequested_.load(std::memory_order_acquire) || ShutdownManager::IsShutdown();
}

void HostRuntime::Stop() noexcept {
    HostState prior;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        prior = state_;
        if (prior == HostState::Stopped || prior == HostState::Stopping) return;
    }
    SetState(HostState::Stopping);
    stopRequested_.store(true, std::memory_order_release);
    ShutdownManager::SetShutdown(true);

    // Exact reverse order of acquisition: target audio/capture, matchmaker,
    // signaling/peer, input integration/handlers, Go runtime, instance mutex.
    DetachTarget();
    if (matchmakerStarted_) {
        try { MatchmakerClient::stopHeartbeatThread(); } catch (...) {}
        matchmakerStarted_ = false;
    }
    if (websocketStarted_) {
        try { stopWebsocket(); } catch (...) {}
        websocketStarted_ = false;
    }
    if (inputIntegrationStarted_) {
        try { InputIntegrationLayer::stop(); } catch (...) {}
        inputIntegrationStarted_ = false;
    }
    if (inputHandlersStarted_) {
        try { stopMouseInputHandler(); } catch (...) {}
        try { stopKeyInputHandler(); } catch (...) {}
        inputHandlersStarted_ = false;
    }
    if (rtcStarted_) {
        try { closePeerConnection(); } catch (...) {}
        try { closeGo(); } catch (...) {}
        rtcStarted_ = false;
    }
    ReleaseInstanceLock();
    SetState(HostState::Stopped);
}

bool HostRuntime::Restart() {
    Stop();
    return Start();
}

HostStatus HostRuntime::GetStatus() const {
    std::lock_guard<std::mutex> lock(mutex_);
    HostStatus status;
    status.state = state_;
    status.failureReason = failureReason_;
    status.hostId = hostId_;
    status.roomId = roomId_;
    status.targetProcessName = targetProcessName_;
    status.targetPid = targetPid_;
    status.targetWindow = targetWindow_;
    status.peerConnectionState = lastPeerState_;
    return status;
}

void PrintBanner(const std::string& roomId) {
    std::cout << "\n----------------------------------------\n"
              << "  Cloud Gaming Host Initialized\n"
              << "  Pairing room: " << roomId << "\n"
              << "----------------------------------------\n\n";
}

} // namespace Runtime
