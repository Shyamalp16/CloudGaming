#pragma once

#include <cstdint>

namespace RuntimeMetrics {
struct Network {
    double packetLoss = 0.0;
    double rttMs = 0.0;
    double jitterMs = 0.0;
    std::uint32_t nackCount = 0;
    std::uint32_t pliCount = 0;
    std::uint32_t pacerQueueLength = 0;
    std::uint32_t sendBitrateKbps = 0;
};

void UpdateBasic(double packetLoss, double rttMs, double jitterSeconds) noexcept;
void UpdateEnhanced(double packetLoss, double rttMs, double jitterSeconds,
                    std::uint32_t nackCount, std::uint32_t pliCount,
                    std::uint32_t pacerQueueLength, std::uint32_t sendBitrateKbps) noexcept;
Network GetNetwork() noexcept;
}
