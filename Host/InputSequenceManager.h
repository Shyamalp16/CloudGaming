#pragma once

#include <atomic>
#include <mutex>
#include <chrono>
#include <functional>

namespace InputSequenceManager {

// Sequence state for a client session
struct SequenceState {
    std::atomic<uint64_t> lastReceivedSeq{0};   // Last received sequence number
    std::atomic<uint64_t> expectedSeq{0};       // Expected next sequence number
    std::atomic<uint64_t> gapsDetected{0};      // Total gaps detected
    std::atomic<uint64_t> recoveriesTriggered{0}; // Total recoveries triggered
};

// Recovery action types
enum class RecoveryAction {
    NONE,               // No action needed
    RELEASE_MODIFIERS,  // Release stuck modifier keys
};

// Gap detection result
struct GapResult {
    RecoveryAction action;
    uint64_t gapSize;
    uint64_t lastReceivedSeq;
    uint64_t expectedSeq;
    bool shouldThrottle;

    GapResult() : action(RecoveryAction::NONE), gapSize(0), lastReceivedSeq(0),
                  expectedSeq(0), shouldThrottle(false) {}
};

// Main sequence manager class
class SequenceManager {
private:
    SequenceState state_;
    std::mutex mutex_;
    std::function<void(RecoveryAction)> recoveryCallback_;

    // Recovery throttling
    std::chrono::steady_clock::time_point lastRecoveryTime_;
    bool recoveryThrottled_;

public:
    SequenceManager();

    // Process incoming message sequence
    GapResult processSequence(uint64_t sequenceId);

    // Reset sequence state (on reconnect)
    void reset();

    // Set recovery callback
    void setRecoveryCallback(std::function<void(RecoveryAction)> callback);

private:
    // Detect gaps in sequence
    GapResult detectGap(uint64_t sequenceId);

    // Determine recovery action based on gap
    RecoveryAction determineRecoveryAction(uint64_t gapSize);

    // Check if recovery should be throttled
    bool shouldThrottleRecovery();

    // Update recovery throttle timer
    void updateRecoveryThrottle();

    // Metrics tracking
    void trackRecoveryTriggered();
};

// Global sequence manager instance
extern SequenceManager globalSequenceManager;

} // namespace InputSequenceManager
