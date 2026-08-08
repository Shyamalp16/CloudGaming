#include "InputIntegrationLayer.h"
#include "InputTransportLayer.h"
#include "InputStateManager.h"
#include "KeyInputHandler.h"
#include "MouseInputHandler.h"
#include <iostream>
#include <sstream>
#include <thread>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include "ShutdownManager.h"
#include "InputConfig.h"

namespace InputIntegrationLayer {

// Global integration configuration
IntegrationConfig globalIntegrationConfig;

// Global instances
static std::thread statsReportingThread;
static std::atomic<bool> integrationRunning{false};
static std::atomic<bool> statsReportingRunning{false};
static std::mutex statsWaitMutex;
static std::condition_variable statsWaitCondition;

// Use handler namespaces directly

/**
 * @brief Dispatch validated input events to their channel-specific handlers.
 * @param eventType The type of input event
 * @param eventData JSON string containing event data
 */
static void inputMessageHandler(const std::string& eventType, const std::string& eventData) {
    try {
        if (eventType == "keydown" || eventType == "keyup" ||
            eventType == "emergency_keyup" || eventType == "stuck_key_recovery") {
            KeyInputHandler::enqueueMessage(eventData);
        } else if (eventType == "mousedown" || eventType == "mouseup" ||
                   eventType == "mousemove" || eventType == "wheel" || eventType == "hwheel") {
            MouseInputHandler::enqueueMessage(eventData);
        } else {
            LOG_WARNING(ErrorUtils::ErrorCategory::INPUT,
                       "Unknown input event type: " + eventType);
        }
    } catch (const std::exception& e) {
        LOG_INPUT_ERROR("Exception in input message handler: " + std::string(e.what()), eventData);
    }
}

/**
 * @brief Statistics reporting loop
 */
void statsReportingLoop() {
    LOG_INFO(ErrorUtils::ErrorCategory::INPUT, "Statistics reporting thread started");

    while (statsReportingRunning.load() && !ShutdownManager::IsShutdown()) {
        try {
            std::unique_lock<std::mutex> lock(statsWaitMutex);
            statsWaitCondition.wait_for(lock, globalIntegrationConfig.statsReportInterval,
                [] { return !statsReportingRunning.load() || ShutdownManager::IsShutdown(); });
            lock.unlock();

            if (statsReportingRunning.load()) {
                std::string stats = getStatistics();
                LOG_INFO(ErrorUtils::ErrorCategory::INPUT, "Input Statistics:\n" + stats);
            }
        } catch (const std::exception& e) {
            LOG_INPUT_ERROR("Exception in statistics reporting loop: " + std::string(e.what()), "");
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
    }

    LOG_INFO(ErrorUtils::ErrorCategory::INPUT, "Statistics reporting thread stopped");
}

bool initialize() {
    if (integrationRunning.load()) {
        LOG_WARNING(ErrorUtils::ErrorCategory::INPUT, "Integration layer already initialized");
        return true;
    }

    LOG_INFO(ErrorUtils::ErrorCategory::INPUT, "Initializing input integration layer...");

    try {
        auto handler = [](const InputTransportLayer::InputMessage& msg) {
            try {
                if (auto* sm = InputStateManager::getGlobalStateManager()) {
                    sm->processInputMessage(msg);
                }
            } catch (const std::exception& e) {
                LOG_INPUT_ERROR("Transport handler exception: " + std::string(e.what()), msg.data);
            }
        };

        auto resetHandler = [](const std::string& reason) { resetAllInput(reason); };
        if (!InputTransportLayer::initializeGlobalTransport(handler, resetHandler)) {
            LOG_SYSTEM_ERROR("Failed to initialize global transport layer");
            return false;
        }
        LOG_INFO(ErrorUtils::ErrorCategory::INPUT, "Transport layer initialized successfully");

        if (!InputStateManager::initializeGlobalStateManager(inputMessageHandler)) {
            LOG_SYSTEM_ERROR("Failed to initialize global state manager");
            return false;
        }
        LOG_INFO(ErrorUtils::ErrorCategory::INPUT, "State manager initialized successfully");

        LOG_INFO(ErrorUtils::ErrorCategory::INPUT, "Input integration layer initialized successfully");
        return true;

    } catch (const std::exception& e) {
        LOG_SYSTEM_ERROR("Exception during integration layer initialization: " + std::string(e.what()));
        return false;
    }
}

bool start() {
    if (integrationRunning.load()) {
        LOG_WARNING(ErrorUtils::ErrorCategory::INPUT, "Integration layer already running");
        return true;
    }

    LOG_INFO(ErrorUtils::ErrorCategory::INPUT, "Starting input integration layer...");

    try {
        if (!InputTransportLayer::startGlobalTransport()) {
            LOG_SYSTEM_ERROR("Failed to start global transport layer");
            return false;
        }
        if (!InputStateManager::startGlobalStateManager()) {
            InputTransportLayer::stopGlobalTransport();
            LOG_SYSTEM_ERROR("Failed to start global state manager");
            return false;
        }

        // Start statistics reporting if enabled
        if (globalIntegrationConfig.enableStatisticsReporting) {
            statsReportingRunning.store(true);
            statsReportingThread = std::thread(statsReportingLoop);
        }

        integrationRunning.store(true);
        LOG_INFO(ErrorUtils::ErrorCategory::INPUT, "Input integration layer started successfully");
        return true;

    } catch (const std::exception& e) {
        LOG_SYSTEM_ERROR("Exception during integration layer startup: " + std::string(e.what()));
        return false;
    }
}

void stop() {
    if (!integrationRunning.load()) {
        return;
    }

    LOG_INFO(ErrorUtils::ErrorCategory::INPUT, "Stopping input integration layer...");

    // Stop statistics reporting
    statsReportingRunning.store(false);
    statsWaitCondition.notify_all();
    if (statsReportingThread.joinable()) {
        statsReportingThread.join();
    }

    InputTransportLayer::stopGlobalTransport();
    InputStateManager::stopGlobalStateManager();

    integrationRunning.store(false);
    LOG_INFO(ErrorUtils::ErrorCategory::INPUT, "Input integration layer stopped");
}

bool isRunning() {
    return integrationRunning.load();
}

std::string getStatistics() {
    std::stringstream ss;
    ss << "=== Input Integration Layer Statistics ===\n";
    ss << "Integration Status: " << (isRunning() ? "RUNNING" : "STOPPED") << "\n";
    if (auto* tl = InputTransportLayer::getGlobalTransport()) {
        auto transportStats = tl->getStats();
        ss << "\n--- Transport Layer ---\n";
        ss << transportStats.toString() << "\n";
    }

    if (auto* sm = InputStateManager::getGlobalStateManager()) {
        auto stateStats = sm->getStats();
        ss << "\n--- State Manager ---\n";
        ss << stateStats.toString() << "\n";
    }

    return ss.str();
}

void resetAllInput(const std::string& reason) {
    if (auto* state = InputStateManager::getGlobalStateManager()) {
        state->emergencyReleaseAllKeys(reason);
    }
    KeyInputHandler::releaseAllKeysEmergency();
    MouseInputHandler::releaseAllButtonsEmergency();
}

} // namespace InputIntegrationLayer
