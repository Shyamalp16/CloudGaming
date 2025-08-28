#include "InputSequenceManager.h"
#include "InputStateMachine.h"
#include <iostream>
#include <sstream>

namespace InputSequenceManager {

// Global sequence manager instance
SequenceManager globalSequenceManager;

SequenceManager::SequenceManager(const SequenceConfig& config)
    : config_(config), recoveryThrottled_(false),
      lastRecoveryTime_(std::chrono::steady_clock::now()) {
}

GapResult SequenceManager::processSequence(uint64_t sequenceId, const std::string& messageType) {
    std::lock_guard<std::mutex> lock(mutex_);

    // Check for gaps
    GapResult gapResult = detectGap(sequenceId);

    if (gapResult.action != RecoveryAction::NONE) {
        // Gap detected, trigger recovery if not throttled
        if (!gapResult.shouldThrottle) {
            trackGapDetected(gapResult.gapSize);
            trackRecoveryTriggered(gapResult.action);

            // Execute recovery action
            if (recoveryCallback_) {
                recoveryCallback_(gapResult.action);
            }
        }
    }

    // Update sequence state
    state_.lastReceivedSeq.store(sequenceId);
    state_.expectedSeq.store(sequenceId + 1);

    return gapResult;
}

void SequenceManager::processSnapshot(const KeyStateSnapshot& snapshot) {
    std::lock_guard<std::mutex> lock(mutex_);

    std::cout << "[InputSequenceManager] Processing snapshot: seq=" << snapshot.sequenceId
              << ", keys=" << snapshot.keyStates.size() << std::endl;

    // Update metrics
    state_.snapshotsReceived.fetch_add(1);

    // Here we would apply the snapshot to synchronize state
    // This would involve updating the FSM with the received key states

    // Reset expected sequence to snapshot sequence + 1
    state_.expectedSeq.store(snapshot.sequenceId + 1);
}

void SequenceManager::reset() {
    std::lock_guard<std::mutex> lock(mutex_);

    std::cout << "[InputSequenceManager] Resetting sequence state" << std::endl;

    state_.lastReceivedSeq.store(0);
    state_.expectedSeq.store(1);
    state_.gapsDetected.store(0);
    state_.recoveriesTriggered.store(0);
    state_.snapshotsRequested.store(0);
    state_.snapshotsReceived.store(0);
    state_.pendingSequences.clear();

    recoveryThrottled_ = false;
    lastRecoveryTime_ = std::chrono::steady_clock::now();
}

void SequenceManager::updateConfig(const SequenceConfig& config) {
    std::lock_guard<std::mutex> lock(mutex_);
    config_ = config;
}

void SequenceManager::setRecoveryCallback(std::function<void(RecoveryAction)> callback) {
    std::lock_guard<std::mutex> lock(mutex_);
    recoveryCallback_ = callback;
}

void SequenceManager::setSnapshotRequestCallback(std::function<void()> callback) {
    std::lock_guard<std::mutex> lock(mutex_);
    snapshotRequestCallback_ = callback;
}

bool SequenceManager::isRecoveryThrottled() {
    std::lock_guard<std::mutex> lock(mutex_);
    return recoveryThrottled_;
}

GapResult SequenceManager::detectGap(uint64_t sequenceId) {
    GapResult result;

    uint64_t expectedSeq = state_.expectedSeq.load();
    uint64_t lastReceivedSeq = state_.lastReceivedSeq.load();

    // Check for gap
    if (sequenceId > expectedSeq) {
        // Gap detected
        result.gapSize = sequenceId - expectedSeq;
        result.lastReceivedSeq = lastReceivedSeq;
        result.expectedSeq = expectedSeq;

        // Determine recovery action
        result.action = determineRecoveryAction(result.gapSize);

        // Check if recovery should be throttled
        result.shouldThrottle = shouldThrottleRecovery();

        if (!result.shouldThrottle) {
            state_.gapsDetected.fetch_add(1);
        }

        std::cout << "[InputSequenceManager] Gap detected: expected=" << expectedSeq
                  << ", received=" << sequenceId << ", gap=" << result.gapSize
                  << ", action=" << (result.action == RecoveryAction::RELEASE_MODIFIERS ? "RELEASE_MODIFIERS" :
                                   result.action == RecoveryAction::REQUEST_SNAPSHOT ? "REQUEST_SNAPSHOT" :
                                   result.action == RecoveryAction::RESET_STATE ? "RESET_STATE" : "NONE")
                  << (result.shouldThrottle ? " (THROTTLED)" : "") << std::endl;

    } else if (sequenceId < expectedSeq) {
        // Out-of-order message (late arrival)
        std::cout << "[InputSequenceManager] Out-of-order message: expected=" << expectedSeq
                  << ", received=" << sequenceId << std::endl;
    }

    return result;
}

RecoveryAction SequenceManager::determineRecoveryAction(uint64_t gapSize) {
    if (!config_.enableGapRecovery) {
        return RecoveryAction::NONE;
    }

    if (gapSize >= config_.maxGapBeforeRecovery) {
        // Large gap - request snapshot for full state recovery
        if (config_.enableSnapshotRecovery) {
            return RecoveryAction::REQUEST_SNAPSHOT;
        } else {
            return RecoveryAction::RELEASE_MODIFIERS;
        }
    } else {
        // Small gap - just release modifiers
        return RecoveryAction::RELEASE_MODIFIERS;
    }
}

bool SequenceManager::shouldThrottleRecovery() {
    auto now = std::chrono::steady_clock::now();
    auto timeSinceLastRecovery = std::chrono::duration_cast<std::chrono::milliseconds>(
        now - lastRecoveryTime_);

    if (timeSinceLastRecovery.count() < config_.recoveryThrottleMs) {
        recoveryThrottled_ = true;
        return true;
    }

    recoveryThrottled_ = false;
    return false;
}

void SequenceManager::updateRecoveryThrottle() {
    lastRecoveryTime_ = std::chrono::steady_clock::now();
    recoveryThrottled_ = false;
}

void SequenceManager::trackGapDetected(uint64_t gapSize) {
    // Update global metrics
    InputMetrics::inc(InputMetrics::fsmInvalidTransitions()); // Reuse for gap tracking
}

void SequenceManager::trackRecoveryTriggered(RecoveryAction action) {
    state_.recoveriesTriggered.fetch_add(1);

    if (action == RecoveryAction::REQUEST_SNAPSHOT) {
        state_.snapshotsRequested.fetch_add(1);
    }

    updateRecoveryThrottle();
}

// Utility functions
KeyStateSnapshot createSnapshot(uint64_t seqId, const std::unordered_map<std::string, bool>& keyStates) {
    KeyStateSnapshot snapshot;
    snapshot.sequenceId = seqId;
    snapshot.keyStates = keyStates;
    snapshot.timestamp = std::chrono::steady_clock::now();
    return snapshot;
}

KeyStateSnapshot parseSnapshot(const std::string& snapshotJson) {
    KeyStateSnapshot snapshot;

    // Parse JSON snapshot - simplified implementation
    // In a real implementation, you'd use a proper JSON parser
    if (snapshotJson.find("\"sequenceId\"") != std::string::npos) {
        // Extract sequence ID and key states from JSON
        // This is a placeholder - real implementation would parse properly
        snapshot.sequenceId = 1; // placeholder
        snapshot.timestamp = std::chrono::steady_clock::now();
    }

    return snapshot;
}

std::string serializeSnapshot(const KeyStateSnapshot& snapshot) {
    std::stringstream ss;
    ss << "{";
    ss << "\"sequenceId\":" << snapshot.sequenceId << ",";
    ss << "\"timestamp\":" << std::chrono::duration_cast<std::chrono::milliseconds>(
        snapshot.timestamp.time_since_epoch()).count() << ",";
    ss << "\"keyStates\":{";
    bool first = true;
    for (const auto& [key, pressed] : snapshot.keyStates) {
        if (!first) ss << ",";
        ss << "\"" << key << "\":" << (pressed ? "true" : "false");
        first = false;
    }
    ss << "}}";
    return ss.str();
}

void updateGlobalConfig(const SequenceConfig& config) {
    globalSequenceManager.updateConfig(config);
}

} // namespace InputSequenceManager
