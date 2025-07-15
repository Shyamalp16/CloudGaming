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

1.  **Host (C++ & Go)**: A native Windows application that captures the screen, audio, and encodes them into a video stream. It also receives and simulates keyboard/mouse input from the client.
2.  **Signaling Server (Node.js)**: A lightweight server that acts as a matchmaker. It introduces the Host and Client to each other so they can negotiate a direct WebRTC connection. It does not handle any video or audio data itself.
3.  **Client (HTML/JS)**: A web-based application that connects to the Host, receives the video/audio stream, and sends user input back.

---

## Core Technologies

| Module | Language(s) | Key Libraries & Frameworks | Purpose |
| :--- | :--- | :--- | :--- |
| **Host** | C++, Go | **FFmpeg** (Encoding), **Pion WebRTC** (Go), **Windows Graphics Capture**, **D3D11** | Screen/audio capture, H.264 encoding, WebRTC session management. |
| **Server** | JavaScript | **Node.js**, **ws** (WebSocket library) | Signaling, matchmaking, and ICE candidate exchange. |
| **Client** | JavaScript | **HTML5**, **wrtc** (Node WebRTC, for client-side JS) | Renders video, captures user input, and manages the WebRTC connection. |

---

## Module Breakdown

### 1. Host (`/Host`)

The Host is the core of the streaming solution. It runs on the machine with the game or application to be streamed.

**Key Components:**
*   **`main.cpp`**: Entry point of the application. Initializes all components.
*   **Capture (`FrameCaptureThread`, `AudioCapturer`)**: Uses the modern `Windows.Graphics.Capture` API for high-performance, low-latency screen capture and `IAudioCaptureClient` for audio.
*   **Encoding (`Encoder.cpp`)**: Takes raw captured frames and uses the FFmpeg library (`libavcodec`) to encode them into the efficient **H.264** video format.
*   **WebRTC (`gortc_main/main.go`)**: A Go module, compiled into a C-shared library, that handles all the complexities of the WebRTC protocol using the excellent **Pion** library. It manages the peer connection, data channels, and ICE negotiation. C++ communicates with this Go layer via CGO.
*   **Input Handling (`KeyInputHandler`, `MouseInputHandler`)**: Receives keyboard and mouse events from the Client via WebRTC data channels and simulates them locally on the Host machine using `SendInput`.
*   **Signaling (`Websocket.cpp`)**: Connects to the Node.js Signaling Server to announce its availability and negotiate with a client.

### 2. Signaling Server (`/Server`)

This is a lightweight but essential component for establishing the P2P connection.

**Key Components:**
*   **`PureSignalingServer.js`**: A simple implementation that allows two peers (one Host, one Client) to find each other and exchange the necessary information to connect.
*   **`ScalableSignalingServer.js`**: (Presumed) A more advanced implementation for handling multiple rooms and concurrent sessions.
*   **Logic**: The server listens for WebSocket connections. When it receives a message from one peer (e.g., an "offer" from the Client), it forwards it to the other peer. This relaying of session descriptions and ICE candidates is its only job.

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

## Getting Started

*(This section is a template. You will need to fill in the specific build steps for your environment.)*

### Prerequisites
- **Host**: Windows 10/11, Visual Studio with C++ workload, Go, vcpkg (for dependencies like FFmpeg).
- **Server**: Node.js, npm.
- **Client**: A modern web browser.

### Running the Application
1.  **Start the Server**:
    ```bash
    cd Server
    npm install
    node PureSignalingServer.js
    ```
2.  **Build and Run the Host**:
    - Open `DisplayCaptureProject.sln` in Visual Studio.
    - Ensure all dependencies (FFmpeg, etc.) are correctly linked.
    - Build and run the project. It will connect to the server.
3.  **Open the Client**:
    - Open `Client/html-server/index.html` in a web browser.
    - Enter the Room ID that the Host is using.
    - Click "Join" to start the stream.