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
    ss << ", queue:" << queueSize;
    ss << ", maxQueue:" << maxQueueSize;
    ss << "}";
    return ss.str();
}

// InputTransportLayer implementation
bool Layer::initialize(MessageHandler handler, ResetHandler reset) {
    if (running.load()) {
        LOG_WARNING(ErrorUtils::ErrorCategory::INPUT, "Transport layer already running");
        return false;
    }

    messageHandler = std::move(handler);
    resetHandler = std::move(reset);
    if (!messageHandler) {
        LOG_SYSTEM_ERROR("Invalid message handler provided");
        return false;
    }

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

    pionRunning.store(true);
    pionThread = std::thread(&Layer::pionMessageLoop, this);
    logTransportEvent("pion_started", "Pion data channel transport started");

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

    // Notify condition variable to wake up waiting threads
    {
        std::lock_guard<std::mutex> lock(queueMutex);
        queueCondition.notify_all();
    }

    // Join transport threads
    if (pionThread.joinable()) {
        pionThread.join();
    }

    // Stop processing thread
    running.store(false);
    if (processingThread.joinable()) {
        processingThread.join();
    }

    if (resetHandler) resetHandler("transport_stop");

    // Clear message queue
    {
        std::lock_guard<std::mutex> lock(queueMutex);
        messageQueue.clear();
    }

    logTransportEvent("stopped", "Transport layer stopped and cleaned up");
}

bool Layer::isRunning() const {
    return running.load();
}

TransportStats Layer::getStats() const {
    std::lock_guard<std::mutex> lock(statsMutex);
    return stats;
}

void Layer::authorizeSession(const std::string& sessionId) {
    std::lock_guard<std::mutex> lock(sessionMutex);
    authorizedSessionId = sessionId;
}

void Layer::clearAuthorizedSession(const std::string& reason) {
    {
        std::lock_guard<std::mutex> lock(sessionMutex);
        authorizedSessionId.clear();
    }
    if (resetHandler) resetHandler(reason);
}

// Private methods
void Layer::pionMessageLoop() {
    logTransportEvent("pion_loop_started", "Pion message loop starting");

    while (pionRunning.load() && !shouldStop.load()) {
        try {
            uint64_t receivedCount = 0;
            uint64_t timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();

            // Drain the keyboard queue completely before sleeping so bursts do
            // not accumulate one millisecond of latency per event.
            for (;;) {
                std::string kbMsg = WebRTCWrapper::getDataChannelMessageString();
                if (kbMsg.empty()) break;
                InputMessage message("pion_data", std::move(kbMsg), timestamp);
                enqueueMessage(std::move(message));
                receivedCount++;
            }

            for (;;) {
                std::string mouseMsg = WebRTCWrapper::getMouseChannelMessageString();
                if (mouseMsg.empty()) break;
                InputMessage message("pion_data", std::move(mouseMsg), timestamp);
                enqueueMessage(std::move(message));
                receivedCount++;
            }

            if (receivedCount > 0) {
                std::lock_guard<std::mutex> lock(statsMutex);
                stats.pionMessagesReceived += receivedCount;
            } else {
                // Wait for new messages with shorter timeout for responsiveness
                std::unique_lock<std::mutex> lock(queueMutex);
                queueCondition.wait_for(lock, std::chrono::milliseconds(1),
                    [this]() { return shouldStop.load(); });
            }
        } catch (const std::exception& e) {
            LOG_INPUT_ERROR("Exception in Pion message loop: " + std::string(e.what()), "");
            std::this_thread::yield();
        }
    }

    logTransportEvent("pion_loop_stopped", "Pion message loop stopped");
}

