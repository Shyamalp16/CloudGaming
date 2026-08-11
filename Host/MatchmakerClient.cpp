#include "MatchmakerClient.h"
#include "httplib.h"
#include <nlohmann/json.hpp>
#include <iostream>
#include <mutex>
#include <chrono>
#include <condition_variable>
#include <algorithm>
#include <Windows.h>
#include <Winhttp.h>
#include "IdGenerator.h"
#include "pion_webrtc.h"

namespace MatchmakerClient {

#pragma comment(lib, "Winhttp.lib")

static std::string g_matchmakerUrl;
static std::string g_hostSecret;
static std::atomic<bool> g_initialized{false};
static std::atomic<bool> g_heartbeatRunning{false};
static std::atomic<bool> g_stopHeartbeat{false};
static std::atomic<double> g_lastRttMs{200.0};
static std::thread g_heartbeatThread;
static std::mutex g_mutex;
static std::mutex g_heartbeatWaitMutex;
static std::condition_variable g_heartbeatWait;

static std::wstring utf8ToWide(const std::string& value) {
    if (value.empty()) return {};
    const int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                                            static_cast<int>(value.size()), nullptr, 0);
    if (length <= 0) return {};
    std::wstring result(static_cast<size_t>(length), L'\0');
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                            static_cast<int>(value.size()), result.data(), length) != length) {
        return {};
    }
    return result;
}

static std::string wideToUtf8(const wchar_t* value, size_t length) {
    if (!value || length == 0) return {};
    const int bytes = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value,
                                          static_cast<int>(length), nullptr, 0, nullptr, nullptr);
    if (bytes <= 0) return {};
    std::string result(static_cast<size_t>(bytes), '\0');
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value, static_cast<int>(length),
                            result.data(), bytes, nullptr, nullptr) != bytes) {
        return {};
    }
    return result;
}

bool initialize(const std::string& url, const std::string& secret) {
    std::lock_guard<std::mutex> lock(g_mutex);
    
    if (url.empty()) {
        std::cerr << "[MatchmakerClient] Error: Empty matchmaker URL" << std::endl;
        return false;
    }
    
    g_matchmakerUrl = url;
    g_hostSecret = secret;
    g_initialized = true;
    
    std::cout << "[MatchmakerClient] Initialized" << std::endl;
    return true;
}

struct HttpResponse { int status; std::string body; };

static std::optional<HttpResponse> postJson(const std::string& path, const nlohmann::json& payload) {
    if (!g_initialized || g_matchmakerUrl.empty() || g_matchmakerUrl.size() > 2048) return std::nullopt;
    const std::wstring endpoint = utf8ToWide(g_matchmakerUrl);
    URL_COMPONENTS parts{sizeof(parts)};
    parts.dwSchemeLength = parts.dwHostNameLength = parts.dwUrlPathLength =
        parts.dwExtraInfoLength = parts.dwUserNameLength = parts.dwPasswordLength = static_cast<DWORD>(-1);
    if (endpoint.empty() || !WinHttpCrackUrl(endpoint.c_str(), 0, 0, &parts) ||
        (parts.nScheme != INTERNET_SCHEME_HTTP && parts.nScheme != INTERNET_SCHEME_HTTPS) ||
        parts.dwUserNameLength || parts.dwPasswordLength || parts.dwExtraInfoLength || parts.dwUrlPathLength > 1)
        return std::nullopt;
    const auto host = wideToUtf8(parts.lpszHostName, parts.dwHostNameLength);
    const int port = parts.nPort;
    if (host.empty() || port < 1 || port > 65535) return std::nullopt;
    const httplib::Headers headers{{"Authorization", "Bearer " + g_hostSecret}};
    httplib::Result response;
    if (parts.nScheme == INTERNET_SCHEME_HTTPS) {
#ifdef CPPHTTPLIB_OPENSSL_SUPPORT
        httplib::SSLClient client(host, port);
        client.enable_server_certificate_verification(true);
        client.set_follow_location(false);
        client.set_connection_timeout(5); client.set_read_timeout(5); client.set_write_timeout(5);
        response = client.Post(path, headers, payload.dump(), "application/json");
#else
        return std::nullopt;
#endif
    } else {
        httplib::Client client(host, port);
        client.set_connection_timeout(5); client.set_read_timeout(5); client.set_write_timeout(5);
        response = client.Post(path, headers, payload.dump(), "application/json");
    }
    if (!response || response->body.size() > 64 * 1024) return std::nullopt;
    return HttpResponse{response->status, response->body};
}

static HeartbeatResult parseHeartbeatResponse(const std::optional<HttpResponse>& response) {
    if (!response || response->status != 200 || response->body.size() > 4096)
        return HeartbeatResult::Failed;
    const auto body = nlohmann::json::parse(response->body, nullptr, false);
    if (!body.is_object() || body.value("success", false) != true) return HeartbeatResult::Failed;
    return body.value("rotatePairingCode", false) ? HeartbeatResult::RotatePairingCode
                                                   : HeartbeatResult::Accepted;
}

