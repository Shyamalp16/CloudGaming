#pragma once
#ifndef INPUT_METRICS_H
#define INPUT_METRICS_H

#include <atomic>
#include <cstdint>

namespace InputMetrics {

// Counters
inline std::atomic<uint64_t>& receivedKeyboard() { static std::atomic<uint64_t> v{0}; return v; }
inline std::atomic<uint64_t>& receivedMouse() { static std::atomic<uint64_t> v{0}; return v; }
inline std::atomic<uint64_t>& enqueuedKeyboard() { static std::atomic<uint64_t> v{0}; return v; }
inline std::atomic<uint64_t>& enqueuedMouse() { static std::atomic<uint64_t> v{0}; return v; }
inline std::atomic<uint64_t>& droppedKeyboard() { static std::atomic<uint64_t> v{0}; return v; }
inline std::atomic<uint64_t>& droppedMouse() { static std::atomic<uint64_t> v{0}; return v; }
inline std::atomic<uint64_t>& injectedKeys() { static std::atomic<uint64_t> v{0}; return v; }
inline std::atomic<uint64_t>& injectedMouse() { static std::atomic<uint64_t> v{0}; return v; }
inline std::atomic<uint64_t>& injectErrors() { static std::atomic<uint64_t> v{0}; return v; }

// Input injection precondition metrics
inline std::atomic<uint64_t>& skippedDueToForeground() { static std::atomic<uint64_t> v{0}; return v; }
inline std::atomic<uint64_t>& skippedDueToWindowState() { static std::atomic<uint64_t> v{0}; return v; }
inline std::atomic<uint64_t>& injectionAttempts() { static std::atomic<uint64_t> v{0}; return v; }
inline std::atomic<uint64_t>& injectionSuccesses() { static std::atomic<uint64_t> v{0}; return v; }

// FSM recovery metrics
inline std::atomic<uint64_t>& fsmRecoveries() { static std::atomic<uint64_t> v{0}; return v; }
inline std::atomic<uint64_t>& fsmInvalidTransitions() { static std::atomic<uint64_t> v{0}; return v; }
inline std::atomic<uint64_t>& fsmStaleTransitions() { static std::atomic<uint64_t> v{0}; return v; }
inline std::atomic<uint64_t>& fsmEmergencyReleases() { static std::atomic<uint64_t> v{0}; return v; }
inline std::atomic<uint64_t>& fsmStateResets() { static std::atomic<uint64_t> v{0}; return v; }

// Mouse coordinate transformation metrics
inline std::atomic<uint64_t>& mouseCoordTransformSuccess() { static std::atomic<uint64_t> v{0}; return v; }
inline std::atomic<uint64_t>& mouseCoordTransformErrors() { static std::atomic<uint64_t> v{0}; return v; }
inline std::atomic<uint64_t>& mouseCoordClipped() { static std::atomic<uint64_t> v{0}; return v; }
inline std::atomic<uint64_t>& mouseCoordOutOfBounds() { static std::atomic<uint64_t> v{0}; return v; }

// Last error codes
inline std::atomic<uint32_t>& lastSendInputError() { static std::atomic<uint32_t> v{0}; return v; }
inline std::atomic<uint32_t>& lastError() { static std::atomic<uint32_t> v{0}; return v; }

// Access helpers
inline uint64_t load(std::atomic<uint64_t>& c) { return c.load(std::memory_order_relaxed); }
inline void inc(std::atomic<uint64_t>& c, uint64_t n=1) { c.fetch_add(n, std::memory_order_relaxed); }
inline void setLastError(uint32_t e) { lastSendInputError().store(e, std::memory_order_relaxed); lastError().store(e, std::memory_order_relaxed); }

} // namespace InputMetrics

#endif // INPUT_METRICS_H