void Layer::processingLoop() {
    logTransportEvent("processing_loop_started", "Processing loop starting");

    while (running.load() && !shouldStop.load()) {
        try {
            std::unique_lock<std::mutex> lock(queueMutex);

            // Notify-first waiting eliminates fixed 1ms wakeups while preserving responsiveness.
            queueCondition.wait(lock, [this]() {
                return !messageQueue.empty() || shouldStop.load();
            });

            // Process all pending messages
            size_t processed = 0;
            while (!messageQueue.empty() && !shouldStop.load()) {
                InputMessage message = std::move(messageQueue.front());
                messageQueue.pop_front();

                lock.unlock(); // Unlock while processing to allow new messages

                // Fast-path message processing with minimal validation
                if (message.type.empty()) {
                    // Skip invalid messages without full validation
                    lock.lock();
                    continue;
                }

                if (messageHandler) {
                    messageHandler(message);
                    processed++;
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
            // Yield instead of sleep to maintain responsiveness in critical input path
            std::this_thread::yield();
        }
    }

    logTransportEvent("processing_loop_stopped", "Processing loop stopped");
}

void Layer::enqueueMessage(InputMessage&& message) {
    auto validation = InputSchema::Validate(message.data);
    if (!validation.valid) {
        std::lock_guard<std::mutex> statsLock(statsMutex);
        stats.messagesDropped++;
        LOG_WARNING(ErrorUtils::ErrorCategory::INPUT, "Rejected input payload: " + validation.error);
        return;
    }
    message.eventType = validation.eventType;
	{
		const std::string suppliedSession = validation.value.value("sessionId", std::string{});
		std::lock_guard<std::mutex> sessionLock(sessionMutex);
		if (authorizedSessionId.empty() || suppliedSession != authorizedSessionId) {
			std::lock_guard<std::mutex> statsLock(statsMutex);
			stats.messagesDropped++;
			return;
		}
	}

    bool overflowed = false;
    uint64_t dropped = 0;
    {
        std::lock_guard<std::mutex> lock(queueMutex);
        if (InputSchema::IsMouseMove(message.eventType) && !messageQueue.empty() &&
            InputSchema::IsMouseMove(messageQueue.back().eventType)) {
            messageQueue.back() = std::move(message);
        } else if (messageQueue.size() >= static_cast<size_t>((std::max)(1, config.maxPendingMessages))) {
            overflowed = true;
            dropped = messageQueue.size();
            messageQueue.clear();
            InputMessage reset("pion_data",
                "{\"type\":\"input_reset\",\"reason\":\"transport_queue_overflow\"}", message.timestamp);
            reset.eventType = "input_reset";
            messageQueue.push_back(std::move(reset));
            if (InputSchema::IsReleaseEvent(message.eventType)) messageQueue.push_back(std::move(message));
            else ++dropped;
        } else {
            messageQueue.push_back(std::move(message));
        }
        queueCondition.notify_one();
    }

    if (overflowed && resetHandler) resetHandler("transport_queue_overflow");
    updateStatsQueueSize();

    {
        std::lock_guard<std::mutex> statsLock(statsMutex);
        stats.messagesReceived++;
        stats.messagesDropped += dropped;
    }
}

void Layer::updateStatsQueueSize() {
    std::lock_guard<std::mutex> lock(statsMutex);
    stats.queueSize = messageQueue.size();
    stats.maxQueueSize = (std::max)(stats.maxQueueSize, stats.queueSize);
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
bool initializeGlobalTransport(Layer::MessageHandler handler, Layer::ResetHandler resetHandler) {
    if (globalTransportLayer) {
        LOG_WARNING(ErrorUtils::ErrorCategory::INPUT, "Global transport layer already initialized");
        return true;
    }

    globalTransportLayer = std::make_unique<Layer>();
    if (!globalTransportLayer->initialize(std::move(handler), std::move(resetHandler))) {
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

void authorizeSession(const std::string& sessionId) {
    if (globalTransportLayer) globalTransportLayer->authorizeSession(sessionId);
}

void clearAuthorizedSession(const std::string& reason) {
    if (globalTransportLayer) globalTransportLayer->clearAuthorizedSession(reason);
}

} // namespace InputTransportLayer
