#include "InputIntegrationLayer.h"
#include "KeyInputHandler.h"
#include "MouseInputHandler.h"
#include <iostream>
#include <sstream>
#include <thread>
#include <chrono>
#include "ShutdownManager.h"

namespace InputIntegrationLayer {

// Global integration configuration
IntegrationConfig globalIntegrationConfig;

// Global instances
static std::unique_ptr<InputTransportLayer::Layer> transportLayer;
static std::unique_ptr<InputStateManager::Manager> stateManager;
static std::thread statsReportingThread;
static std::atomic<bool> integrationRunning{false};
static std::atomic<bool> statsReportingRunning{false};

// Use handler namespaces directly

/**
 * @brief Message handler callback for new architecture
 * @param eventType The type of input event
 * @param eventData JSON string containing event data
 */
static void newArchitectureMessageHandler(const std::string& eventType, const std::string& eventData) {
    try {
        // Forward to existing handlers for compatibility
        if (eventType == "keydown" || eventType == "keyup") {
            KeyInputHandler::enqueueMessage(eventData);
        } else if (eventType == "mousedown" || eventType == "mouseup" ||
                   eventType == "mousemove" || eventType == "wheel") {
            MouseInputHandler::enqueueMessage(eventData);
        } else {
            LOG_WARNING(ErrorUtils::ErrorCategory::INPUT,
                       "Unknown event type in new architecture: " + eventType);
        }
    } catch (const std::exception& e) {
        LOG_INPUT_ERROR("Exception in new architecture message handler: " + std::string(e.what()), eventData);
    }
}

/**
 * @brief Statistics reporting loop
 */
void statsReportingLoop() {
    LOG_INFO(ErrorUtils::ErrorCategory::INPUT, "Statistics reporting thread started");

    while (statsReportingRunning.load() && !ShutdownManager::IsShutdown()) {
        try {
            // Sleep for reporting interval
            std::this_thread::sleep_for(globalIntegrationConfig.statsReportInterval);

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
        if (globalIntegrationConfig.enableNewArchitecture) {
            // Initialize new architecture components
            if (InputConfig::globalInputConfig.usePionDataChannels) {
                // Wrap the 2-arg event callback into a transport message handler
                auto handler = [](const InputTransportLayer::InputMessage& msg) {
                    try {
                        // Extract type from JSON for routing
                        nlohmann::json eventData = nlohmann::json::parse(msg.data);
                        std::string eventType = eventData.value("type", std::string());
                        newArchitectureMessageHandler(eventType, msg.data);
                        // Also forward to state manager if available
                        if (auto* sm = InputStateManager::getGlobalStateManager()) {
                            sm->processInputMessage(msg);
                        }
                    } catch (const std::exception& e) {
                        LOG_INPUT_ERROR("Transport handler exception: " + std::string(e.what()), msg.data);
                    }
                };

                if (!InputTransportLayer::initializeGlobalTransport(handler)) {
                    LOG_SYSTEM_ERROR("Failed to initialize global transport layer");
                    return false;
                }

                LOG_INFO(ErrorUtils::ErrorCategory::INPUT, "Transport layer initialized successfully");
            }

            if (!InputStateManager::initializeGlobalStateManager(newArchitectureMessageHandler)) {
                LOG_SYSTEM_ERROR("Failed to initialize global state manager");
                return false;
            }

            LOG_INFO(ErrorUtils::ErrorCategory::INPUT, "State manager initialized successfully");
        }

        if (globalIntegrationConfig.enableLegacyCompatibility) {
            LOG_INFO(ErrorUtils::ErrorCategory::INPUT, "Legacy compatibility mode enabled - legacy WebSocket polling will be available");
        } else {
            LOG_INFO(ErrorUtils::ErrorCategory::INPUT, "Legacy compatibility mode disabled - using new architecture only");
        }

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
        if (globalIntegrationConfig.enableNewArchitecture) {
            // Start new architecture components
            if (InputConfig::globalInputConfig.usePionDataChannels) {
                if (!InputTransportLayer::startGlobalTransport()) {
                    LOG_SYSTEM_ERROR("Failed to start global transport layer");
                    return false;
                }
            }

            if (!InputStateManager::startGlobalStateManager()) {
                LOG_SYSTEM_ERROR("Failed to start global state manager");
                return false;
            }
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
    if (statsReportingThread.joinable()) {
        statsReportingThread.join();
    }

    // Stop new architecture components
    if (globalIntegrationConfig.enableNewArchitecture) {
        InputTransportLayer::stopGlobalTransport();
        InputStateManager::stopGlobalStateManager();
    }

    integrationRunning.store(false);
    LOG_INFO(ErrorUtils::ErrorCategory::INPUT, "Input integration layer stopped");
}

bool isRunning() {
    return integrationRunning.load();
}

void processInputMessage(const InputTransportLayer::InputMessage& message) {
    try {
        if (globalIntegrationConfig.enableNewArchitecture && stateManager) {
            // Process through new state manager
            stateManager->processInputMessage(message);
        } else if (globalIntegrationConfig.enableLegacyCompatibility) {
            // Fall back to legacy processing
            if (message.type == "pion_data") {
                MouseInputHandler::enqueueMessage(message.data);
            }
        } else {
            LOG_WARNING(ErrorUtils::ErrorCategory::INPUT,
                       "No processing path available for message type: " + message.type);
        }
    } catch (const std::exception& e) {
        LOG_INPUT_ERROR("Exception processing input message: " + std::string(e.what()), message.data);
    }
}

std::string getStatistics() {
    std::stringstream ss;
    ss << "=== Input Integration Layer Statistics ===\n";
    ss << "Integration Status: " << (isRunning() ? "RUNNING" : "STOPPED") << "\n";
    ss << "New Architecture: " << (globalIntegrationConfig.enableNewArchitecture ? "ENABLED" : "DISABLED") << "\n";
    ss << "Legacy Compatibility: " << (globalIntegrationConfig.enableLegacyCompatibility ? "ENABLED" : "DISABLED") << "\n";

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

void updateConfiguration(const IntegrationConfig& config) {
    globalIntegrationConfig = config;
    LOG_INFO(ErrorUtils::ErrorCategory::INPUT,
            "Integration configuration updated - New Architecture: " +
            std::string(config.enableNewArchitecture ? "ENABLED" : "DISABLED"));
}

void emergencyStop(const std::string& reason) {
    LOG_SYSTEM_ERROR("Emergency stop requested: " + reason);

    // Emergency release all keys if state manager is available
    if (stateManager) {
        stateManager->emergencyReleaseAllKeys(reason);
    }

    // Force stop all components
    stop();
}

bool isNewArchitectureEnabled() {
    return globalIntegrationConfig.enableNewArchitecture &&
           transportLayer != nullptr &&
           stateManager != nullptr;
}

InputTransportLayer::Layer* getTransportLayer() {
    return transportLayer.get();
}

InputStateManager::Manager* getStateManager() {
    return stateManager.get();
}

} // namespace InputIntegrationLayer
