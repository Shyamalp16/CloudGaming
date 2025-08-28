#pragma once

#include <string>
#include <thread>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <chrono>

// Only include legacy WebSocket code if explicitly enabled
#ifdef ENABLE_LEGACY_WEBSOCKET

#include "WebRTCWrapper.h"
#include "ShutdownManager.h"
#include "ErrorUtils.h"

// Legacy WebSocket thread variables (only compiled if legacy mode is enabled)
extern std::atomic<bool> g_legacy_input_poll_running;
extern std::thread g_legacy_kb_poller;
extern std::thread g_legacy_mouse_poller;
extern std::mutex g_legacy_poller_mutex;
extern std::condition_variable g_legacy_poller_cv;

// Forward declarations for legacy compatibility functions
extern "C" {
    void enqueueKeyboardMessage(const std::string& message);
    void enqueueMouseMessage(const std::string& message);
}

/**
 * @brief Legacy WebSocket polling functions
 *
 * These functions provide backward compatibility with the old direct WebRTC/WebSocket
 * polling approach. They are only compiled when ENABLE_LEGACY_WEBSOCKET is defined.
 */
namespace LegacyWebSocketCompat {

/**
 * @brief Start legacy input polling threads
 * @return true if started successfully, false otherwise
 */
bool startLegacyPolling();

/**
 * @brief Stop legacy input polling threads
 */
void stopLegacyPolling();

/**
 * @brief Check if legacy polling is running
 * @return true if running, false otherwise
 */
bool isLegacyPollingRunning();

/**
 * @brief Legacy keyboard polling thread function
 */
void legacyKeyboardPollingLoop();

/**
 * @brief Legacy mouse polling thread function
 */
void legacyMousePollingLoop();

} // namespace LegacyWebSocketCompat

#else // ENABLE_LEGACY_WEBSOCKET not defined

// Stub implementations when legacy WebSocket is disabled
namespace LegacyWebSocketCompat {

inline bool startLegacyPolling() {
    LOG_WARNING(ErrorUtils::ErrorCategory::INPUT,
               "Legacy WebSocket support not compiled in (ENABLE_LEGACY_WEBSOCKET not defined)");
    return false;
}

inline void stopLegacyPolling() {
    // No-op when legacy WebSocket is not compiled
}

inline bool isLegacyPollingRunning() {
    return false;
}

} // namespace LegacyWebSocketCompat

#endif // ENABLE_LEGACY_WEBSOCKET
