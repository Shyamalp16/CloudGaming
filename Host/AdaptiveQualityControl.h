#pragma once

#include <atomic>
#include <mutex>
#include <functional>
#include <chrono>
#include <vector>
#include "WebRTCWrapper.h"

/**
 * @brief Adaptive Quality Control System
 *
 * Monitors WebRTC network conditions and implements upstream frame dropping
 * to prevent queue buildup and maintain low-latency streaming.
 *
 * This system provides:
 * - Real-time network condition assessment
 * - Adaptive frame dropping based on congestion signals
 * - Quality degradation prevention through early intervention
 * - Configurable dropping strategies and thresholds
 */

namespace AdaptiveQualityControl {

// Network condition assessment
enum class NetworkCondition {
    Excellent,   // < 10ms RTT, < 1% loss, minimal queue
    Good,        // < 50ms RTT, < 5% loss, small queue
    Fair,        // < 100ms RTT, < 10% loss, moderate queue
    Poor,        // < 200ms RTT, < 20% loss, large queue
    Critical     // > 200ms RTT, > 20% loss, very large queue
};

// Dropping strategy configuration
struct DroppingConfig {
    // RTT thresholds (milliseconds)
    double rttExcellentThreshold = 10.0;
    double rttGoodThreshold = 50.0;
    double rttFairThreshold = 100.0;
    double rttPoorThreshold = 200.0;

    // Packet loss thresholds (percentage)
    double lossExcellentThreshold = 0.01;  // 1%
    double lossGoodThreshold = 0.05;       // 5%
    double lossFairThreshold = 0.10;       // 10%
    double lossPoorThreshold = 0.20;       // 20%

    // Queue length thresholds
    uint32_t queueExcellentThreshold = 1;
    uint32_t queueGoodThreshold = 2;
    uint32_t queueFairThreshold = 5;
    uint32_t queuePoorThreshold = 10;

    // Dropping ratios (frames to drop per N frames)
    double excellentDropRatio = 0.0;   // No dropping
    double goodDropRatio = 0.0;        // No dropping
    double fairDropRatio = 0.25;       // Drop 1 in 4 frames
    double poorDropRatio = 0.5;        // Drop 1 in 2 frames
    double criticalDropRatio = 0.75;   // Drop 3 in 4 frames

    // Adaptive behavior
    bool enableAdaptiveDropping = true;
    uint32_t minFrameIntervalMs = 5;   // Minimum time between frames (200fps max)
    uint32_t statsUpdateIntervalMs = 100; // How often to update network assessment
};

// Current network statistics
struct NetworkStats {
    double packetLoss = 0.0;           // Packet loss percentage (0.0-1.0)
    double rttMs = 0.0;               // Round-trip time in milliseconds
    double jitterMs = 0.0;            // Jitter in milliseconds
    uint32_t nackCount = 0;           // Total NACK packets received
    uint32_t pliCount = 0;            // Total PLI packets received
    uint32_t twccCount = 0;           // Total TWCC feedback packets
    uint32_t pacerQueueLength = 0;    // Current pacer queue length
    uint32_t sendBitrateKbps = 0;     // Current send bitrate
    std::chrono::steady_clock::time_point lastUpdate;
};

// Quality control decision
struct QualityDecision {
    bool shouldDropFrame = false;
    NetworkCondition condition = NetworkCondition::Excellent;
    double dropRatio = 0.0;
    uint32_t framesUntilNext = 0;     // Frames until next should be sent
    std::string reason;               // Reason for the decision
};

// Frame dropping state
struct DroppingState {
    uint32_t frameCounter = 0;
    uint32_t framesDropped = 0;
    uint32_t framesSent = 0;
    double dropAccumulator = 0.0;
    std::chrono::steady_clock::time_point lastFrameTime;
    std::mutex mutex;
};

// Main adaptive quality control class
class QualityController {
private:
    DroppingConfig config;
    NetworkStats currentStats;
    DroppingState droppingState;
    std::atomic<bool> isEnabled;
    std::mutex statsMutex;
    // Fast-path flag: set to true only when network condition could require dropping.
    // Allows shouldDropFrame() to return immediately without the mutex in the common case.
    std::atomic<bool> g_dropMayBeNeeded{false};

    // WebRTC stats callback
    static void webrtcStatsCallback(double packetLoss, double rtt, double jitter,
                                   uint32_t nackCount, uint32_t pliCount, uint32_t twccCount,
                                   uint32_t pacerQueueLength, uint32_t sendBitrateKbps);

public:
    QualityController();
    ~QualityController();

    // Configuration
    void setConfig(const DroppingConfig& newConfig);

    // Control
    void enable();
    void disable();
    bool isActive() const { return isEnabled.load(); }

    // Quality decision making
    QualityDecision shouldDropFrame();

    // Stats management
    void updateNetworkStats(const NetworkStats& stats);

    // Network condition assessment
    NetworkCondition assessNetworkCondition() const;

};

// Global instance
extern QualityController globalQualityController;

// Convenience functions
inline void enableAdaptiveQualityControl() {
    globalQualityController.enable();
}

inline QualityDecision checkFrameDropping() {
    return globalQualityController.shouldDropFrame();
}

} // namespace AdaptiveQualityControl
