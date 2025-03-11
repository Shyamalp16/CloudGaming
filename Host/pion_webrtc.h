#pragma once
#ifndef PION_WEBRTC_H
#define PION_WEBRTC_H

#ifdef __cplusplus
extern "C" {
#endif

    /**
     * @brief Creates a new WebRTC PeerConnection.
     * @return 1 on success, 0 on failure.
     */
    int createPeerConnectionGo();

    /**
     * @brief Handles an incoming SDP offer and creates an answer.
     * @param offerSDP The SDP offer string from the remote peer.
     */
    void handleOffer(const char* offerSDP);

    /**
     * @brief Adds a remote ICE candidate to the PeerConnection.
     * @param candidateStr The ICE candidate string.
     */
    void handleRemoteIceCandidate(const char* candidateStr);

    /**
     * @brief Sends the local SDP answer (to be handled by C++ for WebSocket transmission).
     */
    void sendAnswerGo();

    char* getAnswerSDP();

    /**
     * @brief Sends a frame of video data to the WebRTC pipeline.
     * @param frameData Pointer to the frame data.
     * @param frameLen Length of the frame data in bytes.
     */
    void sendFrame(const char* frameData, int frameLen);

    /**
     * @brief Gets the current ICE connection state of the PeerConnection.
     * @return The ICE connection state as an integer (-1 if no PeerConnection).
     */
    int getIceConnectionState();

#ifdef __cplusplus
}
#endif

#endif // PION_WEBRTC_H