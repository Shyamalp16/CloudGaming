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

static HeartbeatResult parseHeartbeatResponse(const httplib::Result& response, const char* transport) {
    if (!response) {
        std::cerr << "[MatchmakerClient] Heartbeat request failed (" << transport << "): "
                  << httplib::to_string(response.error()) << std::endl;
        return HeartbeatResult::Failed;
    }
    if (response->status != 200 || response->body.size() > 4096) {
        std::cerr << "[MatchmakerClient] Heartbeat failed with status: " << response->status << std::endl;
        return HeartbeatResult::Failed;
    }
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
		if (g_matchmakerUrl.empty() || g_matchmakerUrl.size() > 2048) return HeartbeatResult::Failed;
		const std::wstring endpoint = utf8ToWide(g_matchmakerUrl);
		if (endpoint.empty()) return HeartbeatResult::Failed;
		URL_COMPONENTS parts{sizeof(parts)};
		parts.dwSchemeLength = parts.dwHostNameLength = parts.dwUrlPathLength =
			parts.dwExtraInfoLength = parts.dwUserNameLength = parts.dwPasswordLength = static_cast<DWORD>(-1);
		if (!WinHttpCrackUrl(endpoint.c_str(), 0, 0, &parts) ||
			(parts.nScheme != INTERNET_SCHEME_HTTP && parts.nScheme != INTERNET_SCHEME_HTTPS) ||
			parts.dwUserNameLength || parts.dwPasswordLength || parts.dwExtraInfoLength ||
			(parts.dwUrlPathLength > 1)) {
			std::cerr << "[MatchmakerClient] Invalid matchmaker endpoint" << std::endl;
			return HeartbeatResult::Failed;
		}
		const std::string host = wideToUtf8(parts.lpszHostName, parts.dwHostNameLength);
		const int port = parts.nPort;
		const bool isHttps = parts.nScheme == INTERNET_SCHEME_HTTPS;
		if (host.empty() || port < 1 || port > 65535) return HeartbeatResult::Failed;

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

        std::string body = payload.dump();

        httplib::Headers headers;
        headers.emplace("Content-Type", "application/json");
        headers.emplace("Authorization", "Bearer " + g_hostSecret);

        if (isHttps) {
#ifdef CPPHTTPLIB_OPENSSL_SUPPORT
            httplib::SSLClient cli(host, port);
			cli.enable_server_certificate_verification(true);
			cli.set_follow_location(false);
            cli.set_connection_timeout(5);
            cli.set_read_timeout(5);
            cli.set_write_timeout(5);

            auto res = cli.Post("/api/host/heartbeat", headers, body, "application/json");
            return parseHeartbeatResponse(res, "HTTPS");
#else
            std::cerr << "[MatchmakerClient] HTTPS URL configured but OpenSSL support is not enabled in cpp-httplib build" << std::endl;
            return HeartbeatResult::Failed;
#endif
        } else {
            httplib::Client cli(host, port);
            cli.set_connection_timeout(5);
            cli.set_read_timeout(5);
            cli.set_write_timeout(5);

            auto res = cli.Post("/api/host/heartbeat", headers, body, "application/json");
            return parseHeartbeatResponse(res, "HTTP");
        }
    } catch (const std::exception& e) {
        std::cerr << "[MatchmakerClient] Exception during heartbeat: " << e.what() << std::endl;
        return HeartbeatResult::Failed;
    }
}

void heartbeatLoop(std::string hostId, std::string roomId, std::string pairingCode, int intervalMs,
                   std::function<void(const std::string&)> onPairingCodeRotated) {
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
						  std::function<void(const std::string&)> onPairingCodeRotated) {
    std::lock_guard<std::mutex> lock(g_mutex);
    
    if (g_heartbeatRunning) {
        std::cerr << "[MatchmakerClient] Heartbeat thread already running" << std::endl;
        return;
    }
    
    g_stopHeartbeat = false;
    g_heartbeatRunning = true;
	g_heartbeatThread = std::thread(heartbeatLoop, hostId, roomId, pairingCode, intervalMs,
		std::move(onPairingCodeRotated));
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

