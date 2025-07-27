# P2P Cloud Gaming & Remote Desktop

This project is a high-performance, peer-to-peer (P2P) solution for cloud gaming and remote desktop streaming. It allows a user to stream gameplay or a desktop session from a powerful "Host" machine to a lightweight "Client" machine with low latency, using a custom-built architecture that leverages WebRTC for direct P2P communication.

## Architecture Overview

The system is composed of three main components that work together to establish a streaming session:

```
+-------------+      (1) WebSocket      +-----------------+      (2) WebSocket      +--------+
|             | <---------------------> |                 | <---------------------> |        |
|   Client    |      Signaling (JSON)   | Signaling Server|      Signaling (JSON)   |  Host  |
| (Browser)   |                         |    (Node.js)    |                         |  (C++) |
|             | <-----------------------------------------------------------------> |        |
+-------------+                         +-----------------+                         +--------+
      ^                                                                                 ^
      |                                                                                 |
      | (3) WebRTC P2P Connection (Video, Audio, Input Data Channels)                   |
      +---------------------------------------------------------------------------------+

```

1.  **Host (C++)**: A native Windows application that captures the screen, audio, and encodes them into a video stream. It also receives and simulates keyboard/mouse input from the client.
2.  **Signaling Server (Node.js)**: A lightweight server that acts as a matchmaker. It introduces the Host and Client to each other so they can negotiate a direct WebRTC connection. It does not handle any video or audio data itself.
3.  **Client (HTML/JS)**: A web-based application that connects to the Host, receives the video/audio stream, and sends user input back.

---

## Core Technologies

| Module | Language(s) | Key Libraries & Frameworks | Purpose |
| :--- | :--- | :--- | :--- |
| **Host** | C++ | **FFmpeg** (Encoding), **Pion WebRTC** (Go), **WinRT/C++**, **DirectX 11** | Screen/audio capture, H.264 encoding, WebRTC session management. |
| **Server** | JavaScript | **Node.js**, **ws** (WebSocket library), **Redis** | Signaling, matchmaking, and ICE candidate exchange. |
| **Client** | JavaScript | **HTML5**, **wrtc** (Node WebRTC, for client-side JS) | Renders video, captures user input, and manages the WebRTC connection. |

---

## Module Breakdown

### 1. Host (`/Host`)

The Host is the core of the streaming solution. It runs on the machine with the game or application to be streamed.

