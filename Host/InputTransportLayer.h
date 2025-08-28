#pragma once
#include <string>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <atomic>
#include <functional>
#include <memory>

#include "InputConfig.h"
#include "WebRTCWrapper.h"
#include "ErrorUtils.h"

namespace InputTransportLayer {

/**
 * @brief Input message structure representing a single input event
 */
struct InputMessage {
    std::string type;           // Message type (e.g., "keydown", "mousemove")
    std::string data;           // Raw JSON data from client
    uint64_t sequenceId = 0;    // Sequence ID for ordering (if supported)
    uint64_t timestamp = 0;     // Timestamp when message was received

    InputMessage() = default;
    InputMessage(std::string msgType, std::string msgData, uint64_t seq = 0, uint64_t ts = 0)
        : type(std::move(msgType)), data(std::move(msgData)), sequenceId(seq), timestamp(ts) {}
};

/**
 * @brief Transport statistics for monitoring and debugging
 */
struct TransportStats {
    uint64_t messagesReceived = 0;
    uint64_t messagesProcessed = 0;
    uint64_t messagesDropped = 0;
    uint64_t pionMessagesReceived = 0;
    uint64_t websocketMessagesReceived = 0;
    uint64_t queueSize = 0;
    uint64_t maxQueueSize = 0;
    uint64_t sequenceGapsDetected = 0;

    void reset() {
        messagesReceived = 0;
        messagesProcessed = 0;
        messagesDropped = 0;
        pionMessagesReceived = 0;
        websocketMessagesReceived = 0;
        maxQueueSize = std::max(maxQueueSize, queueSize);
        sequenceGapsDetected = 0;
    }

    std::string toString() const;
};

/**
 * @brief Input transport layer - unified interface for all input message handling
 *
 * This layer consolidates input from multiple transport mechanisms:
 * - Pion WebRTC data channels (primary)
 * - Legacy WebSocket (if enabled in config)
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

    /**
     * @brief Initialize the transport layer
     * @param handler Callback function to process received messages
     * @return true if initialization successful, false otherwise
     */
    bool initialize(MessageHandler handler);

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
    const TransportStats& getStats() const;

    /**
     * @brief Reset transport statistics
     */
    void resetStats();

    /**
     * @brief Get the current queue size
     * @return Number of pending messages
     */
    size_t getQueueSize() const;

    /**
     * @brief Force processing of pending messages
     * @return Number of messages processed
     */
    size_t processPendingMessages();

    /**
     * @brief Check if a specific transport is enabled
     * @param transportName Name of transport ("pion" or "websocket")
     * @return true if enabled, false otherwise
     */
    bool isTransportEnabled(const std::string& transportName) const;

private:
    // Configuration
    const InputConfig::InputConfiguration& config = InputConfig::globalInputConfig;

    // Message handling
    MessageHandler messageHandler;
    std::queue<InputMessage> messageQueue;
    mutable std::mutex queueMutex;
    std::condition_variable queueCondition;

    // Processing thread
    std::thread processingThread;
    std::atomic<bool> running{false};
    std::atomic<bool> shouldStop{false};

    // Transport threads
    std::thread pionThread;
    std::thread websocketThread;
    std::atomic<bool> pionRunning{false};
    std::atomic<bool> websocketRunning{false};

    // Statistics
    mutable std::mutex statsMutex;
    TransportStats stats;

    // Sequence tracking for gap detection
    uint64_t lastSequenceId = 0;
    bool sequenceTrackingEnabled = false;

    // Private methods
    void pionMessageLoop();
    void websocketMessageLoop();
    void processingLoop();
    void enqueueMessage(InputMessage&& message);
    void updateStatsQueueSize();
    void detectSequenceGap(uint64_t sequenceId);
    bool shouldProcessMessage(const InputMessage& message);
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
bool initializeGlobalTransport(Layer::MessageHandler handler);

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

} // namespace InputTransportLayer
