#pragma once
#include "InputTransportLayer.h"
#include "InputStateManager.h"
#include "InputConfig.h"
#include "ErrorUtils.h"
#include <functional>
#include <memory>
#include <string>
#include <sstream>
#include <chrono>

// Legacy WebSocket compatibility macros
#ifdef ENABLE_LEGACY_WEBSOCKET
    #define LEGACY_WS_AVAILABLE 1
    #define IF_LEGACY_WS(x) x
    #define IF_NOT_LEGACY_WS(x)
#else
    #define LEGACY_WS_AVAILABLE 0
    #define IF_LEGACY_WS(x)
    #define IF_NOT_LEGACY_WS(x) x
#endif

// Configuration-based legacy WebSocket control
#define LEGACY_WEBSOCKET_ENABLED() (InputConfig::globalInputConfig.enableLegacyWebSocket)

/**
 * @brief Input Integration Layer - Bridges new architecture with existing handlers
 *
 * This layer provides a smooth transition from the old direct WebRTC/WebSocket
 * handling to the new unified InputTransportLayer + InputStateManager architecture.
 * It maintains compatibility with existing KeyInputHandler and MouseInputHandler
 * while enabling the new features and reliability improvements.
 */
namespace InputIntegrationLayer {

/**
 * @brief Integration configuration
 */
struct IntegrationConfig {
    bool enableNewArchitecture = true;         // Use new InputTransportLayer + InputStateManager
    bool enableLegacyCompatibility = false;    // Keep old direct WebRTC polling for compatibility
    bool enableStatisticsReporting = true;     // Report transport and state statistics
    std::chrono::milliseconds statsReportInterval = std::chrono::milliseconds(5000);
};

/**
 * @brief Global integration configuration
 */
extern IntegrationConfig globalIntegrationConfig;

/**
 * @brief Initialize the input integration layer
 * @return true if initialization successful, false otherwise
 */
bool initialize();

/**
 * @brief Start the input integration layer
 * @return true if started successfully, false otherwise
 */
bool start();

/**
 * @brief Stop the input integration layer
 */
void stop();

/**
 * @brief Check if the integration layer is running
 * @return true if running, false otherwise
 */
bool isRunning();

/**
 * @brief Process an input message through the integration layer
 * @param message The input message to process
 */
void processInputMessage(const InputTransportLayer::InputMessage& message);

/**
 * @brief Get integration statistics
 * @return Formatted statistics string
 */
std::string getStatistics();

/**
 * @brief Update integration configuration
 * @param config New configuration
 */
void updateConfiguration(const IntegrationConfig& config);

/**
 * @brief Emergency stop all input processing
 * @param reason Reason for emergency stop
 */
void emergencyStop(const std::string& reason = "emergency");

/**
 * @brief Check if new architecture is enabled
 * @return true if new architecture is active, false otherwise
 */
bool isNewArchitectureEnabled();

/**
 * @brief Get reference to global transport layer (if initialized)
 * @return Pointer to transport layer or nullptr
 */
InputTransportLayer::Layer* getTransportLayer();

/**
 * @brief Get reference to global state manager (if initialized)
 * @return Pointer to state manager or nullptr
 */
InputStateManager::Manager* getStateManager();

} // namespace InputIntegrationLayer
