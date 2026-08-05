#include "InputSequenceManager.h"
#include <iostream>

namespace InputSequenceManager {

// Global sequence manager instance
SequenceManager globalSequenceManager;

SequenceManager::SequenceManager()
    : recoveryThrottled_(false),
      lastRecoveryTime_(std::chrono::steady_clock::now()) {
}

GapResult SequenceManager::processSequence(uint64_t sequenceId) {
    std::lock_guard<std::mutex> lock(mutex_);

    // Check for gaps
    GapResult gapResult = detectGap(sequenceId);

    if (gapResult.action != RecoveryAction::NONE) {
        // Gap detected, trigger recovery if not throttled
        if (!gapResult.shouldThrottle) {
            trackRecoveryTriggered();

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

void SequenceManager::reset() {
    std::lock_guard<std::mutex> lock(mutex_);

    std::cout << "[InputSequenceManager] Resetting sequence state" << std::endl;

    state_.lastReceivedSeq.store(0);
    state_.expectedSeq.store(1);
    state_.gapsDetected.store(0);
    state_.recoveriesTriggered.store(0);
    recoveryThrottled_ = false;
    lastRecoveryTime_ = std::chrono::steady_clock::now();
}

void SequenceManager::setRecoveryCallback(std::function<void(RecoveryAction)> callback) {
    std::lock_guard<std::mutex> lock(mutex_);
    recoveryCallback_ = callback;
}

GapResult SequenceManager::detectGap(uint64_t sequenceId) {
    GapResult result;

    uint64_t expectedSeq = state_.expectedSeq.load();
    uint64_t lastReceivedSeq = state_.lastReceivedSeq.load();

    // First message - no gap possible
    if (lastReceivedSeq == 0 && expectedSeq == 0) {
        result.action = RecoveryAction::NONE;
        return result;
    }

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
                  << ", action=" << (result.action == RecoveryAction::RELEASE_MODIFIERS ? "RELEASE_MODIFIERS" : "NONE")
                  << (result.shouldThrottle ? " (THROTTLED)" : "") << std::endl;

    } else if (sequenceId < expectedSeq && sequenceId != 0) {
        // Out-of-order message (late arrival) - but allow sequence 0 as valid first message
        // Only log if this is a significant out-of-order event (not just sequence 0)
        if (expectedSeq > 1) {
            std::cout << "[InputSequenceManager] Out-of-order message: expected=" << expectedSeq
                      << ", received=" << sequenceId << std::endl;
        }
    }

    return result;
}

RecoveryAction SequenceManager::determineRecoveryAction(uint64_t) {
    return RecoveryAction::RELEASE_MODIFIERS;
}

bool SequenceManager::shouldThrottleRecovery() {
    auto now = std::chrono::steady_clock::now();
    auto timeSinceLastRecovery = std::chrono::duration_cast<std::chrono::milliseconds>(
        now - lastRecoveryTime_);

    if (timeSinceLastRecovery.count() < 1000) {
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

void SequenceManager::trackRecoveryTriggered() {
    state_.recoveriesTriggered.fetch_add(1);
    updateRecoveryThrottle();
}

} // namespace InputSequenceManager
