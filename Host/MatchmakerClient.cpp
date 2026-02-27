#include "MatchmakerClient.h"
#include "httplib.h"
#include <nlohmann/json.hpp>
#include <iostream>
#include <mutex>
#include <chrono>

namespace MatchmakerClient {

static std::string g_matchmakerUrl;
static std::string g_hostSecret;
static std::atomic<bool> g_initialized{false};
static std::atomic<bool> g_heartbeatRunning{false};
static std::atomic<bool> g_stopHeartbeat{false};
static std::thread g_heartbeatThread;
static std::mutex g_mutex;

bool initialize(const std::string& url, const std::string& secret) {
    std::lock_guard<std::mutex> lock(g_mutex);
    
    if (url.empty()) {
        std::cerr << "[MatchmakerClient] Error: Empty matchmaker URL" << std::endl;
        return false;
    }
    
    g_matchmakerUrl = url;
    g_hostSecret = secret;
    g_initialized = true;
    
    std::cout << "[MatchmakerClient] Initialized with URL: " << url << std::endl;
    return true;
}

bool sendHeartbeat(const std::string& hostId, const std::string& roomId) {
    if (!g_initialized) {
        std::cerr << "[MatchmakerClient] Error: Client not initialized" << std::endl;
        return false;
    }

    try {
        std::string url = g_matchmakerUrl;
        std::string host;
        int port = 80;
        
        if (url.find("http://") == 0) {
            url = url.substr(7);
        } else if (url.find("https://") == 0) {
            url = url.substr(8);
            port = 443;
        }
        
        size_t colonPos = url.find(':');
        size_t slashPos = url.find('/');
        
        if (colonPos != std::string::npos && (slashPos == std::string::npos || colonPos < slashPos)) {
            host = url.substr(0, colonPos);
            size_t portEnd = (slashPos != std::string::npos) ? slashPos : url.length();
            port = std::stoi(url.substr(colonPos + 1, portEnd - colonPos - 1));
        } else {
            host = (slashPos != std::string::npos) ? url.substr(0, slashPos) : url;
        }

        httplib::Client cli(host, port);
        cli.set_connection_timeout(5);  
        cli.set_read_timeout(5);
        cli.set_write_timeout(5);

        nlohmann::json payload;
        payload["hostId"] = hostId;
        payload["roomId"] = roomId;
        payload["status"] = "idle";
        payload["region"] = "local";
        payload["capacity"] = 1;
        payload["availableSlots"] = 1;

        std::string body = payload.dump();

        httplib::Headers headers;
        headers.emplace("Content-Type", "application/json");
        headers.emplace("Authorization", "Bearer " + g_hostSecret);

        auto res = cli.Post("/api/host/heartbeat", headers, body, "application/json");

        if (res) {
            if (res->status == 200) {
                std::cout << "[MatchmakerClient] Heartbeat sent successfully" << std::endl;
                return true;
            } else {
                std::cerr << "[MatchmakerClient] Heartbeat failed with status: " << res->status 
                          << " body: " << res->body << std::endl;
                return false;
            }
        } else {
            std::cerr << "[MatchmakerClient] Heartbeat request failed: connection error" << std::endl;
            return false;
        }
    } catch (const std::exception& e) {
        std::cerr << "[MatchmakerClient] Exception during heartbeat: " << e.what() << std::endl;
        return false;
    }
}

void heartbeatLoop(std::string hostId, std::string roomId, int intervalMs) {
    try {
        std::cout << "[MatchmakerClient] Heartbeat thread started (interval: " << intervalMs << "ms)" << std::endl;
        
        while (!g_stopHeartbeat) {
            try {
                sendHeartbeat(hostId, roomId);
            } catch (const std::exception& e) {
                std::cerr << "[MatchmakerClient] Heartbeat exception: " << e.what() << std::endl;
            } catch (...) {
                std::cerr << "[MatchmakerClient] Unknown heartbeat exception" << std::endl;
            }
            
            int sleptMs = 0;
            while (sleptMs < intervalMs && !g_stopHeartbeat) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                sleptMs += 100;
            }
        }
        
        std::cout << "[MatchmakerClient] Heartbeat thread stopped" << std::endl;
    } catch (const std::exception& e) {
        std::cerr << "[MatchmakerClient] Fatal heartbeat thread exception: " << e.what() << std::endl;
    } catch (...) {
        std::cerr << "[MatchmakerClient] Fatal unknown heartbeat thread exception" << std::endl;
    }
}

void startHeartbeatThread(const std::string& hostId, const std::string& roomId, int intervalMs) {
    std::lock_guard<std::mutex> lock(g_mutex);
    
    if (g_heartbeatRunning) {
        std::cerr << "[MatchmakerClient] Heartbeat thread already running" << std::endl;
        return;
    }
    
    g_stopHeartbeat = false;
    g_heartbeatRunning = true;
    g_heartbeatThread = std::thread(heartbeatLoop, hostId, roomId, intervalMs);
}

void stopHeartbeatThread() {
    g_stopHeartbeat = true;
    
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
    std::cout << "[MatchmakerClient] Heartbeat thread stopped and joined" << std::endl;
}

bool isInitialized() {
    return g_initialized;
}

bool isHeartbeatRunning() {
    return g_heartbeatRunning;
}

}

