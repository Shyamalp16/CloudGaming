#pragma once

#include <atomic>
#include <cstdint>

namespace VideoMetrics {

// Capture/queue metrics
inline std::atomic<uint64_t>& overwriteDrops() { static std::atomic<uint64_t> v{0}; return v; }
inline std::atomic<uint64_t>& backpressureSkips() { static std::atomic<uint64_t> v{0}; return v; }
inline std::atomic<uint64_t>& outOfOrder() { static std::atomic<uint64_t> v{0}; return v; }
inline std::atomic<uint64_t>& queueDepth() { static std::atomic<uint64_t> v{0}; return v; }

// GPU timing (ms, last observed)
inline std::atomic<double>& vpGpuMs() { static std::atomic<double> v{0.0}; return v; }

// Helpers
inline uint64_t load(std::atomic<uint64_t>& c) { return c.load(std::memory_order_relaxed); }
inline void inc(std::atomic<uint64_t>& c, uint64_t n=1) { c.fetch_add(n, std::memory_order_relaxed); }

} // namespace VideoMetrics