**Key Components:**
*   **`main.cpp`**: Entry point of the application. Initializes all components.
*   **Capture (`CaptureHelpers.cpp`, `AudioHelper.cpp`)**: Uses the modern `Windows.Graphics.Capture` API for high-performance, low-latency screen capture and `winrt::Windows::Media::Audio::AudioGraph` for audio.
*   **Encoding (`Encoder.cpp`)**: Takes raw captured frames and uses the FFmpeg library (`libavcodec`) to encode them into the efficient **H.264** video format. It dynamically selects the best available hardware encoder (NVIDIA's NVENC, AMD's AMF, or Intel's QSV).
*   **WebRTC (`gortc_main/main.go`)**: A Go module, compiled into a C-shared library, that handles all the complexities of the WebRTC protocol using the excellent **Pion** library. It manages the peer connection, data channels, and ICE negotiation. C++ communicates with this Go layer via CGO, using the functions defined in `pion_webrtc.h`.
*   **Input Handling (`KeyInputHandler`, `MouseInputHandler`)**: Receives keyboard and mouse events from the Client via WebRTC data channels and simulates them locally on the Host machine using `SendInput`.
*   **Signaling (`Websocket.cpp`)**: Connects to the Node.js Signaling Server to announce its availability and negotiate with a client.

### 2. Signaling Server (`/Server`)

This is a lightweight but essential component for establishing the P2P connection. The server is designed to be stateless, allowing multiple instances to run behind a load balancer for high availability and scalability.

**Key Components:**
*   **`PureSignalingServer.js`**: A simple implementation that allows two peers (one Host, one Client) to find each other and exchange the necessary information to connect.
*   **`ScalableSignalingServer.js`**: A more advanced, stateless implementation that uses **Redis** to store all room and session state. It uses a single, pattern-based Redis subscription (`psubscribe`) to efficiently handle messaging for all rooms, eliminating the need for per-client subscriptions and allowing for seamless scaling.
*   **`SecureSignalingServer.js`**: A room-based signaling server that provides more robust session management.
*   **Logic**: The server listens for WebSocket connections. When a client connects, it is added to a room. When it sends a message, the message is forwarded to the other peer in the room.

### 3. Client (`/Client`)

The Client is a simple web page that allows a user to connect to a Host and start streaming.

**Key Components:**
*   **`index.html`**: The main HTML structure, including a video element to render the stream.
*   **`PeerClient.js`**: Contains all the client-side logic for:
    *   Connecting to the Signaling Server.
    *   Creating a `RTCPeerConnection`.
    *   Creating an "offer" to send to the Host.
    *   Processing the "answer" from the Host.
    *   Handling ICE candidates to find the best P2P path.
    *   Receiving the remote video track and attaching it to the `<video>` element.
    *   Capturing keyboard/mouse events and sending them over a `RTCDataChannel`.

---

## How It Works: The Connection Flow

1.  The **Host** application is started. It connects to the **Signaling Server** via WebSocket and waits in a "room".
2.  The user opens the **Client** web page and enters the same room ID. The Client also connects to the Signaling Server.
3.  The Signaling Server now knows about both peers in the room and can relay messages between them.
4.  The **Client** creates a WebRTC "offer" (a description of its desired media session) and sends it to the server.
5.  The **Server** forwards this offer to the **Host**.
6.  The **Host** receives the offer, creates an "answer," and sends it back to the **Client** via the server.
7.  Simultaneously, both Client and Host are gathering **ICE candidates** (potential IP addresses and ports) and exchanging them through the server.
8.  Once they have exchanged the offer/answer and enough ICE candidates, a direct **P2P connection** is established between the Client and Host.
9.  The **Host** begins capturing, encoding, and streaming H.264 video frames directly to the **Client**.
10. The **Client** begins receiving the video stream and sending user input back to the Host. The Signaling Server is no longer needed for this session.

## Peer Disconnection Handling

The system now properly handles peer disconnection. When a client closes their browser or the connection is otherwise interrupted, the following occurs:

1.  The **Signaling Server** detects the closed WebSocket connection.
2.  It sends a `peer-disconnected` message to the other peer in the room (the **Host**).
3.  The **Host** receives this message and initiates a graceful shutdown, closing the PeerConnection, stopping the capture and encoding threads, and releasing all resources.
4.  On the **Client** side, if the connection is lost, it will display a "Connection Lost" message and attempt to reconnect. If the host disconnects, the client will be notified and will not attempt to reconnect.

---

## Dynamic Bitrate for Adaptive Streaming

To provide a smooth experience even under changing network conditions, the Host implements a dynamic bitrate system that adapts the video quality in real-time. This prevents stream stuttering and freezing on weaker or unstable networks.

### Technical Implementation: The Feedback Loop

The system works by creating a continuous feedback loop between the Client and the Host.

1.  **Client Sends Feedback (RTCP):**
    *   The user's web browser, while receiving the video, automatically sends **RTCP (RTP Control Protocol) Receiver Reports** back to the Host. This is a standard part of the WebRTC protocol.
    *   These reports contain crucial statistics, most importantly **Packet Loss** (the percentage of video packets that never arrived) and **Jitter** (the variation in packet arrival times).

2.  **Go/Pion Intercepts the Feedback:**
    *   In `gortc_main/main.go`, an **RTCP Interceptor** (`rtcpReaderInterceptor`) is registered with the Pion WebRTC stack.
    *   This interceptor's job is to "catch" these incoming RTCP reports, open them, and extract the packet loss and jitter values.

3.  **Go Calls the C++ Callback:**
    *   The Go interceptor then calls a C function pointer that was registered by the C++ application.
    *   This call crosses the language boundary from Go to C++, passing the network statistics as arguments.

4.  **C++ Logic Makes a Decision:**
    *   The call arrives in `main.cpp` at the `onRTCP` function, which contains the control logic:
        ```cpp
        // If packet loss is high, reduce bitrate.
        if (packetLoss > 0.05) {
            currentBitrate *= 0.8; // Decrease bitrate by 20%
            Encoder::AdjustBitrate(currentBitrate);
        } 
        // If packet loss is low, increase bitrate for better quality.
        else if (packetLoss < 0.02) {
            currentBitrate *= 1.1; // Increase bitrate by 10%
            Encoder::AdjustBitrate(currentBitrate);
        }
        ```

5.  **The Encoder Adjusts its Target:**
    *   The `onRTCP` function calls `Encoder::AdjustBitrate()`, passing the newly calculated target bitrate.
    *   Inside `Encoder.cpp`, this function safely updates the `bit_rate` property of the FFmpeg `AVCodecContext`.
    *   From this point forward, the hardware encoder will compress the video stream to match this new, adjusted bitrate.

This entire process runs continuously, allowing the stream to adapt to changing network conditions in near real-time, ensuring the best possible quality and smoothness.