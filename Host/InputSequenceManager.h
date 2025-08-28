#pragma once

#include <atomic>
#include <unordered_map>
#include <mutex>
#include <chrono>
#include <functional>
#include "Metrics.h"

namespace InputSequenceManager {

// Sequence gap detection and recovery configuration
struct SequenceConfig {
    uint32_t maxGapBeforeRecovery = 10;        // Max gap before triggering recovery
    uint32_t recoveryThrottleMs = 1000;        // Minimum time between recoveries
    bool enableGapRecovery = true;             // Enable gap-based recovery
    bool enableSnapshotRecovery = true;        // Enable snapshot-based recovery
    uint32_t snapshotRequestTimeoutMs = 5000;  // Timeout for snapshot requests
};

// Sequence state for a client session
struct SequenceState {
    std::atomic<uint64_t> lastReceivedSeq{0};   // Last received sequence number
    std::atomic<uint64_t> expectedSeq{0};       // Expected next sequence number
    std::atomic<uint64_t> gapsDetected{0};      // Total gaps detected
    std::atomic<uint64_t> recoveriesTriggered{0}; // Total recoveries triggered
    std::atomic<uint64_t> snapshotsRequested{0}; // Snapshots requested
    std::atomic<uint64_t> snapshotsReceived{0};  // Snapshots received

    std::chrono::steady_clock::time_point lastRecoveryTime; // Last recovery timestamp
    std::unordered_map<uint64_t, bool> pendingSequences;    // Track pending sequences
};

// Key state snapshot for recovery
struct KeyStateSnapshot {
    uint64_t sequenceId;
    std::chrono::steady_clock::time_point timestamp;
    std::unordered_map<std::string, bool> keyStates; // jsCode -> isPressed

    KeyStateSnapshot() : sequenceId(0), timestamp(std::chrono::steady_clock::now()) {}
};

// Recovery action types
enum class RecoveryAction {
    NONE,               // No action needed
    RELEASE_MODIFIERS,  // Release stuck modifier keys
    REQUEST_SNAPSHOT,   // Request full state snapshot from client
    RESET_STATE         // Full state reset
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
    SequenceConfig config_;
    std::mutex mutex_;
    std::function<void(RecoveryAction)> recoveryCallback_;
    std::function<void()> snapshotRequestCallback_;

    // Recovery throttling
    std::chrono::steady_clock::time_point lastRecoveryTime_;
    bool recoveryThrottled_;

public:
    SequenceManager(const SequenceConfig& config = SequenceConfig());

    // Process incoming message sequence
    GapResult processSequence(uint64_t sequenceId, const std::string& messageType = "input");

    // Handle snapshot response from client
    void processSnapshot(const KeyStateSnapshot& snapshot);

    // Reset sequence state (on reconnect)
    void reset();

    // Update configuration
    void updateConfig(const SequenceConfig& config);

    // Set recovery callback
    void setRecoveryCallback(std::function<void(RecoveryAction)> callback);

    // Set snapshot request callback
    void setSnapshotRequestCallback(std::function<void()> callback);

    // Get current state
    const SequenceState& getState() const { return state_; }

    // Check if recovery is currently throttled
    bool isRecoveryThrottled();

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
    void trackGapDetected(uint64_t gapSize);
    void trackRecoveryTriggered(RecoveryAction action);
};

// Global sequence manager instance
extern SequenceManager globalSequenceManager;

// Configuration update function
void updateGlobalConfig(const SequenceConfig& config);

// Snapshot creation and parsing utilities
KeyStateSnapshot createSnapshot(uint64_t seqId, const std::unordered_map<std::string, bool>& keyStates);
KeyStateSnapshot parseSnapshot(const std::string& snapshotJson);
std::string serializeSnapshot(const KeyStateSnapshot& snapshot);

} // namespace InputSequenceManager
