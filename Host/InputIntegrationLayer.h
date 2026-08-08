#pragma once
#include <string>
#include <chrono>

/**
 * @brief Coordinates the WebRTC input transport and state manager.
 */
namespace InputIntegrationLayer {

/**
 * @brief Integration configuration
 */
struct IntegrationConfig {
    bool enableStatisticsReporting = true;     // Report transport and state statistics
    std::chrono::milliseconds statsReportInterval = std::chrono::milliseconds(60000);
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
 * @brief Get integration statistics
 * @return Formatted statistics string
 */
std::string getStatistics();
void resetAllInput(const std::string& reason);
void authorizeSession(const std::string& sessionId);
void clearAuthorizedSession(const std::string& reason);

} // namespace InputIntegrationLayer
