#include "InputTransportLayer.h"
#include <iostream>
#include <chrono>
#include <sstream>
#include <algorithm>

namespace InputTransportLayer {

// Global instance
std::unique_ptr<Layer> globalTransportLayer;

// TransportStats implementation
std::string TransportStats::toString() const {
    std::stringstream ss;
    ss << "TransportStats{";
    ss << "received:" << messagesReceived;
    ss << ", processed:" << messagesProcessed;
    ss << ", dropped:" << messagesDropped;
    ss << ", pion:" << pionMessagesReceived;
    ss << ", ws:" << websocketMessagesReceived;
    ss << ", queue:" << queueSize;
    ss << ", maxQueue:" << maxQueueSize;
    ss << ", seqGaps:" << sequenceGapsDetected;
    ss << "}";
    return ss.str();
}

// InputTransportLayer implementation
bool Layer::initialize(MessageHandler handler) {
    if (running.load()) {
        LOG_WARNING(ErrorUtils::ErrorCategory::INPUT, "Transport layer already running");
        return false;
    }

    messageHandler = std::move(handler);
    if (!messageHandler) {
        LOG_SYSTEM_ERROR("Invalid message handler provided");
        return false;
    }

    sequenceTrackingEnabled = config.enableSequenceRecovery;
    logTransportEvent("initialized", "Transport layer initialized successfully");

    return true;
}

bool Layer::start() {
    if (running.load()) {
        LOG_WARNING(ErrorUtils::ErrorCategory::INPUT, "Transport layer already running");
        return true;
    }

    if (!messageHandler) {
        LOG_SYSTEM_ERROR("Transport layer not initialized");
        return false;
    }

    shouldStop.store(false);
    running.store(true);

    // Start processing thread
    processingThread = std::thread(&Layer::processingLoop, this);

    // Start transport threads based on configuration
    if (config.usePionDataChannels) {
        pionRunning.store(true);
        pionThread = std::thread(&Layer::pionMessageLoop, this);
        logTransportEvent("pion_started", "Pion data channel transport started");
    }

    if (config.enableLegacyWebSocket) {
        websocketRunning.store(true);
        websocketThread = std::thread(&Layer::websocketMessageLoop, this);
        logTransportEvent("websocket_started", "Legacy WebSocket transport started");
    }

    logTransportEvent("started", "Transport layer started successfully");
    return true;
}

void Layer::stop() {
    if (!running.load()) {
        return;
    }

    logTransportEvent("stopping", "Stopping transport layer...");

    shouldStop.store(true);

    // Stop transport threads
    pionRunning.store(false);
    websocketRunning.store(false);

    // Notify condition variable to wake up waiting threads
    {
        std::lock_guard<std::mutex> lock(queueMutex);
        queueCondition.notify_all();
    }

    // Join transport threads
    if (pionThread.joinable()) {
        pionThread.join();
    }
    if (websocketThread.joinable()) {
        websocketThread.join();
    }

    // Stop processing thread
    running.store(false);
    if (processingThread.joinable()) {
        processingThread.join();
    }

    // Clear message queue
    {
        std::lock_guard<std::mutex> lock(queueMutex);
        std::queue<InputMessage> emptyQueue;
        std::swap(messageQueue, emptyQueue);
    }

    logTransportEvent("stopped", "Transport layer stopped and cleaned up");
}

bool Layer::isRunning() const {
    return running.load();
}

const TransportStats& Layer::getStats() const {
    std::lock_guard<std::mutex> lock(statsMutex);
    return stats;
}

void Layer::resetStats() {
    std::lock_guard<std::mutex> lock(statsMutex);
    stats.reset();
    logTransportEvent("stats_reset", "Transport statistics reset");
}

size_t Layer::getQueueSize() const {
    std::lock_guard<std::mutex> lock(queueMutex);
    return messageQueue.size();
}

size_t Layer::processPendingMessages() {
    std::lock_guard<std::mutex> lock(queueMutex);
    size_t processed = 0;

    while (!messageQueue.empty() && !shouldStop.load()) {
        InputMessage message = std::move(messageQueue.front());
        messageQueue.pop();

        if (messageHandler) {
            try {
                messageHandler(message);
                processed++;
            } catch (const std::exception& e) {
                LOG_INPUT_ERROR("Exception in message handler: " + std::string(e.what()), message.data);
            }
        }
    }

    updateStatsQueueSize();
    return processed;
}

bool Layer::isTransportEnabled(const std::string& transportName) const {
    if (transportName == "pion") {
        return config.usePionDataChannels;
    } else if (transportName == "websocket") {
        return config.enableLegacyWebSocket;
    }
    return false;
}

// Private methods
void Layer::pionMessageLoop() {
    logTransportEvent("pion_loop_started", "Pion message loop starting");

    while (pionRunning.load() && !shouldStop.load()) {
        try {
            // Use the safe RAII wrapper for message retrieval
            auto msgWrapper = WebRTCWrapper::getMouseChannelMessageSafe();
            if (msgWrapper) {
                uint64_t timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::system_clock::now().time_since_epoch()).count();

                InputMessage message("pion_data", msgWrapper.toString(), 0, timestamp);
                enqueueMessage(std::move(message));

                {
                    std::lock_guard<std::mutex> lock(statsMutex);
                    stats.pionMessagesReceived++;
                }
            } else {
                // Wait for new messages with shorter timeout for responsiveness
                std::unique_lock<std::mutex> lock(queueMutex);
                queueCondition.wait_for(lock, std::chrono::milliseconds(5),
                    [this]() { return shouldStop.load(); });
            }
        } catch (const std::exception& e) {
            LOG_INPUT_ERROR("Exception in Pion message loop: " + std::string(e.what()), "");
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }

    logTransportEvent("pion_loop_stopped", "Pion message loop stopped");
}

void Layer::websocketMessageLoop() {
    logTransportEvent("websocket_loop_started", "WebSocket message loop starting");

    while (websocketRunning.load() && !shouldStop.load()) {
        try {
            // TODO: Implement legacy WebSocket message retrieval
            // For now, just sleep to avoid busy waiting
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        } catch (const std::exception& e) {
            LOG_INPUT_ERROR("Exception in WebSocket message loop: " + std::string(e.what()), "");
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }

    logTransportEvent("websocket_loop_stopped", "WebSocket message loop stopped");
}

void Layer::processingLoop() {
    logTransportEvent("processing_loop_started", "Processing loop starting");

    while (running.load() && !shouldStop.load()) {
        try {
            std::unique_lock<std::mutex> lock(queueMutex);

            // Wait for messages with shorter timeout for responsiveness
            queueCondition.wait_for(lock, std::chrono::milliseconds(1), [this]() {
                return !messageQueue.empty() || shouldStop.load();
            });

            // Process all pending messages
            size_t processed = 0;
            while (!messageQueue.empty() && !shouldStop.load()) {
                InputMessage message = std::move(messageQueue.front());
                messageQueue.pop();

                lock.unlock(); // Unlock while processing to allow new messages

                // Fast-path message processing with minimal validation
                if (message.type.empty()) {
                    // Skip invalid messages without full validation
                    continue;
                }

                if (messageHandler) {
                    try {
                        messageHandler(message);
                        processed++;
                    } catch (const std::exception& e) {
                        LOG_INPUT_ERROR("Exception in message handler: " + std::string(e.what()), message.data);
                    }
                }

                lock.lock(); // Re-lock for next iteration
            }

            // Update statistics
            {
                std::lock_guard<std::mutex> statsLock(statsMutex);
                stats.messagesProcessed += processed;
            }

            updateStatsQueueSize();

        } catch (const std::exception& e) {
            LOG_INPUT_ERROR("Exception in processing loop: " + std::string(e.what()), "");
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }

    logTransportEvent("processing_loop_stopped", "Processing loop stopped");
}

void Layer::enqueueMessage(InputMessage&& message) {
    std::lock_guard<std::mutex> lock(queueMutex);

    // Check queue size limit
    if (messageQueue.size() >= config.maxPendingMessages) {
        std::lock_guard<std::mutex> statsLock(statsMutex);
        stats.messagesDropped++;
        LOG_WARNING(ErrorUtils::ErrorCategory::INPUT,
                   "Message queue full, dropping message. Queue size: " + std::to_string(messageQueue.size()));
        return;
    }

    // Detect sequence gaps if enabled
    if (sequenceTrackingEnabled && message.sequenceId > 0) {
        detectSequenceGap(message.sequenceId);
    }

    messageQueue.push(std::move(message));
    updateStatsQueueSize();

    // Notify processing thread
    queueCondition.notify_one();

    {
        std::lock_guard<std::mutex> statsLock(statsMutex);
        stats.messagesReceived++;
    }
}

void Layer::updateStatsQueueSize() {
    std::lock_guard<std::mutex> lock(statsMutex);
    stats.queueSize = messageQueue.size();
    stats.maxQueueSize = (std::max)(stats.maxQueueSize, stats.queueSize);
}

void Layer::detectSequenceGap(uint64_t sequenceId) {
    if (lastSequenceId > 0 && sequenceId > lastSequenceId + 1) {
        uint64_t gapSize = sequenceId - lastSequenceId - 1;
        std::lock_guard<std::mutex> lock(statsMutex);
        stats.sequenceGapsDetected++;

        LOG_WARNING(ErrorUtils::ErrorCategory::INPUT,
                   "Sequence gap detected: expected " + std::to_string(lastSequenceId + 1) +
                   ", got " + std::to_string(sequenceId) + " (gap: " + std::to_string(gapSize) + ")");
    }
    lastSequenceId = sequenceId;
}

bool Layer::shouldProcessMessage(const InputMessage& message) {
    // Basic validation - can be extended with more sophisticated filtering
    if (message.type.empty()) {
        LOG_WARNING(ErrorUtils::ErrorCategory::INPUT, "Dropping message with empty type");
        return false;
    }

    // Rate limiting could be added here based on config.maxInjectHz

    return true;
}

void Layer::logTransportEvent(const std::string& event, const std::string& details) {
    std::string message = "Transport event: " + event;
    if (!details.empty()) {
        message += " - " + details;
    }

    if (config.enableAggregatedLogging) {
        LOG_INFO(ErrorUtils::ErrorCategory::INPUT, message);
    }
}

// Global functions
bool initializeGlobalTransport(Layer::MessageHandler handler) {
    if (globalTransportLayer) {
        LOG_WARNING(ErrorUtils::ErrorCategory::INPUT, "Global transport layer already initialized");
        return true;
    }

    globalTransportLayer = std::make_unique<Layer>();
    if (!globalTransportLayer->initialize(std::move(handler))) {
        LOG_SYSTEM_ERROR("Failed to initialize global transport layer");
        globalTransportLayer.reset();
        return false;
    }

    LOG_INFO(ErrorUtils::ErrorCategory::INPUT, "Global transport layer initialized successfully");
    return true;
}

bool startGlobalTransport() {
    if (!globalTransportLayer) {
        LOG_SYSTEM_ERROR("Global transport layer not initialized");
        return false;
    }

    return globalTransportLayer->start();
}

void stopGlobalTransport() {
    if (globalTransportLayer) {
        globalTransportLayer->stop();
        LOG_INFO(ErrorUtils::ErrorCategory::INPUT, "Global transport layer stopped");
    }
}

Layer* getGlobalTransport() {
    return globalTransportLayer.get();
}

} // namespace InputTransportLayer
