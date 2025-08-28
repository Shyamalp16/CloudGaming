#pragma once

#include <atomic>
#include <cstdint>

namespace VideoMetrics {

// Capture/queue metrics
inline std::atomic<uint64_t>& overwriteDrops() { static std::atomic<uint64_t> v{0}; return v; }
inline std::atomic<uint64_t>& backpressureSkips() { static std::atomic<uint64_t> v{0}; return v; }
inline std::atomic<uint64_t>& outOfOrder() { static std::atomic<uint64_t> v{0}; return v; }
inline std::atomic<uint64_t>& queueDepth() { static std::atomic<uint64_t> v{0}; return v; }
inline std::atomic<uint64_t>& eagainEvents() { static std::atomic<uint64_t> v{0}; return v; }
inline std::atomic<uint64_t>& sendQueueDrops() { static std::atomic<uint64_t> v{0}; return v; }
// Depth of the encoder->Go sender queue
inline std::atomic<uint64_t>& sendQueueDepth() { static std::atomic<uint64_t> v{0}; return v; }

// GPU timing (ms, last observed)
inline std::atomic<double>& vpGpuMs() { static std::atomic<double> v{0.0}; return v; }

// EWMAs and last-observed timings (low overhead)
inline std::atomic<double>& captureFps() { static std::atomic<double> v{0.0}; return v; }
inline std::atomic<double>& captureToEnqueueUsAvg() { static std::atomic<double> v{0.0}; return v; }
inline std::atomic<double>& avSendMsAvg() { static std::atomic<double> v{0.0}; return v; }
inline std::atomic<double>& packetsPerFrameAvg() { static std::atomic<double> v{0.0}; return v; }
inline std::atomic<double>& ffiSendLatencyMsAvg() { static std::atomic<double> v{0.0}; return v; }

// Helpers
inline uint64_t load(std::atomic<uint64_t>& c) { return c.load(std::memory_order_relaxed); }
inline void inc(std::atomic<uint64_t>& c, uint64_t n=1) { c.fetch_add(n, std::memory_order_relaxed); }

// Update an EWMA value with smoothing factor alpha (default 0.1)
inline void ewmaUpdate(std::atomic<double>& metric, double sample, double alpha = 0.1) {
    double prev = metric.load(std::memory_order_relaxed);
    double next = (prev <= 0.0) ? sample : (prev * (1.0 - alpha) + sample * alpha);
    metric.store(next, std::memory_order_relaxed);
}

} // namespace VideoMetrics


