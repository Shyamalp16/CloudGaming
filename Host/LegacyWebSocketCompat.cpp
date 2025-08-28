// Only compile legacy WebSocket code if explicitly enabled
#ifdef ENABLE_LEGACY_WEBSOCKET

#include "LegacyWebSocketCompat.h"
#include <iostream>

// Legacy WebSocket thread variables
std::atomic<bool> g_legacy_input_poll_running{false};
std::thread g_legacy_kb_poller;
std::thread g_legacy_mouse_poller;
std::mutex g_legacy_poller_mutex;
std::condition_variable g_legacy_poller_cv;

namespace LegacyWebSocketCompat {

bool startLegacyPolling() {
    bool expected = false;
    if (!g_legacy_input_poll_running.compare_exchange_strong(expected, true)) {
        LOG_WARNING(ErrorUtils::ErrorCategory::INPUT, "Legacy polling already running");
        return true;
    }

    LOG_INFO(ErrorUtils::ErrorCategory::INPUT, "Starting legacy WebSocket polling threads");

    try {
        // Start keyboard poller thread
        g_legacy_kb_poller = std::thread(legacyKeyboardPollingLoop);

        // Start mouse poller thread
        g_legacy_mouse_poller = std::thread(legacyMousePollingLoop);

        LOG_INFO(ErrorUtils::ErrorCategory::INPUT, "Legacy WebSocket polling threads started successfully");
        return true;

    } catch (const std::exception& e) {
        LOG_SYSTEM_ERROR("Exception starting legacy polling threads: " + std::string(e.what()));
        g_legacy_input_poll_running.store(false);
        return false;
    }
}

void stopLegacyPolling() {
    bool expected = true;
    if (!g_legacy_input_poll_running.compare_exchange_strong(expected, false)) {
        return;
    }

    LOG_INFO(ErrorUtils::ErrorCategory::INPUT, "Stopping legacy WebSocket polling threads");

    // Notify condition variables to wake up threads
    {
        std::lock_guard<std::mutex> lock(g_legacy_poller_mutex);
        g_legacy_poller_cv.notify_all();
    }

    // Join threads with timeout
    auto joinWithTimeout = [](std::thread& thread, const std::string& name) {
        if (thread.joinable()) {
            LOG_INFO(ErrorUtils::ErrorCategory::INPUT, "Waiting for " + name + " thread to join");

            // Use a timeout to avoid hanging
            std::thread joinThread([&thread, name]() {
                if (thread.joinable()) {
                    thread.join();
                    LOG_INFO(ErrorUtils::ErrorCategory::INPUT, name + " thread joined successfully");
                }
            });

            if (joinThread.joinable()) {
                joinThread.detach(); // Let it run in background to avoid blocking
            }
        }
    };

    joinWithTimeout(g_legacy_kb_poller, "legacy keyboard poller");
    joinWithTimeout(g_legacy_mouse_poller, "legacy mouse poller");

    LOG_INFO(ErrorUtils::ErrorCategory::INPUT, "Legacy WebSocket polling threads stopped");
}

bool isLegacyPollingRunning() {
    return g_legacy_input_poll_running.load();
}

void legacyKeyboardPollingLoop() {
    LOG_INFO(ErrorUtils::ErrorCategory::INPUT, "Legacy keyboard polling thread started");

    while (g_legacy_input_poll_running.load() && !ShutdownManager::IsShutdown()) {
        try {
            std::string msg = WebRTCWrapper::getDataChannelMessageString();
            if (!msg.empty()) {
                enqueueKeyboardMessage(msg);
            } else {
                // Wait for messages with timeout
                std::unique_lock<std::mutex> lock(g_legacy_poller_mutex);
                g_legacy_poller_cv.wait_for(lock, std::chrono::milliseconds(100), []() {
                    return !g_legacy_input_poll_running.load() || ShutdownManager::IsShutdown();
                });
            }
        } catch (const std::exception& e) {
            LOG_INPUT_ERROR("Exception in legacy keyboard polling: " + std::string(e.what()), "");
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }

    LOG_INFO(ErrorUtils::ErrorCategory::INPUT, "Legacy keyboard polling thread stopped");
}

void legacyMousePollingLoop() {
    LOG_INFO(ErrorUtils::ErrorCategory::INPUT, "Legacy mouse polling thread started");

    while (g_legacy_input_poll_running.load() && !ShutdownManager::IsShutdown()) {
        try {
            // Use RAII wrapper for safe memory management
            auto msgWrapper = WebRTCWrapper::getMouseChannelMessageSafe();
            if (msgWrapper) {
                std::string msg = msgWrapper; // Implicit conversion
                enqueueMouseMessage(msg);
            } else {
                // Wait for messages with timeout
                std::unique_lock<std::mutex> lock(g_legacy_poller_mutex);
                g_legacy_poller_cv.wait_for(lock, std::chrono::milliseconds(100), []() {
                    return !g_legacy_input_poll_running.load() || ShutdownManager::IsShutdown();
                });
            }
        } catch (const std::exception& e) {
            LOG_INPUT_ERROR("Exception in legacy mouse polling: " + std::string(e.what()), "");
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }

    LOG_INFO(ErrorUtils::ErrorCategory::INPUT, "Legacy mouse polling thread stopped");
}

} // namespace LegacyWebSocketCompat

#else // ENABLE_LEGACY_WEBSOCKET not defined

// Empty implementation when legacy WebSocket is disabled at compile time
// All functions are implemented as inline stubs in the header file

#endif // ENABLE_LEGACY_WEBSOCKET
