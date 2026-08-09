#include "pch.h"
#include "Runtime.h"

#include <algorithm>
#include <iostream>
#include <thread>
#include <utility>

#include "AppInit.h"
#include "CaptureHelpers.h"
#include "Diagnostics.h"
#include "Encoder.h"
#include "IdGenerator.h"
#include "InputConfig.h"
#include "InputIntegrationLayer.h"
#include "InputTransportLayer.h"
#include "KeyInputHandler.h"
#include "MatchmakerClient.h"
#include "MouseInputHandler.h"
#include "ShutdownManager.h"
#include "SecretStore.h"
#include "ConfigStore.h"
#include "RuntimeMetrics.h"
#include "Websocket.h"
#include "WindowUtils.h"
#include "pion_webrtc.h"

namespace Runtime {

namespace {
constexpr wchar_t kInstanceMutexName[] = L"Local\\CloudGaming.DisplayCaptureProject.Host";

std::string ReadEnvironment(const char* name) {
    const DWORD length = GetEnvironmentVariableA(name, nullptr, 0);
    if (length == 0) return {};
    std::string value(length, '\0');
    GetEnvironmentVariableA(name, value.data(), length);
    if (!value.empty() && value.back() == '\0') value.pop_back();
    return value;
}

std::string ReadCredential(const char* environmentName, const char* secretName) {
    auto value = ReadEnvironment(environmentName);
    if (!value.empty()) return value;
    std::string error;
    const auto stored = SecretStore::Get(secretName, error);
    if (!error.empty()) Diagnostics::Log("WARNING", "SECURITY", "Could not read protected credential", error);
    return stored.value_or(std::string{});
}

bool HasValidIceUrlList(const std::string& urls) {
    if (urls.empty()) return false;
    size_t start = 0;
    while (start < urls.size()) {
        const size_t end = urls.find(',', start);
        const auto entry = urls.substr(start, end == std::string::npos ? std::string::npos : end - start);
        if (entry.rfind("turn:", 0) != 0 && entry.rfind("turns:", 0) != 0 &&
            entry.rfind("stun:", 0) != 0 && entry.rfind("stuns:", 0) != 0) return false;
        if (end == std::string::npos) break;
        start = end + 1;
    }
    return true;
}

std::string WideToUtf8(const std::wstring& value) {
    if (value.empty()) return {};
    const int size = WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
                                         nullptr, 0, nullptr, nullptr);
    if (size <= 0) return {};
    std::string result(static_cast<size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
                        result.data(), size, nullptr, nullptr);
    return result;
}
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
    HostState previous;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        previous = state_;
        state_ = state;
        failureReason_ = std::move(failureReason);
    }
    if (previous != state) {
        std::cout << "[runtime] " << ToString(previous) << " -> " << ToString(state) << std::endl;
    }
    Diagnostics::Log(state == HostState::Failed ? "ERROR" : "INFO", "LIFECYCLE",
                     std::string("host state ") + ToString(state), GetStatus().failureReason);
    NotifyStatus();
}

void HostRuntime::SetStatusCallback(StatusCallback callback) {
    {
        std::lock_guard<std::mutex> lock(callbackMutex_);
        statusCallback_ = std::move(callback);
    }
    NotifyStatus();
}

void HostRuntime::NotifyStatus() {
    StatusCallback callback;
    {
        std::lock_guard<std::mutex> lock(callbackMutex_);
        callback = statusCallback_;
    }
    if (!callback) return;
    try { callback(GetStatus()); }
    catch (...) { Diagnostics::Log("WARNING", "UI", "Host status callback threw"); }
}

bool HostRuntime::QueueTargetSelection(std::string processName, std::wstring preferredTitle, std::string& error) {
    if (processName.empty() || processName.size() > 260 || preferredTitle.size() > 512 ||
        std::filesystem::path(processName).filename().string() != processName) {
        error = "Invalid target process selection";
        return false;
    }
    std::lock_guard<std::mutex> lock(commandMutex_);
    pendingTarget_ = PendingTarget{std::move(processName), std::move(preferredTitle)};
    return true;
}

