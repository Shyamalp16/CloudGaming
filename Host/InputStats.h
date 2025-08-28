#pragma once

#include <atomic>
#include <chrono>
#include <thread>
#include <mutex>
#include <string>
#include <iostream>
#include "Metrics.h"

namespace InputStats {

// Comprehensive input statistics structure
struct KeyboardStats {
    std::atomic<uint64_t> events_received{0};      // Total key events received
    std::atomic<uint64_t> events_dropped_invalid{0}; // Invalid transitions dropped
    std::atomic<uint64_t> events_skipped_foreground{0}; // Skipped due to foreground check
    std::atomic<uint64_t> events_injected{0};      // Successfully injected events
    std::atomic<uint64_t> events_timeout_released{0}; // Released due to timeout
    std::atomic<uint64_t> modifier_timeout_released{0}; // Modifier keys released due to timeout
    std::atomic<uint64_t> regular_timeout_released{0}; // Regular keys released due to timeout
    std::atomic<uint64_t> release_all_count{0};    // Emergency release operations
    std::atomic<uint64_t> events_stale_ignored{0}; // Stale events ignored
    std::atomic<uint64_t> recovery_success_count{0}; // Successful recoveries

    // Computed metrics (updated periodically)
    double injection_success_rate{0.0}; // events_injected / events_received
    double drop_rate{0.0};              // events_dropped_invalid / events_received
    double skip_rate{0.0};              // events_skipped_foreground / events_received
};

struct MouseStats {
    std::atomic<uint64_t> events_received{0};      // Total mouse events received
    std::atomic<uint64_t> events_skipped_foreground{0}; // Skipped due to foreground check
    std::atomic<uint64_t> events_injected{0};      // Successfully injected events
    std::atomic<uint64_t> moves_coalesced{0};      // Coalesced mouse movements
    std::atomic<uint64_t> clicks_processed{0};     // Click events processed
    std::atomic<uint64_t> wheel_events{0};         // Wheel events processed

    // Computed metrics
    double injection_success_rate{0.0};
    double skip_rate{0.0};
    double coalesce_rate{0.0};
};

// Logging configuration for hot path optimization
struct LoggingConfig {
    bool enablePerEventLogging = false;      // Enable detailed per-event logging
    bool enableAggregatedLogging = true;     // Enable 10Hz aggregated stats
    bool enableMouseMoveCoalescing = true;   // Coalesce rapid mouse moves
    uint32_t aggregatedLogIntervalMs = 60000;  // 1 minute interval
    uint32_t maxMouseMovesPerFrame = 1;      // Max mouse moves to process per frame
};

// Global logging configuration
extern LoggingConfig globalLoggingConfig;

// Configuration API
void updateLoggingConfig(const LoggingConfig& config);
void enablePerEventLogging(bool enable);
void enableAggregatedLogging(bool enable);
void enableMouseMoveCoalescing(bool enable);
void setAggregatedLogInterval(uint32_t intervalMs);

struct InputHealthStats {
    KeyboardStats keyboard;
    MouseStats mouse;

    // Overall health metrics
    std::atomic<uint64_t> total_events_processed{0};
    std::atomic<uint64_t> uptime_seconds{0};

    // Timing stats (rolling averages)
    std::atomic<double> avg_processing_time_us{0.0};
    std::atomic<uint64_t> processing_time_samples{0};

    // Reset all counters
    void reset() {
        keyboard.events_received.store(0);
        keyboard.events_dropped_invalid.store(0);
        keyboard.events_skipped_foreground.store(0);
        keyboard.events_injected.store(0);
        keyboard.events_timeout_released.store(0);
        keyboard.modifier_timeout_released.store(0);
        keyboard.regular_timeout_released.store(0);
        keyboard.release_all_count.store(0);
        keyboard.events_stale_ignored.store(0);
        keyboard.recovery_success_count.store(0);

        mouse.events_received.store(0);
        mouse.events_skipped_foreground.store(0);
        mouse.events_injected.store(0);
        mouse.moves_coalesced.store(0);
        mouse.clicks_processed.store(0);
        mouse.wheel_events.store(0);

        total_events_processed.store(0);
        uptime_seconds.store(0);
        avg_processing_time_us.store(0.0);
        processing_time_samples.store(0);
    }

