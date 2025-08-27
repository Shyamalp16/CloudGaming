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

// Last error codes
inline std::atomic<uint32_t>& lastSendInputError() { static std::atomic<uint32_t> v{0}; return v; }

// Access helpers
inline uint64_t load(std::atomic<uint64_t>& c) { return c.load(std::memory_order_relaxed); }
inline void inc(std::atomic<uint64_t>& c, uint64_t n=1) { c.fetch_add(n, std::memory_order_relaxed); }
inline void setLastError(uint32_t e) { lastSendInputError().store(e, std::memory_order_relaxed); }

} // namespace InputMetrics

#endif // INPUT_METRICS_H