void HostRuntime::ApplyPendingTargetSelection() {
    std::optional<PendingTarget> pending;
    {
        std::lock_guard<std::mutex> lock(commandMutex_);
        pending.swap(pendingTarget_);
    }
    if (!pending) return;
    DetachTarget();
    nlohmann::json persistentConfig;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        targetProcessName_ = pending->processName;
        wideTargetProcessName_.assign(targetProcessName_.begin(), targetProcessName_.end());
        preferredWindowTitle_ = pending->preferredTitle;
        config_["host"]["targetProcessName"] = targetProcessName_;
        config_["host"]["window"]["preferredTitleContains"] = WideToUtf8(preferredWindowTitle_);
        persistentConfig = config_;
    }
    std::string saveError;
    if (!ConfigStore::Save(persistentConfig, saveError)) {
        Diagnostics::Log("WARNING", "CONFIG", "Target changed but configuration persistence failed", saveError);
    }
    nextTargetPoll_ = std::chrono::steady_clock::now();
    SetState(HostState::WaitingForTarget);
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

    if (!ConfigUtils::LoadNetworkEndpoints(config_, endpoints_)) return false;
    const bool production = endpoints_.mode == "production";
    if (production) {
        if (endpoints_.signalingUrl.rfind("wss://", 0) != 0 ||
            (!endpoints_.matchmakerUrl.empty() && endpoints_.matchmakerUrl.rfind("https://", 0) != 0)) {
            SetState(HostState::Failed, "Production requires WSS signaling and HTTPS matchmaker endpoints");
            return false;
        }
    } else if (endpoints_.mode != "local" && endpoints_.mode != "lan") {
        SetState(HostState::Failed, "Unknown network mode");
        return false;
    }

    hostSecret_ = ReadCredential("CLOUDGAMING_HOST_SECRET", "hostSecret");
    if (production && hostSecret_.size() < 32) {
        SetState(HostState::Failed, "CLOUDGAMING_HOST_SECRET is required in production");
        return false;
    }
    SetEnvironmentVariableA("PION_NETWORK_MODE", endpoints_.mode.c_str());
    std::string turnUrls = ReadCredential("PION_TURN_URLS", "turnUrls");
    if (turnUrls.empty()) turnUrls = ReadEnvironment("PION_TURN_URL");
    const std::string turnUsername = ReadCredential("PION_TURN_USERNAME", "turnUsername");
    const std::string turnCredential = ReadCredential("PION_TURN_CREDENTIAL", "turnCredential");
    if (!turnUrls.empty() && (!HasValidIceUrlList(turnUrls) ||
        turnUsername.empty() || turnCredential.empty())) {
        SetState(HostState::Failed, "TURN URLs and credentials are incomplete or invalid");
        return false;
    }
    if (production && turnUrls.empty()) {
        SetState(HostState::Failed, "Production requires configured TURN credentials");
        return false;
    }
    if (!turnUrls.empty()) {
        SetEnvironmentVariableA("PION_TURN_URLS", turnUrls.c_str());
        SetEnvironmentVariableA("PION_TURN_USERNAME", turnUsername.c_str());
        SetEnvironmentVariableA("PION_TURN_CREDENTIAL", turnCredential.c_str());
    }
    matchmakerEnabled_ = !endpoints_.matchmakerUrl.empty();
    if (config_.contains("host") && config_["host"].contains("matchmaker")) {
        const auto& matchmaker = config_["host"]["matchmaker"];
        heartbeatIntervalMs_ = std::max(1000, matchmaker.value("heartbeatIntervalMs", heartbeatIntervalMs_));
    }

    const int configuredFps = config_.contains("host") && config_["host"].contains("video")
        ? config_["host"]["video"].value("fps", 60) : 60;
    if (!(config_.contains("host") && config_["host"].contains("video"))) {
        SetState(HostState::Failed, "Missing host.video configuration");
        return false;
    }
    std::string profileError;
    if (!streamProfiles_.Configure(config_["host"]["video"], profileError)) {
        SetState(HostState::Failed, "Invalid default stream profile: " + profileError);
        return false;
    }
    SetStreamProfileManager(&streamProfiles_);
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

    initWebsocket(roomId_, endpoints_.signalingUrl, hostSecret_, endpoints_.mode, &session_, &streamProfiles_);
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

        const auto generatedRoomId = generateRoomId();
        const auto generatedHostId = generateHostId();
        {
            std::lock_guard<std::mutex> lock(mutex_);
            roomId_ = generatedRoomId;
            hostId_ = generatedHostId;
        }
        PrintBanner(generatedRoomId);
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
    std::wstring processName;
    std::wstring preferredTitle;
    std::string narrowProcessName;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        processName = wideTargetProcessName_;
        preferredTitle = preferredWindowTitle_;
        narrowProcessName = targetProcessName_;
    }
    HWND window = nullptr;
    DWORD pid = 0;
    if (!WindowUtils::PickWindowByProcessName(
            processName, window, pid, preferredTitle, false) || !window) {
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
    targetWindow_.store(window, std::memory_order_release);
    targetPid_.store(pid, std::memory_order_release);
    WindowUtils::SetTargetWindow(window);
    StartCapture();
    GraphicsAndCapture::Start(capture_);
    captureStarted_ = true;

    audioStarted_ = audio_.StartCapture(pid, narrowProcessName);
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
    targetWindow_.store(nullptr, std::memory_order_release);
    targetPid_.store(0, std::memory_order_release);
}

