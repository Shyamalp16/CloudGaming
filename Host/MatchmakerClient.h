#pragma once

#include <string>
#include <atomic>
#include <functional>
#include <thread>

namespace MatchmakerClient {
    // Initialize the matchmaker client with the server URL and authentication secret
    bool initialize(const std::string& url, const std::string& secret);

    enum class HeartbeatResult { Failed, Accepted, RotatePairingCode };

    // Send a single heartbeat to register/update the host.
	HeartbeatResult sendHeartbeat(const std::string& hostId, const std::string& roomId,
		const std::string& pairingCode);

    // Start a background thread that sends heartbeats at the specified interval
	void startHeartbeatThread(const std::string& hostId, const std::string& roomId,
		const std::string& pairingCode, int intervalMs,
		std::function<void(const std::string&)> onPairingCodeRotated);

    // Stop the heartbeat thread gracefully
    void stopHeartbeatThread();

}

