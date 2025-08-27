#pragma once
#ifndef PION_WEBRTC_H
#define PION_WEBRTC_H

#include <cstdint>

/**
 * =======================================================================
 *                           ABI COMPATIBILITY GUARANTEES
 * =======================================================================
 *
 * This header maintains ABI compatibility across versions:
 * - Function signatures will not change without major version bump
 * - Structure layouts are fixed and will not change
 * - All functions are thread-safe unless explicitly documented otherwise
 * - Memory ownership contracts are clearly documented and stable
 *
 * =======================================================================
 *                        MEMORY OWNERSHIP CONTRACTS
 * =======================================================================
 *
 * 1. STRING MEMORY MANAGEMENT:
 *    All functions returning char* follow this contract:
 *    - ALLOCATOR: Go runtime (via C.CString())
 *    - OWNER: C++ caller becomes owner after function returns
 *    - RESPONSIBILITY: Caller MUST call freeCString() to deallocate
 *    - THREAD SAFETY: Each allocation is independent
 *    - EXCEPTIONS: Never thrown from C functions
 *
 * 2. STRING CONTENT:
 *    - ENCODING: UTF-8 always
 *    - TERMINATION: Null-terminated always
 *    - LIFETIME: Valid until freeCString() is called
 *    - MODIFICATION: Caller may read but should not modify
 *
 * 3. ERROR HANDLING:
 *    - RETURN VALUES: 0 for success, non-zero for failure
 *    - ERROR CODES: Function-specific, documented per function
 *    - THREAD SAFETY: Error state is per-call, not global
 *
 * 4. RECOMMENDED USAGE:
 *    Use WebRTCWrapper.h for safer C++ interface with automatic
 *    memory management instead of calling these functions directly.
 *
 * =======================================================================
 */

