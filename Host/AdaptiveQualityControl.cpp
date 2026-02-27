#include "AdaptiveQualityControl.h"
#include <iostream>
#include <algorithm>
#include <cmath>

// Temporary logging macros (replace with proper logging system)
#define LOG_INFO(msg) std::cout << "[AdaptiveQC] " << msg << std::endl
#define LOG_WARN(msg) std::cout << "[AdaptiveQC] WARNING: " << msg << std::endl
#define LOG_ERROR(msg) std::cout << "[AdaptiveQC] ERROR: " << msg << std::endl

namespace AdaptiveQualityControl {

// Global instance
QualityController globalQualityController;

// WebRTC stats callback implementation
void QualityController::webrtcStatsCallback(double packetLoss, double rtt, double jitter,
                                           uint32_t nackCount, uint32_t pliCount, uint32_t twccCount,
                                           uint32_t pacerQueueLength, uint32_t sendBitrateKbps) {
    if (globalQualityController.isActive()) {
        NetworkStats stats;
        stats.packetLoss = packetLoss;
        stats.rttMs = rtt;
        stats.jitterMs = jitter * 1000.0; // Convert to milliseconds
        stats.nackCount = nackCount;
        stats.pliCount = pliCount;
        stats.twccCount = twccCount;
        stats.pacerQueueLength = pacerQueueLength;
        stats.sendBitrateKbps = sendBitrateKbps;
        stats.lastUpdate = std::chrono::steady_clock::now();

        // FIX: Validate encoder bitrate matches WebRTC send rate
        // If encoder produces significantly more than WebRTC can send, it's wasteful
        // Note: We can't directly access g_currentBitrate here, but we can log a warning
        // The bitrate controller should handle this via RTCP feedback
        if (sendBitrateKbps > 0 && pacerQueueLength > 10) {
            // High queue length suggests encoder is producing faster than WebRTC can send
            LOG_WARN("High pacer queue length (" + std::to_string(pacerQueueLength) + 
                    ") detected. WebRTC send rate: " + std::to_string(sendBitrateKbps) + 
                    " kbps. Consider reducing encoder bitrate if queue persists.");
        }

        globalQualityController.updateNetworkStats(stats);
    }
}

QualityController::QualityController() : isEnabled(false) {
    LOG_INFO("Adaptive Quality Controller initialized");
}

QualityController::~QualityController() {
    disable();
    LOG_INFO("Adaptive Quality Controller destroyed");
}

void QualityController::setConfig(const DroppingConfig& newConfig) {
    std::lock_guard<std::mutex> lock(statsMutex);
    config = newConfig;
    LOG_INFO("Quality control configuration updated");
}

void QualityController::enable() {
    if (!isEnabled.exchange(true)) {
        // Register WebRTC stats callback
        WebRTCWrapper::setWebRTCStatsCallback(webrtcStatsCallback);
        LOG_INFO("Adaptive quality control enabled");
    }
}

void QualityController::disable() {
    if (isEnabled.exchange(false)) {
        // Unregister WebRTC stats callback
        WebRTCWrapper::setWebRTCStatsCallback(nullptr);
        LOG_INFO("Adaptive quality control disabled");
    }
}

NetworkCondition QualityController::assessNetworkCondition() const {
    const auto& stats = currentStats;

    // Critical condition: High RTT + High loss + Long queue
    if (stats.rttMs >= config.rttPoorThreshold &&
        stats.packetLoss >= config.lossPoorThreshold &&
        stats.pacerQueueLength >= config.queuePoorThreshold) {
        return NetworkCondition::Critical;
    }

    // Poor condition: High RTT or high loss or long queue
    if (stats.rttMs >= config.rttPoorThreshold ||
        stats.packetLoss >= config.lossPoorThreshold ||
        stats.pacerQueueLength >= config.queuePoorThreshold) {
        return NetworkCondition::Poor;
    }

    // Fair condition: Moderate RTT or moderate loss or moderate queue
    if (stats.rttMs >= config.rttFairThreshold ||
        stats.packetLoss >= config.lossFairThreshold ||
        stats.pacerQueueLength >= config.queueFairThreshold) {
        return NetworkCondition::Fair;
    }

    // Good condition: Low RTT and low loss and short queue
    if (stats.rttMs <= config.rttGoodThreshold &&
        stats.packetLoss <= config.lossGoodThreshold &&
        stats.pacerQueueLength <= config.queueGoodThreshold) {
        return NetworkCondition::Good;
    }

    // Excellent condition: Very low RTT and very low loss and minimal queue
    if (stats.rttMs <= config.rttExcellentThreshold &&
        stats.packetLoss <= config.lossExcellentThreshold &&
        stats.pacerQueueLength <= config.queueExcellentThreshold) {
        return NetworkCondition::Excellent;
    }

    // Default to Good if conditions don't match extremes
    return NetworkCondition::Good;
}

QualityDecision QualityController::shouldDropFrame() {
    if (!isEnabled.load(std::memory_order_relaxed)) {
        return QualityDecision{false, NetworkCondition::Excellent, 0.0, 0, "Quality control disabled"};
    }

    // Fast path: skip the mutex entirely when we know no dropping is needed.
    // g_dropMayBeNeeded is set by updateNetworkStats() whenever the condition
    // degrades to Fair/Poor/Critical. On localhost this is almost always false.
    if (!g_dropMayBeNeeded.load(std::memory_order_relaxed) && config.minFrameIntervalMs == 0) {
        return QualityDecision{false, NetworkCondition::Excellent, 0.0, 0, "Fast path: no drop"};
    }

    std::lock_guard<std::mutex> lock(droppingState.mutex);

    // Check minimum frame interval to prevent excessive dropping
    auto now = std::chrono::steady_clock::now();
    auto timeSinceLastFrame = std::chrono::duration_cast<std::chrono::milliseconds>(
        now - droppingState.lastFrameTime).count();

    if (timeSinceLastFrame < static_cast<long long>(config.minFrameIntervalMs)) {
        return QualityDecision{true, NetworkCondition::Excellent, 0.0, 1,
                             "Frame rate too high, enforcing minimum interval"};
    }

    NetworkCondition condition = assessNetworkCondition();
    double dropRatio = 0.0;

    // Determine drop ratio based on network condition
    switch (condition) {
        case NetworkCondition::Excellent:
            dropRatio = config.excellentDropRatio;
            break;
        case NetworkCondition::Good:
            dropRatio = config.goodDropRatio;
            break;
        case NetworkCondition::Fair:
            dropRatio = config.fairDropRatio;
            break;
        case NetworkCondition::Poor:
            dropRatio = config.poorDropRatio;
            break;
        case NetworkCondition::Critical:
            dropRatio = config.criticalDropRatio;
            break;
    }

    // If adaptive dropping is disabled, only drop in critical conditions
    if (!config.enableAdaptiveDropping && condition != NetworkCondition::Critical) {
        dropRatio = 0.0;
    }

    // Calculate if this frame should be dropped
    bool shouldDrop = false;
    uint32_t framesUntilNext = 0;

    if (dropRatio > 0.0) {
        // Use a simple counter-based dropping strategy
        // This ensures even distribution of dropped frames
        uint32_t dropInterval = static_cast<uint32_t>(1.0 / dropRatio);
        shouldDrop = (droppingState.frameCounter % dropInterval) != 0;

        if (shouldDrop) {
            framesUntilNext = dropInterval - (droppingState.frameCounter % dropInterval);
        }
    }

    droppingState.frameCounter++;

    if (shouldDrop) {
        droppingState.framesDropped++;
        droppingState.lastFrameTime = now;

        std::string reason = "Network condition: " + std::to_string(static_cast<int>(condition)) +
                           ", RTT: " + std::to_string(currentStats.rttMs) + "ms" +
                           ", Loss: " + std::to_string(currentStats.packetLoss * 100.0) + "%" +
                           ", Queue: " + std::to_string(currentStats.pacerQueueLength);

        return QualityDecision{true, condition, dropRatio, framesUntilNext, reason};
    } else {
        droppingState.framesSent++;
        droppingState.lastFrameTime = now;

        return QualityDecision{false, condition, dropRatio, 0, "Frame accepted"};
    }
}

void QualityController::updateNetworkStats(const NetworkStats& stats) {
    std::lock_guard<std::mutex> lock(statsMutex);
    currentStats = stats;

    // Log significant changes in network conditions
    static NetworkCondition lastCondition = NetworkCondition::Excellent;
    NetworkCondition newCondition = assessNetworkCondition();

    // Update fast-path flag: dropping is only possible for Fair/Poor/Critical conditions.
    // This lets shouldDropFrame() skip the mutex entirely on healthy networks.
    bool dropPossible = (newCondition == NetworkCondition::Fair ||
                         newCondition == NetworkCondition::Poor ||
                         newCondition == NetworkCondition::Critical);
    g_dropMayBeNeeded.store(dropPossible, std::memory_order_relaxed);

    if (newCondition != lastCondition) {
        std::string conditionStr;
        switch (newCondition) {
            case NetworkCondition::Excellent: conditionStr = "EXCELLENT"; break;
            case NetworkCondition::Good: conditionStr = "GOOD"; break;
            case NetworkCondition::Fair: conditionStr = "FAIR"; break;
            case NetworkCondition::Poor: conditionStr = "POOR"; break;
            case NetworkCondition::Critical: conditionStr = "CRITICAL"; break;
        }

        LOG_INFO("Network condition changed to: " + conditionStr +
                " (RTT: " + std::to_string(stats.rttMs) + "ms, " +
                "Loss: " + std::to_string(stats.packetLoss * 100.0) + "%, " +
                "Queue: " + std::to_string(stats.pacerQueueLength) + ")");

        lastCondition = newCondition;
    }

    // Log congestion signals
    if (stats.pliCount > 0 || stats.nackCount > 0) {
        LOG_WARN("Network congestion detected - PLI: " + std::to_string(stats.pliCount) +
                ", NACK: " + std::to_string(stats.nackCount));
    }
}

double QualityController::getDropRate() const {
    uint32_t totalFrames = droppingState.framesDropped + droppingState.framesSent;
    if (totalFrames == 0) {
        return 0.0;
    }
    return static_cast<double>(droppingState.framesDropped) / static_cast<double>(totalFrames);
}

void QualityController::resetStats() {
    std::lock_guard<std::mutex> lock(droppingState.mutex);
    droppingState.frameCounter = 0;
    droppingState.framesDropped = 0;
    droppingState.framesSent = 0;
    droppingState.lastFrameTime = std::chrono::steady_clock::now();

    LOG_INFO("Quality control statistics reset");
}

} // namespace AdaptiveQualityControl
