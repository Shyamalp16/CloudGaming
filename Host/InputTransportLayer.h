#pragma once
#include <string>
#include <deque>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <atomic>
#include <functional>
#include <memory>

#include "InputConfig.h"
#include "WebRTCWrapper.h"
#include "ErrorUtils.h"
#include "InputSchema.h"

namespace InputTransportLayer {

/**
 * @brief Input message structure representing a single input event
 */
struct InputMessage {
    std::string type;           // Message type (e.g., "keydown", "mousemove")
    std::string data;           // Raw JSON data from client
    uint64_t timestamp = 0;     // Timestamp when message was received
    std::string eventType;      // Strictly validated payload event type

    InputMessage() = default;
    InputMessage(std::string msgType, std::string msgData, uint64_t ts = 0)
        : type(std::move(msgType)), data(std::move(msgData)), timestamp(ts) {}
};

/**
 * @brief Transport statistics for monitoring and debugging
 */
struct TransportStats {
    uint64_t messagesReceived = 0;
    uint64_t messagesProcessed = 0;
    uint64_t messagesDropped = 0;
    uint64_t pionMessagesReceived = 0;
    uint64_t queueSize = 0;
    uint64_t maxQueueSize = 0;

    std::string toString() const;
};

/**
 * @brief Input transport layer - unified interface for all input message handling
 *
 * This layer receives input from Pion WebRTC data channels.
 *
 * It provides a single, well-documented path for input ingestion and
 * separates transport concerns from processing logic.
 */
class Layer {
public:
    /**
     * @brief Message handler callback type
     */
    using MessageHandler = std::function<void(const InputMessage&)>;
    using ResetHandler = std::function<void(const std::string&)>;

    /**
     * @brief Initialize the transport layer
     * @param handler Callback function to process received messages
     * @return true if initialization successful, false otherwise
     */
    bool initialize(MessageHandler handler, ResetHandler resetHandler);

    /**
     * @brief Start the transport layer (begin processing messages)
     * @return true if started successfully, false otherwise
     */
    bool start();

    /**
     * @brief Stop the transport layer and clean up resources
     */
    void stop();

    /**
     * @brief Check if the transport layer is running
     * @return true if running, false otherwise
     */
    bool isRunning() const;

    /**
     * @brief Get current transport statistics
     * @return Reference to current statistics
     */
    TransportStats getStats() const;
    void authorizeSession(const std::string& sessionId);
    void clearAuthorizedSession(const std::string& reason);

private:
    // Configuration
    const InputConfig::InputConfiguration& config = InputConfig::globalInputConfig;

    // Message handling
    MessageHandler messageHandler;
    ResetHandler resetHandler;
    std::deque<InputMessage> messageQueue;
    mutable std::mutex queueMutex;
    std::condition_variable queueCondition;

    // Processing thread
    std::thread processingThread;
    std::atomic<bool> running{false};
    std::atomic<bool> shouldStop{false};

    // Transport threads
    std::thread pionThread;
    std::atomic<bool> pionRunning{false};

    // Statistics
    mutable std::mutex statsMutex;
    TransportStats stats;
    mutable std::mutex sessionMutex;
    std::string authorizedSessionId;

    // Private methods
    void pionMessageLoop();
    void processingLoop();
    void enqueueMessage(InputMessage&& message);
    void updateStatsQueueSize();
    void logTransportEvent(const std::string& event, const std::string& details = "");
};

/**
 * @brief Global transport layer instance
 */
extern std::unique_ptr<Layer> globalTransportLayer;

/**
 * @brief Initialize the global transport layer
 * @param handler Message processing callback
 * @return true if initialization successful, false otherwise
 */
bool initializeGlobalTransport(Layer::MessageHandler handler, Layer::ResetHandler resetHandler);

/**
 * @brief Start the global transport layer
 * @return true if started successfully, false otherwise
 */
bool startGlobalTransport();

/**
 * @brief Stop the global transport layer
 */
void stopGlobalTransport();

/**
 * @brief Get the global transport layer instance
 * @return Pointer to global instance (nullptr if not initialized)
 */
Layer* getGlobalTransport();
void authorizeSession(const std::string& sessionId);
void clearAuthorizedSession(const std::string& reason);

} // namespace InputTransportLayer