#ifdef __cplusplus
#include <string>
extern "C" {
#endif

    /**
     * @brief Initializes the Go runtime.
     */
    void initGo();

    /**
     * @brief Closes the Go runtime.
     */
    void closeGo();

    /**
     * @brief Creates a new WebRTC PeerConnection with H.264 support.
     * @return 1 on success, 0 on failure.
     */
    int createPeerConnectionGo();

    /**
     * @brief Handles an incoming SDP offer, sets it as the remote description, and creates an answer.
     * @param offerSDP The SDP offer string from the remote peer.
     */
    void handleOffer(const char* offerSDP);

    /**
     * @brief Retrieves the local SDP answer after handleOffer has been called.
     * @return A C-string containing the SDP answer, or NULL if not available. Caller must free the string.
     */
    char* getAnswerSDP();
    /**
     * @brief Frees memory allocated by Go runtime for C strings
     *
     * MEMORY OWNERSHIP CONTRACT:
     * - PURPOSE: Deallocates strings returned by getDataChannelMessage() and getMouseChannelMessage()
     * - PARAMETER: Pointer to string allocated by Go runtime
     * - RESPONSIBILITY: Must be called for every non-NULL string returned by message functions
     * - THREAD SAFETY: Thread-safe, can be called from any thread
     * - BEHAVIOR: Safe to call with NULL pointer (no-op)
     *
     * @param p Pointer to string to free, or NULL
     * @note This function calls C.free() on the underlying Go-allocated memory
     */
    void freeCString(char* p);

    /**
     * @brief Adds a remote ICE candidate to the PeerConnection.
     * @param candidateStr The ICE candidate string.
     */
    void handleRemoteIceCandidate(const char* candidateStr);

    // =======================================================================
    //                        MESSAGE CHANNEL FUNCTIONS
    // =======================================================================

    /**
     * @brief Gets the next message from the data channel
     *
     * MEMORY OWNERSHIP CONTRACT:
     * - ALLOCATOR: Go runtime allocates memory using C.CString()
     * - RETURNS: Pointer to null-terminated UTF-8 string, or NULL if no message
     * - CALLER RESPONSIBILITY: Must call freeCString() when done with the string
     * - THREAD SAFETY: Thread-safe, internally synchronized
     * - LIFETIME: Valid until freeCString() is called or program terminates
     *
     * @return Pointer to message string, or NULL if no message available
     * @note Caller must free the returned string using freeCString()
     * @note RECOMMENDED: Use WebRTCWrapper::getDataChannelMessageString() instead
     */
    char* getDataChannelMessage();

    /**
     * @brief Gets the next mouse message from the mouse channel
     *
     * MEMORY OWNERSHIP CONTRACT:
     * - ALLOCATOR: Go runtime allocates memory using C.CString()
     * - RETURNS: Pointer to null-terminated UTF-8 string, or NULL if no message
     * - CALLER RESPONSIBILITY: Must call freeCString() when done with the string
     * - THREAD SAFETY: Thread-safe, internally synchronized
     * - LIFETIME: Valid until freeCString() is called or program terminates
     *
     * @return Pointer to message string, or NULL if no message available
     * @note Caller must free the returned string using freeCString()
     * @note RECOMMENDED: Use WebRTCWrapper::getMouseChannelMessageString() instead
     */
    char* getMouseChannelMessage();

    /**
     * @brief Sends one encoded H.264 frame as a sample. Pion will packetize and pace.
     * @param data Pointer to Annex-B H.264 frame (include SPS/PPS on keyframes).
     * @param size Length of frame in bytes.
     * @param durationUs Frame duration in microseconds (e.g., 16667 for 60fps).
     * @return 0 on success, -1 on failure.
     */
    int sendVideoSample(uint8_t* data, int size, int64_t durationUs);

    /**
     * @brief Sends an Opus audio packet (RTP payload) to the WebRTC pipeline.
     * @param data Pointer to the Opus RTP payload data.
     * @param size Length of the payload data in bytes.
     * @param pts  Presentation timestamp in microseconds.
     * @return 0 on success, -1 on failure.
     */
    int sendAudioPacket(uint8_t* data, int size, int64_t pts);

    /**
     * @brief Enqueues a message to the data channel (Go-side enqueue)
     *
     * This function is called from C++ to send messages to the Go WebRTC layer.
     * The message content is copied by the Go runtime before this function returns.
     *
     * @param msg Pointer to null-terminated UTF-8 string to enqueue
     * @note This function is primarily for internal use by WebRTCWrapper
     */
    void enqueueDataChannelMessage(char* msg);

    /**
     * @brief Enqueues a mouse message to the mouse channel (Go-side enqueue)
     *
     * This function is called from C++ to send mouse messages to the Go WebRTC layer.
     * The message content is copied by the Go runtime before this function returns.
     *
     * @param msg Pointer to null-terminated UTF-8 string to enqueue
     * @note This function is primarily for internal use by WebRTCWrapper
     */
    void enqueueMouseChannelMessage(char* msg);

    /**
     * @brief Gets the current ICE connection state of the PeerConnection.
     * @return The ICE connection state as an integer (-1 if no PeerConnection).
     *         See webrtc::ICEConnectionState enum for values (e.g., 4 = Connected).
     */
    int getIceConnectionState();

    /**
     * @brief Gets the current PeerConnection state.
     * @return The PeerConnection state as an integer.
     */
    int getPeerConnectionState();

    /**
     * @brief Closes the PeerConnection and cleans up resources.
     */
    void closePeerConnection();

    /**
     * @brief Callback function for ICE candidates, to be implemented in C++.
     * @param candidate The ICE candidate string to be sent via signaling.
     */
    void onIceCandidate(const char* candidate);

    /**
     * @brief Sets the callback function for RTCP statistics.
     * @param callback A function pointer to handle RTCP data (packet loss, RTT, jitter).
     */
    typedef void (*RTCPCallback)(double, double, double);
    void SetRTCPCallback(RTCPCallback callback);

    /**
     * @brief Sets the callback invoked on incoming RTCP PLI/FIR to request a keyframe.
     */
    typedef void (*OnPLICallback)();
    void SetPLICallback(OnPLICallback cb);

    // C wrapper functions for memory management
    void freeDataChannelMessage(char* msg);
    void freeMouseChannelMessage(char* msg);

    // Wake functions for blocking queue signaling (Go -> C++)
    void wakeKeyboardThread();
    void wakeMouseThread();

#ifdef __cplusplus
} // end extern "C"

// =======================================================================
//               DEPRECATED FUNCTIONS (Use WebRTCWrapper.h instead)
// =======================================================================

/**
 * @brief DEPRECATED: Use WebRTCWrapper::getDataChannelMessageString() instead
 *
 * This function is maintained for backward compatibility but should not be used
 * in new code. Use WebRTCWrapper.h for safer memory management.
 *
 * @deprecated Use WebRTCWrapper::getDataChannelMessageString() for safer memory management
 * @return std::string containing the message
 */
std::string getDataChannelMessageString();

/**
 * @brief DEPRECATED: Use WebRTCWrapper::getMouseChannelMessageString() instead
 *
 * This function is maintained for backward compatibility but should not be used
 * in new code. Use WebRTCWrapper.h for safer memory management.
 *
 * @deprecated Use WebRTCWrapper::getMouseChannelMessageString() for safer memory management
 * @return std::string containing the message
 */
std::string getMouseChannelMessageString();

#endif

#endif // PION_WEBRTC_H