    // Update computed metrics
    void updateComputedMetrics() {
        auto total_kb_events = keyboard.events_received.load();
        if (total_kb_events > 0) {
            keyboard.injection_success_rate =
                static_cast<double>(keyboard.events_injected.load()) / total_kb_events;
            keyboard.drop_rate =
                static_cast<double>(keyboard.events_dropped_invalid.load()) / total_kb_events;
            keyboard.skip_rate =
                static_cast<double>(keyboard.events_skipped_foreground.load()) / total_kb_events;
        }

        auto total_mouse_events = mouse.events_received.load();
        if (total_mouse_events > 0) {
            mouse.injection_success_rate =
                static_cast<double>(mouse.events_injected.load()) / total_mouse_events;
            mouse.skip_rate =
                static_cast<double>(mouse.events_skipped_foreground.load()) / total_mouse_events;

            auto total_moves = mouse.events_received.load() - mouse.clicks_processed.load() - mouse.wheel_events.load();
            if (total_moves > 0) {
                mouse.coalesce_rate =
                    static_cast<double>(mouse.moves_coalesced.load()) / total_moves;
            }
        }
    }
};

// Global statistics instance
extern InputHealthStats globalInputStats;

// Statistics logger class
class StatsLogger {
private:
    std::thread logger_thread_;
    std::atomic<bool> running_{false};
    std::mutex mutex_;
    std::chrono::steady_clock::time_point start_time_;

public:
    StatsLogger();
    ~StatsLogger();

    void start();
    void stop();

    // Get formatted statistics string
    std::string getStatsString();

    // Log statistics to console
    void logStats();

private:
    void loggerLoop();
    void updateUptime();
};

// Global stats logger instance
extern StatsLogger globalStatsLogger;

// API for accessing statistics
inline std::string getStatsSummary() {
    return globalStatsLogger.getStatsString();
}

inline const InputHealthStats& getRawStats() {
    return globalInputStats;
}

inline void resetAllStats() {
    globalInputStats.reset();
}

// Helper functions for tracking
namespace Track {

// Keyboard event tracking
inline void keyboardEventReceived() {
    globalInputStats.keyboard.events_received.fetch_add(1);
    globalInputStats.total_events_processed.fetch_add(1);
}

inline void keyboardEventDroppedInvalid() {
    globalInputStats.keyboard.events_dropped_invalid.fetch_add(1);
    InputMetrics::inc(InputMetrics::fsmInvalidTransitions());
}

inline void keyboardEventSkippedForeground() {
    globalInputStats.keyboard.events_skipped_foreground.fetch_add(1);
    InputMetrics::inc(InputMetrics::skippedDueToForeground());
}

inline void keyboardEventInjected() {
    globalInputStats.keyboard.events_injected.fetch_add(1);
    InputMetrics::inc(InputMetrics::injectionSuccesses());
}

inline void keyboardEventTimeoutReleased(bool isModifier = false) {
    globalInputStats.keyboard.events_timeout_released.fetch_add(1);
    if (isModifier) {
        globalInputStats.keyboard.modifier_timeout_released.fetch_add(1);
    } else {
        globalInputStats.keyboard.regular_timeout_released.fetch_add(1);
    }
    InputMetrics::inc(InputMetrics::fsmRecoveries());
}

inline void keyboardEmergencyRelease() {
    globalInputStats.keyboard.release_all_count.fetch_add(1);
    InputMetrics::inc(InputMetrics::fsmEmergencyReleases());
}

inline void keyboardEventStaleIgnored() {
    globalInputStats.keyboard.events_stale_ignored.fetch_add(1);
    InputMetrics::inc(InputMetrics::fsmStaleTransitions());
}

inline void keyboardRecoverySuccess() {
    globalInputStats.keyboard.recovery_success_count.fetch_add(1);
}

inline void keyboardEventBlocked() {
    globalInputStats.keyboard.events_skipped_foreground.fetch_add(1);
    InputMetrics::inc(InputMetrics::keysBlocked());
}

// Mouse event tracking
inline void mouseEventReceived() {
    globalInputStats.mouse.events_received.fetch_add(1);
    globalInputStats.total_events_processed.fetch_add(1);
}

inline void mouseEventCoalesced() {
    globalInputStats.mouse.moves_coalesced.fetch_add(1);
}

inline void mouseEventSkippedForeground() {
    globalInputStats.mouse.events_skipped_foreground.fetch_add(1);
    InputMetrics::inc(InputMetrics::skippedDueToForeground());
}

inline void mouseEventInjected() {
    globalInputStats.mouse.events_injected.fetch_add(1);
    InputMetrics::inc(InputMetrics::injectionSuccesses());
}

inline void mouseMoveCoalesced() {
    globalInputStats.mouse.moves_coalesced.fetch_add(1);
}

inline void mouseClickProcessed() {
    globalInputStats.mouse.clicks_processed.fetch_add(1);
}

inline void mouseWheelEvent() {
    globalInputStats.mouse.wheel_events.fetch_add(1);
}

// Processing time tracking
inline void recordProcessingTime(std::chrono::microseconds duration) {
    double current_avg = globalInputStats.avg_processing_time_us.load();
    uint64_t samples = globalInputStats.processing_time_samples.fetch_add(1);

    // Exponential moving average
    double alpha = 0.1; // Smoothing factor
    double new_avg = current_avg * (1.0 - alpha) + static_cast<double>(duration.count()) * alpha;

    globalInputStats.avg_processing_time_us.store(new_avg);
}

} // namespace Track

} // namespace InputStats
