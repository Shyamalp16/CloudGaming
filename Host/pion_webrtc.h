#pragma once
#ifndef PION_WEBRTC_H
#define PION_WEBRTC_H

#ifdef __cplusplus
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
     * @brief Adds a remote ICE candidate to the PeerConnection.
     * @param candidateStr The ICE candidate string.
     */
    void handleRemoteIceCandidate(const char* candidateStr);

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

    char* getDataChannelMessage();

    char* getMouseChannelMessage();

#ifdef __cplusplus
}
#endif

#endif // PION_WEBRTC_H