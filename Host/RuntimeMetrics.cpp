#include "RuntimeMetrics.h"

#include <atomic>

namespace RuntimeMetrics {
namespace {
std::atomic<double> g_packetLoss{0.0};
std::atomic<double> g_rttMs{0.0};
std::atomic<double> g_jitterMs{0.0};
std::atomic<std::uint32_t> g_nackCount{0};
std::atomic<std::uint32_t> g_pliCount{0};
std::atomic<std::uint32_t> g_pacerQueueLength{0};
std::atomic<std::uint32_t> g_sendBitrateKbps{0};
}

void UpdateBasic(double packetLoss, double rttMs, double jitterSeconds) noexcept {
    g_packetLoss.store(packetLoss, std::memory_order_relaxed);
    g_rttMs.store(rttMs, std::memory_order_relaxed);
    g_jitterMs.store(jitterSeconds * 1000.0, std::memory_order_relaxed);
}

void UpdateEnhanced(double packetLoss, double rttMs, double jitterSeconds,
                    std::uint32_t nackCount, std::uint32_t pliCount,
                    std::uint32_t pacerQueueLength, std::uint32_t sendBitrateKbps) noexcept {
    UpdateBasic(packetLoss, rttMs, jitterSeconds);
    g_nackCount.store(nackCount, std::memory_order_relaxed);
    g_pliCount.store(pliCount, std::memory_order_relaxed);
    g_pacerQueueLength.store(pacerQueueLength, std::memory_order_relaxed);
    g_sendBitrateKbps.store(sendBitrateKbps, std::memory_order_relaxed);
}

Network GetNetwork() noexcept {
    return {g_packetLoss.load(std::memory_order_relaxed), g_rttMs.load(std::memory_order_relaxed),
        g_jitterMs.load(std::memory_order_relaxed), g_nackCount.load(std::memory_order_relaxed),
        g_pliCount.load(std::memory_order_relaxed), g_pacerQueueLength.load(std::memory_order_relaxed),
        g_sendBitrateKbps.load(std::memory_order_relaxed)};
}
}
