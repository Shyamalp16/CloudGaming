#pragma once

#include <string>
#include <atomic>
#include <thread>

namespace MatchmakerClient {
    // Initialize the matchmaker client with the server URL and authentication secret
    bool initialize(const std::string& url, const std::string& secret);

    // Send a single heartbeat to register/update the host
    bool sendHeartbeat(const std::string& hostId, const std::string& roomId);

    // Start a background thread that sends heartbeats at the specified interval
    void startHeartbeatThread(const std::string& hostId, const std::string& roomId, int intervalMs);

    // Stop the heartbeat thread gracefully
    void stopHeartbeatThread();

    // Check if the matchmaker client is initialized
    bool isInitialized();

    // Check if the heartbeat thread is running
    bool isHeartbeatRunning();
}

