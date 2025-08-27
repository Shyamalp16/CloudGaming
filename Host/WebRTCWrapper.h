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
 * 2. enqueueDataChannelMessage() / enqueueMouseChannelMessage():
 *    - PARAMETERS: const std::string& - wrapper copies content
 *    - CALLER RESPONSIBILITY: None - wrapper handles copying
 *    - THREAD SAFETY: Thread-safe, uses internal synchronization
 *
 * 3. freeCString():
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
 *
 *     // Send response
 *     WebRTCWrapper::enqueueDataChannelMessage("acknowledged");
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
 * @brief Enqueue a message to the data channel
 *
 * This function safely copies the message content and enqueues it for sending
 * to the Go WebRTC layer.
 *
 * @param message The message to enqueue
 * @threadsafe
 */
void enqueueDataChannelMessage(const std::string& message);

/**
 * @brief Enqueue a mouse message to the mouse channel
 *
 * This function safely copies the message content and enqueues it for sending
 * to the Go WebRTC mouse channel.
 *
 * @param message The mouse message to enqueue
 * @threadsafe
 */
void enqueueMouseChannelMessage(const std::string& message);

// Forward declarations for internal use only
extern "C" {
    char* getDataChannelMessage();
    char* getMouseChannelMessage();
    void freeCString(char* p);
}

} // namespace WebRTCWrapper

#endif // WEBRTC_WRAPPER_H
