#pragma once
#ifndef WEBRTC_WRAPPER_H
#define WEBRTC_WRAPPER_H

#include <string>
#include <memory>

/**
 * @brief Safer C++ wrapper for WebRTC functionality with clear ownership contracts
 *
 * This wrapper provides:
 * - Clear ownership documentation for all memory operations
 * - Automatic memory management for C string allocations
 * - Exception-safe interfaces
 * - Stable ABI guarantees for cross-language interoperability
 *
 * MEMORY OWNERSHIP CONTRACTS:
 *
 * 1. getDataChannelMessageString() / getMouseChannelMessageString():
 *    - RETURNS: std::string with content copied from Go-allocated C string
 *    - CALLER RESPONSIBILITY: None - wrapper handles all memory cleanup
 *    - THREAD SAFETY: Safe to call from any thread
 *    - EXCEPTIONS: May throw std::runtime_error on allocation failures
 *
 * 2. freeCString():
 *    - INTERNAL USE ONLY: Called automatically by wrappers
 *    - EXTERNAL CALLS: Should never be made by application code
 *
 * USAGE EXAMPLE:
 * ```cpp
 * try {
 *     auto message = WebRTCWrapper::getDataChannelMessageString();
 *     if (!message.empty()) {
 *         // Process message - no memory management needed
 *         processInput(message);
 *     }
 * } catch (const std::exception& e) {
 *     LOG_ERROR("WebRTC operation failed: {}", e.what());
 * }
 * ```
 */

namespace WebRTCWrapper {

/**
 * @brief Get next data channel message as std::string
 *
 * This function safely retrieves the next message from the Go data channel,
 * automatically handling memory ownership transfer and cleanup.
 *
 * @return std::string containing the message, or empty string if no message available
 * @throws std::runtime_error if memory allocation fails
 * @threadsafe
 */
std::string getDataChannelMessageString();

/**
 * @brief Get next mouse channel message as std::string
 *
 * This function safely retrieves the next mouse message from the Go channel,
 * automatically handling memory ownership transfer and cleanup.
 *
 * @return std::string containing the message, or empty string if no message available
 * @throws std::runtime_error if memory allocation fails
 * @threadsafe
 */
std::string getMouseChannelMessageString();

/**
 * @brief Enhanced WebRTC stats callback function type
 *
 * Provides comprehensive network and congestion statistics for adaptive quality control.
 *
 * @param packetLoss Current packet loss percentage (0.0-1.0)
 * @param rtt Round-trip time in milliseconds
 * @param jitter Jitter in seconds
 * @param nackCount Total NACK (Negative ACK) packets received
 * @param pliCount Total PLI (Picture Loss Indication) packets received
 * @param twccCount Total TWCC (Transport-Wide Congestion Control) feedback packets
 * @param pacerQueueLength Current pacer queue length (packets buffered)
 * @param sendBitrateKbps Current send bitrate in Kbps
 */
using WebRTCStatsCallback = void(*)(double packetLoss, double rtt, double jitter,
                                   uint32_t nackCount, uint32_t pliCount, uint32_t twccCount,
                                   uint32_t pacerQueueLength, uint32_t sendBitrateKbps);

/**
 * @brief Set enhanced WebRTC stats callback
 *
 * Registers a callback to receive comprehensive WebRTC statistics for adaptive quality control.
 *
 * @param callback Function pointer to call with stats updates, or nullptr to disable
 * @threadsafe
 */
void setWebRTCStatsCallback(WebRTCStatsCallback callback);

} // namespace WebRTCWrapper

#endif // WEBRTC_WRAPPER_H
