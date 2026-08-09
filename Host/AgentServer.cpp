#include "pch.h"
#include "AgentServer.h"

#include <sddl.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <limits>
#include <stdexcept>
#include <vector>

#include "ConfigStore.h"
#include "Diagnostics.h"
#include "ProcessDiscovery.h"
#include "Version.h"

#pragma comment(lib, "Advapi32.lib")

namespace {
constexpr DWORD kPipeBufferBytes = 64 * 1024;
constexpr std::uint32_t kMaximumMessageBytes = 1024 * 1024;

bool IsValidPipeName(const std::string& value) {
    if (value.empty() || value.size() > 128) return false;
    return std::all_of(value.begin(), value.end(), [](unsigned char c) {
        return std::isalnum(c) || c == '.' || c == '-' || c == '_';
    });
}

std::wstring Utf8ToWide(const std::string& value) {
    if (value.empty()) return {};
    const int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
        static_cast<int>(value.size()), nullptr, 0);
    if (size <= 0) return {};
    std::wstring result(static_cast<size_t>(size), L'\0');
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
            static_cast<int>(value.size()), result.data(), size) != size) return {};
    return result;
}

std::string WideToUtf8(const std::wstring& value) {
    if (value.empty()) return {};
    const int size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
        static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (size <= 0) return {};
    std::string result(static_cast<size_t>(size), '\0');
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
            static_cast<int>(value.size()), result.data(), size, nullptr, nullptr) != size) return {};
    return result;
}

bool ReadExact(HANDLE pipe, void* destination, DWORD bytes) {
    auto* output = static_cast<unsigned char*>(destination);
    DWORD offset = 0;
    while (offset < bytes) {
        DWORD read = 0;
        if (!ReadFile(pipe, output + offset, bytes - offset, &read, nullptr) || read == 0) return false;
        offset += read;
    }
    return true;
}

bool WriteAll(HANDLE pipe, const void* source, DWORD bytes) {
    const auto* input = static_cast<const unsigned char*>(source);
    DWORD offset = 0;
    while (offset < bytes) {
        DWORD written = 0;
        if (!WriteFile(pipe, input + offset, bytes - offset, &written, nullptr) || written == 0) return false;
        offset += written;
    }
    return true;
}

bool BuildPipeSecurity(SECURITY_ATTRIBUTES& attributes,
                       PSECURITY_DESCRIPTOR& descriptor,
                       std::string& error) {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
        error = "OpenProcessToken failed: " + std::to_string(GetLastError());
        return false;
    }
    DWORD bytes = 0;
    GetTokenInformation(token, TokenUser, nullptr, 0, &bytes);
    std::vector<BYTE> storage(bytes);
    if (bytes == 0 || !GetTokenInformation(token, TokenUser, storage.data(), bytes, &bytes)) {
        error = "GetTokenInformation failed: " + std::to_string(GetLastError());
        CloseHandle(token);
        return false;
    }
    CloseHandle(token);

    LPWSTR sidString = nullptr;
    if (!ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(storage.data())->User.Sid, &sidString)) {
        error = "ConvertSidToStringSid failed: " + std::to_string(GetLastError());
        return false;
    }
    const std::wstring sddl = L"D:P(A;;GA;;;SY)(A;;GA;;;" + std::wstring(sidString) + L")";
    LocalFree(sidString);

    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
            sddl.c_str(), SDDL_REVISION_1, &descriptor, nullptr)) {
        error = "Security descriptor conversion failed: " + std::to_string(GetLastError());
        return false;
    }
    attributes.nLength = sizeof(attributes);
    attributes.lpSecurityDescriptor = descriptor;
    attributes.bInheritHandle = FALSE;
    return true;
}

nlohmann::json ErrorResponse(const std::string& requestId,
                             std::string code,
                             std::string message,
                             bool retryable = false) {
    return {{"protocolVersion", AgentServer::kProtocolVersion},
            {"kind", "response"},
            {"requestId", requestId},
            {"ok", false},
            {"error", {{"code", std::move(code)},
                       {"message", std::move(message)},
                       {"retryable", retryable}}}};
}

nlohmann::json SuccessResponse(const std::string& requestId, nlohmann::json result) {
    return {{"protocolVersion", AgentServer::kProtocolVersion},
            {"kind", "response"},
            {"requestId", requestId},
            {"ok", true},
            {"result", std::move(result)}};
}

const char* ArchitectureName() {
#if defined(_M_X64)
    return "x64";
#elif defined(_M_ARM64)
    return "arm64";
#else
    return "unknown";
#endif
}
}

AgentServer::AgentServer(std::string pipeName) : pipeName_(std::move(pipeName)) {
    if (!IsValidPipeName(pipeName_)) throw std::invalid_argument("Invalid agent pipe name");
    pipePath_ = L"\\\\.\\pipe\\" + Utf8ToWide(pipeName_);
    controller_.SetStatusCallback([this] { PublishStatus(); });
}