HeartbeatResult sendHeartbeat(const std::string& hostId, const std::string& roomId,
                              const std::string& pairingCode) {
    if (!g_initialized) {
        std::cerr << "[MatchmakerClient] Error: Client not initialized" << std::endl;
        return HeartbeatResult::Failed;
    }

    try {
        nlohmann::json payload;
        payload["hostId"] = hostId;
        payload["roomId"] = roomId;
		payload["pairingCode"] = pairingCode;
        const int peerState = getPeerConnectionState();
        const bool occupied = peerState == 1 || peerState == 3;
        payload["status"] = occupied ? "busy" : "idle";
        payload["region"] = "local";
        payload["capacity"] = 1;
        payload["availableSlots"] = occupied ? 0 : 1;

        return parseHeartbeatResponse(postJson("/api/host/heartbeat", payload));
    } catch (const std::exception& e) {
        std::cerr << "[MatchmakerClient] Exception during heartbeat: " << e.what() << std::endl;
        return HeartbeatResult::Failed;
    }
}

bool sendPresence(const nlohmann::json& presence) {
    try {
        const auto started = std::chrono::steady_clock::now();
        const auto response = postJson("/api/v1/host/presence", presence);
        const auto elapsed = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - started).count();
        if (response) g_lastRttMs.store(std::clamp(elapsed, 0.0, 5000.0), std::memory_order_relaxed);
        return response && response->status == 200;
    } catch (...) { return false; }
}

double lastRttMs() noexcept { return g_lastRttMs.load(std::memory_order_relaxed); }

void heartbeatLoop(std::string hostId, std::string roomId, std::string pairingCode, int intervalMs,
                   std::function<void(const std::string&)> onPairingCodeRotated,
                   std::function<nlohmann::json()> presenceProvider) {
    intervalMs = std::clamp(intervalMs, 1000, 300000);
    try {
        std::cout << "[MatchmakerClient] Heartbeat thread started (interval: " << intervalMs << "ms)" << std::endl;
        
        while (!g_stopHeartbeat) {
            {
                std::unique_lock<std::mutex> waitLock(g_heartbeatWaitMutex);
                if (g_heartbeatWait.wait_for(waitLock, std::chrono::milliseconds(intervalMs), [] {
                        return g_stopHeartbeat.load();
                    })) {
                    break;
                }
            }
            try {
				const auto result = sendHeartbeat(hostId, roomId, pairingCode);
				if (result == HeartbeatResult::RotatePairingCode) {
					pairingCode = generateRoomId();
					if (onPairingCodeRotated) onPairingCodeRotated(pairingCode);
					(void)sendHeartbeat(hostId, roomId, pairingCode);
				}
                if (presenceProvider) (void)sendPresence(presenceProvider());
            } catch (const std::exception& e) {
                std::cerr << "[MatchmakerClient] Heartbeat exception: " << e.what() << std::endl;
            } catch (...) {
                std::cerr << "[MatchmakerClient] Unknown heartbeat exception" << std::endl;
            }
            
        }
        
        std::cout << "[MatchmakerClient] Heartbeat thread stopped" << std::endl;
    } catch (const std::exception& e) {
        std::cerr << "[MatchmakerClient] Fatal heartbeat thread exception: " << e.what() << std::endl;
    } catch (...) {
        std::cerr << "[MatchmakerClient] Fatal unknown heartbeat thread exception" << std::endl;
    }
    g_heartbeatRunning = false;
}

void startHeartbeatThread(const std::string& hostId, const std::string& roomId,
						  const std::string& pairingCode, int intervalMs,
						  std::function<void(const std::string&)> onPairingCodeRotated,
                          std::function<nlohmann::json()> presenceProvider) {
    std::lock_guard<std::mutex> lock(g_mutex);
    
    if (g_heartbeatRunning) {
        std::cerr << "[MatchmakerClient] Heartbeat thread already running" << std::endl;
        return;
    }
    
    g_stopHeartbeat = false;
    g_heartbeatRunning = true;
	g_heartbeatThread = std::thread(heartbeatLoop, hostId, roomId, pairingCode, intervalMs,
		std::move(onPairingCodeRotated), std::move(presenceProvider));
}

void stopHeartbeatThread() {
    g_stopHeartbeat = true;
    g_heartbeatWait.notify_all();
    
    try {
        if (g_heartbeatThread.joinable()) {
            g_heartbeatThread.join();
        }
    } catch (const std::exception& e) {
        std::cerr << "[MatchmakerClient] Exception while stopping heartbeat thread: " << e.what() << std::endl;
    } catch (...) {
        std::cerr << "[MatchmakerClient] Unknown exception while stopping heartbeat thread" << std::endl;
    }
    
    g_heartbeatRunning = false;
	{
		std::lock_guard<std::mutex> lock(g_mutex);
		if (!g_hostSecret.empty()) SecureZeroMemory(g_hostSecret.data(), g_hostSecret.size());
		g_hostSecret.clear();
		g_initialized = false;
	}
    std::cout << "[MatchmakerClient] Heartbeat thread stopped and joined" << std::endl;
}

}