void HostRuntime::Tick() {
    ApplyPendingTargetSelection();
    const auto now = std::chrono::steady_clock::now();
    if (!captureStarted_) {
        if (now >= nextTargetPoll_) {
            TryAttachTarget();
            nextTargetPoll_ = now + std::chrono::milliseconds(windowPollIntervalMs_);
        }
        return;
    }

    const HWND activeWindow = targetWindow_.load(std::memory_order_acquire);
    if (!activeWindow || !IsWindow(activeWindow)) {
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
    if (peerState != lastPeerState_.load(std::memory_order_relaxed)) {
        lastPeerState_.store(peerState, std::memory_order_relaxed);
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
    auto nextHealthLog = std::chrono::steady_clock::now();
    while (!IsStopRequested()) {
        Tick();
        if (std::chrono::steady_clock::now() >= nextHealthLog) {
            Diagnostics::Log("INFO", "HEALTH", "periodic health snapshot", GetHealthSnapshot().dump());
            nextHealthLog = std::chrono::steady_clock::now() + std::chrono::seconds(30);
        }
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
    {
        std::lock_guard<std::mutex> lock(mutex_);
        roomId_.clear();
        hostId_.clear();
    }
    lastPeerState_.store(-1, std::memory_order_release);
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
    status.targetPid = targetPid_.load(std::memory_order_acquire);
    status.targetWindow = targetWindow_.load(std::memory_order_acquire);
    status.peerConnectionState = lastPeerState_.load(std::memory_order_relaxed);
    return status;
}

nlohmann::json HostRuntime::GetHealthSnapshot() const {
    const auto runtime = GetStatus();
    const auto session = session_.GetStatus();
    const auto profile = streamProfiles_.GetStatus();
    const auto audio = audio_.GetStatus();
    const auto capture = GetCaptureHealth();
    const auto encoder = Encoder::GetHealth();
    const auto logging = Diagnostics::GetStatus();
    const auto network = RuntimeMetrics::GetNetwork();
    nlohmann::json input{{"running", InputIntegrationLayer::isRunning()}};
    if (auto* transport = InputTransportLayer::getGlobalTransport()) {
        const auto stats = transport->getStats();
        input.update({{"received", stats.messagesReceived}, {"processed", stats.messagesProcessed},
                      {"dropped", stats.messagesDropped}, {"queueDepth", stats.queueSize},
                      {"maxQueueDepth", stats.maxQueueSize}});
    }
    auto profileJson = [](const std::optional<StreamProfileManager::Profile>& value) -> nlohmann::json {
        if (!value) return nullptr;
        return {{"width", value->width}, {"height", value->height},
                {"fps", value->fps}, {"bitrate", value->bitrate}};
    };
    return {
        {"runtime", {{"state", ToString(runtime.state)}, {"failureReason", runtime.failureReason},
                     {"hostId", runtime.hostId}, {"roomId", runtime.roomId},
                     {"targetProcess", runtime.targetProcessName}, {"targetPid", runtime.targetPid},
                     {"peerConnectionState", runtime.peerConnectionState}, {"networkMode", endpoints_.mode}}},
        {"session", {{"state", SessionManager::StateName(session.state)},
                     {"sessionId", session.sessionId}, {"failureReason", session.failureReason}}},
        {"profile", {{"state", StreamProfileManager::StateName(profile.state)},
                     {"requested", profileJson(profile.requested)}, {"active", profileJson(profile.active)},
                     {"rejectionReason", profile.rejectionReason}, {"generation", profile.generation}}},
        {"input", input},
        {"audio", {{"state", AudioCapturer::StateName(audio.state)}, {"failureReason", audio.failureReason},
                   {"processId", audio.processId}, {"captureQueueDepth", audio.captureQueueDepth},
                   {"encodedQueueDepth", audio.encodedQueueDepth}, {"captureDrops", audio.droppedCaptureFrames},
                   {"encodedDrops", audio.droppedEncodedPackets}, {"bitrate", audio.bitrate}}},
        {"capture", {{"running", capture.running}, {"targetFps", capture.targetFps},
                     {"framesArrived", capture.framesArrived}, {"queueDepth", capture.queueDepth},
                     {"overwriteDrops", capture.overwriteDrops}, {"backpressureSkips", capture.backpressureSkips},
                     {"outOfOrderFrames", capture.outOfOrderFrames}}},
        {"encoder", {{"initialized", encoder.initialized}, {"width", encoder.width},
                     {"height", encoder.height}, {"fps", encoder.fps}, {"bitrate", encoder.bitrate},
                     {"bitrateChangePending", encoder.bitrateChangePending},
                     {"hwAcquireFailures", encoder.hwAcquireFailures},
                     {"videoProcessorFailures", encoder.videoProcessorFailures},
                     {"submitFailures", encoder.submitFailures}}},
        {"network", {{"rttMs", network.rttMs}, {"jitterMs", network.jitterMs},
                     {"packetLoss", network.packetLoss}, {"sendBitrateKbps", network.sendBitrateKbps},
                     {"pacerQueueLength", network.pacerQueueLength}, {"nackCount", network.nackCount},
                     {"pliCount", network.pliCount}}},
        {"logging", {{"initialized", logging.initialized}, {"activeLog", logging.activeLog.string()},
                     {"recordsWritten", logging.recordsWritten}, {"writeFailures", logging.writeFailures}}}
    };
}

bool HostRuntime::CreateSupportBundle(std::filesystem::path& outputDirectory, std::string& error) const {
    nlohmann::json config;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        config = config_;
    }
    return Diagnostics::CreateSupportBundle(GetHealthSnapshot(), config, outputDirectory, error);
}

void PrintBanner(const std::string& roomId) {
    std::cout << "\n----------------------------------------\n"
              << "  Cloud Gaming Host Initialized\n"
              << "  Pairing room: " << roomId << "\n"
              << "----------------------------------------\n\n";
}

} // namespace Runtime