AgentServer::~AgentServer() {
    controller_.SetStatusCallback({});
    shutdownRequested_.store(true, std::memory_order_release);
    eventCondition_.notify_all();
    if (eventThread_.joinable()) eventThread_.join();
    controller_.StopAsync();
    controller_.WaitForStop();
}

int AgentServer::Run() {
    Diagnostics::Log("INFO", "AGENT", "Native agent started", pipeName_);
    eventThread_ = std::thread(&AgentServer::EventLoop, this);
    while (!shutdownRequested_.load(std::memory_order_acquire)) {
        std::string error;
        HANDLE pipe = CreatePipe(error);
        if (pipe == INVALID_HANDLE_VALUE) {
            Diagnostics::Log("ERROR", "AGENT", "Failed to create control pipe", error);
            return EXIT_FAILURE;
        }

        const BOOL connected = ConnectNamedPipe(pipe, nullptr) ? TRUE :
            (GetLastError() == ERROR_PIPE_CONNECTED ? TRUE : FALSE);
        if (!connected) {
            const DWORD lastError = GetLastError();
            CloseHandle(pipe);
            if (shutdownRequested_.load(std::memory_order_acquire)) break;
            Diagnostics::Log("WARN", "AGENT", "Control pipe connection failed", std::to_string(lastError));
            continue;
        }

        {
            std::lock_guard<std::mutex> lock(connectionMutex_);
            connection_ = pipe;
        }
        Diagnostics::Log("INFO", "AGENT", "Desktop control client connected");
        HandleConnection(pipe);
        {
            std::lock_guard<std::mutex> lock(connectionMutex_);
            if (connection_ == pipe) connection_ = INVALID_HANDLE_VALUE;
        }
        FlushFileBuffers(pipe);
        DisconnectNamedPipe(pipe);
        CloseHandle(pipe);
        Diagnostics::Log("INFO", "AGENT", "Desktop control client disconnected");
    }

    controller_.StopAsync();
    controller_.WaitForStop();
    shutdownRequested_.store(true, std::memory_order_release);
    eventCondition_.notify_all();
    if (eventThread_.joinable()) eventThread_.join();
    Diagnostics::Log("INFO", "AGENT", "Native agent stopped");
    return EXIT_SUCCESS;
}

HANDLE AgentServer::CreatePipe(std::string& error) const {
    SECURITY_ATTRIBUTES attributes{};
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    if (!BuildPipeSecurity(attributes, descriptor, error)) return INVALID_HANDLE_VALUE;
    const HANDLE pipe = CreateNamedPipeW(pipePath_.c_str(),
        PIPE_ACCESS_DUPLEX,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
        1, kPipeBufferBytes, kPipeBufferBytes, 0, &attributes);
    const DWORD lastError = pipe == INVALID_HANDLE_VALUE ? GetLastError() : ERROR_SUCCESS;
    LocalFree(descriptor);
    if (pipe == INVALID_HANDLE_VALUE) error = "CreateNamedPipe failed: " + std::to_string(lastError);
    return pipe;
}

void AgentServer::HandleConnection(HANDLE pipe) {
    while (!shutdownRequested_.load(std::memory_order_acquire)) {
        std::uint32_t length = 0;
        if (!ReadExact(pipe, &length, sizeof(length))) return;
        if (length == 0 || length > kMaximumMessageBytes) {
            Diagnostics::Log("WARN", "AGENT", "Rejected invalid control message length", std::to_string(length));
            return;
        }
        std::string payload(length, '\0');
        if (!ReadExact(pipe, payload.data(), length)) return;

        nlohmann::json response;
        try {
            response = Dispatch(nlohmann::json::parse(payload));
        } catch (const nlohmann::json::exception& ex) {
            response = ErrorResponse("", "INVALID_JSON", ex.what());
        } catch (const std::exception& ex) {
            response = ErrorResponse("", "INTERNAL_ERROR", ex.what(), true);
        }
        if (!Send(pipe, response)) return;
    }
}

nlohmann::json AgentServer::Dispatch(const nlohmann::json& request) {
    const std::string requestId = request.value("requestId", std::string{});
    if (request.value("protocolVersion", 0) != kProtocolVersion) {
        return ErrorResponse(requestId, "INCOMPATIBLE_PROTOCOL", "Unsupported native agent protocol version");
    }
    if (request.value("kind", std::string{}) != "request" || requestId.empty() || requestId.size() > 128) {
        return ErrorResponse(requestId, "INVALID_REQUEST", "Invalid request envelope");
    }

    const std::string method = request.value("method", std::string{});
    const nlohmann::json params = request.value("params", nlohmann::json::object());
    if (!params.is_object()) return ErrorResponse(requestId, "INVALID_PARAMS", "Request params must be an object");

    if (method == "system.hello") {
        return SuccessResponse(requestId, {{"protocolVersion", kProtocolVersion},
            {"nativeVersion", CLOUD_GAMING_VERSION},
            {"architecture", ArchitectureName()},
            {"commands", {"system.hello", "host.getSnapshot", "host.listTargets",
                "host.selectTarget", "host.start", "host.stop", "host.shutdownAgent"}}});
    }
    if (method == "host.getSnapshot") return SuccessResponse(requestId, Snapshot());
    if (method == "host.listTargets") {
        nlohmann::json targets = nlohmann::json::array();
        for (const auto& target : ProcessDiscovery::EnumerateTargets()) {
            targets.push_back({{"processId", target.processId},
                {"processName", WideToUtf8(target.processName)},
                {"title", WideToUtf8(target.title)},
                {"clientWidth", target.clientWidth},
                {"clientHeight", target.clientHeight},
                {"minimized", target.minimized}});
        }
        return SuccessResponse(requestId, std::move(targets));
    }
    if (method == "host.selectTarget") {
        const std::string processName = params.value("processName", std::string{});
        const std::string preferredTitle = params.value("preferredTitle", std::string{});
        if (processName.empty() || processName.size() > 260 || preferredTitle.size() > 512) {
            return ErrorResponse(requestId, "INVALID_TARGET", "Invalid process name or preferred title");
        }
        std::string error;
        if (!controller_.SelectTarget(processName, Utf8ToWide(preferredTitle), error)) {
            return ErrorResponse(requestId, "TARGET_REJECTED", error.empty() ? "Target selection was rejected" : error, true);
        }
        return SuccessResponse(requestId, Snapshot());
    }
    if (method == "host.start") {
        controller_.StartAsync();
        return SuccessResponse(requestId, Snapshot());
    }
    if (method == "host.stop") {
        controller_.StopAsync();
        return SuccessResponse(requestId, Snapshot());
    }
    if (method == "host.shutdownAgent") {
        controller_.StopAsync();
        shutdownRequested_.store(true, std::memory_order_release);
        return SuccessResponse(requestId, Snapshot());
    }
    return ErrorResponse(requestId, "UNKNOWN_METHOD", "Unknown native agent command");
}

nlohmann::json AgentServer::Snapshot() const {
    const auto status = controller_.GetStatus();
    return {{"status", {{"state", Runtime::ToString(status.state)},
                        {"failureReason", status.failureReason},
                        {"hostId", status.hostId},
                        {"roomId", status.roomId},
                        {"pairingCode", status.pairingCode},
                        {"targetProcessName", status.targetProcessName},
                        {"targetPid", status.targetPid},
                        {"peerConnectionState", status.peerConnectionState}}},
            {"health", controller_.GetHealthSnapshot()}};
}

void AgentServer::PublishStatus() {
    {
        std::lock_guard<std::mutex> lock(eventMutex_);
        statusDirty_ = true;
    }
    eventCondition_.notify_one();
}

void AgentServer::EventLoop() {
    while (true) {
        {
            std::unique_lock<std::mutex> lock(eventMutex_);
            eventCondition_.wait(lock, [this] {
                return statusDirty_ || shutdownRequested_.load(std::memory_order_acquire);
            });
            if (shutdownRequested_.load(std::memory_order_acquire)) return;
            statusDirty_ = false;
        }

        nlohmann::json snapshot;
        try {
            snapshot = Snapshot();
        } catch (const std::exception& ex) {
            Diagnostics::Log("WARN", "AGENT", "Failed to create status event", ex.what());
            continue;
        }
    const nlohmann::json message{{"protocolVersion", kProtocolVersion},
        {"kind", "event"},
        {"event", "host.statusChanged"},
        {"sequence", eventSequence_.fetch_add(1, std::memory_order_relaxed) + 1},
            {"data", std::move(snapshot)}};
        std::lock_guard<std::mutex> lock(connectionMutex_);
        if (connection_ != INVALID_HANDLE_VALUE) Send(connection_, message);
    }
}

bool AgentServer::Send(HANDLE pipe, const nlohmann::json& message) {
    const std::string payload = message.dump();
    if (payload.empty() || payload.size() > kMaximumMessageBytes ||
        payload.size() > std::numeric_limits<std::uint32_t>::max()) return false;
    const auto length = static_cast<std::uint32_t>(payload.size());
    std::lock_guard<std::mutex> lock(writeMutex_);
    return WriteAll(pipe, &length, sizeof(length)) &&
        WriteAll(pipe, payload.data(), length);
}
