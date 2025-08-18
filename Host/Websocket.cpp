#include "Websocket.h"
#include "Encoder.h"
#include <chrono> // For high_resolution_clock
#include "KeyInputHandler.h"
#include "MouseInputHandler.h"
#include "ShutdownManager.h"
#include "PacketQueue.h"

typedef websocketpp::client<websocketpp::config::asio_client> client;
using json = nlohmann::json;

client wsClient;
websocketpp::connection_hdl g_connectionHandle;
std::string base_uri = "ws://localhost:3002/";
std::thread g_websocket_thread;
std::thread g_frame_thread;
std::thread g_sender_thread;

void on_open(client* c, websocketpp::connection_hdl hdl);
void on_fail(client* c, websocketpp::connection_hdl hdl);
void on_close(client* c, websocketpp::connection_hdl hdl);
void on_message(websocketpp::connection_hdl hdl, client::message_ptr msg);
void send_message(const json& message);

void senderThread() {
    while (!ShutdownManager::IsShutdown()) {
        Packet packet;
        if (g_packetQueue.pop(packet)) {
            if (getIceConnectionState() >= 0) {
                if (!packet.data.empty() && packet.data.data() != nullptr) {
                    // If this legacy path is re-enabled, compute from encoder fps instead of 60 fps
                    int64_t frameDurationUs = 1000000 / 120; // ~8333us at 120fps
                    int result = sendVideoSample(packet.data.data(), static_cast<int>(packet.data.size()), frameDurationUs);
                    // std::cerr << "[SenderThread] Failed to send video packet: " << result << std::endl;
                    }
                    else {
                        // std::cout << "[SenderThread] Sent video packet successfully (PTS: " << packet.pts << ")" << std::endl;
                    }
                }
            }
        }
    }

void sendFrames() {
    while (!ShutdownManager::IsShutdown()) {
        std::vector<uint8_t> frameData;
        int64_t pts = 0;

        if (Encoder::getEncodedFrame(frameData, pts)) {
            Packet packet;
            packet.data = frameData;
            packet.pts = pts;
            g_packetQueue.push(packet);
        }
    }
}

bool createPeerConnection() {
    if (createPeerConnectionGo() == 0) {
        std::cerr << "[C++ Host] Error creating peer connection" << std::endl;
        return false;
    }
    std::cout << "[C++ Host] Peer Connection Created." << std::endl;
    return true;
}

void sendAnswer() {
    char* sdp = getAnswerSDP();
    if (!sdp) {
        std::cerr << "[WebSocket] Error getting answer SDP\n";
        return;
    }
    json answerMsg;
    answerMsg["type"] = "answer";
    answerMsg["sdp"] = std::string(sdp);
    send_message(answerMsg);
    std::cout << "[WebSocket] Answer sent with SDP: " << answerMsg.dump() << "\n";
    //free(sdp); // Free C string allocated by Go // UNCOMMENTING THIS WILL CRASH AT EVERY RUN 
}

void handleOffer(const std::string& offer) {
    if (!createPeerConnection()) return;
    handleOffer(offer.c_str());
    sendAnswer(); // Trigger sending the answer
    initKeyInputHandler();
    //initMouseInputHandler();
}

void handleRemoteIceCandidate(const json& candidateJson) {
    std::string candidateStr = candidateJson.value("candidate", "");
    handleRemoteIceCandidate(candidateStr.c_str());
}

void on_open(client* c, websocketpp::connection_hdl hdl) {
    std::cout << "[WebSocket] Connected opened" << std::endl;
    g_connectionHandle = hdl;
}

void on_fail(client* c, websocketpp::connection_hdl hdl) {
    client::connection_ptr con = c->get_con_from_hdl(hdl);
    std::cerr << "[WebSocket] Connection failed: " << con->get_ec().message() << std::endl;
}

void on_close(client* c, websocketpp::connection_hdl hdl) {
    std::cout << "[WebSocket] Connection closed" << std::endl;
    // Do not propagate Shutdown here; allow manual Stop/Close order only
}

void on_message(websocketpp::connection_hdl hdl, client::message_ptr msg) {
    try {
        json message = json::parse(msg->get_payload());

        if (!message.contains("type") || !message["type"].is_string()) {
            std::cerr << "[WebSocket] Received message without a valid 'type' field: " << message.dump() << std::endl;
            return; // Skip processing this message
        }

        std::string type = message["type"];
        std::cout << "[Host] Received message type: " << type << std::endl;

        if (type == "ping") {
            long long client_timestamp = message["timestamp"].get<long long>();
            int sequence_number = message["sequence_number"].get<int>();
            auto host_receive_time_point = std::chrono::high_resolution_clock::now();
            long long host_receive_time_ms = std::chrono::duration_cast<std::chrono::milliseconds>(host_receive_time_point.time_since_epoch()).count();

            json pong_response;
            pong_response["type"] = "pong";
            pong_response["timestamp"] = client_timestamp; // Original client timestamp
            pong_response["sequence_number"] = sequence_number;
            pong_response["host_receive_time"] = host_receive_time_ms; // Host's receive time

            send_message(pong_response);

            std::cout << "[Host] Received ping " << sequence_number
                      << " from client at " << client_timestamp
                      << ", host receive time: " << host_receive_time_ms << std::endl;
        }
        else if (type == "keydown" || type == "keyup" || type == "mousemove" || type == "mousedown" || type == "mouseup") {
            // Validate and rate-limit input events
            static auto lastInputLog = std::chrono::steady_clock::now();
            auto now = std::chrono::steady_clock::now();
            auto msSince = std::chrono::duration_cast<std::chrono::milliseconds>(now - lastInputLog).count();

            // Validate timestamp
            long long client_send_time = 0;
            if (message.contains("client_send_time") && message["client_send_time"].is_number()) {
                client_send_time = message["client_send_time"].get<long long>();
            }
            auto host_receive_time_point = std::chrono::high_resolution_clock::now();
            long long host_receive_time_ms = std::chrono::duration_cast<std::chrono::milliseconds>(host_receive_time_point.time_since_epoch()).count();

            long long one_way_latency = client_send_time > 0 ? (host_receive_time_ms - client_send_time) : -1;

            if (msSince >= 200) { // log at most every 200ms to avoid spam
                std::cout << "[Host] Received " << type << " event."
                          << " Client send time: " << client_send_time
                          << ", Host receive time: " << host_receive_time_ms
                          << ", One-way latency: " << one_way_latency << " ms" << std::endl;
                lastInputLog = now;
            }

            // Basic field validation for mouse events
            if (type == "mousemove" || type == "mousedown" || type == "mouseup") {
                if (!message.contains("x") || !message["x"].is_number_integer() ||
                    !message.contains("y") || !message["y"].is_number_integer()) {
                    std::cerr << "[WebSocket] Invalid mouse event payload: missing or bad x/y" << std::endl;
                    return;
                }
                int x = message["x"].get<int>();
                int y = message["y"].get<int>();
                if (x < 0 || y < 0 || x > 8192 || y > 8192) { // simple sanity clamp
                    std::cerr << "[WebSocket] Mouse coordinates out of expected range" << std::endl;
                    return;
                }
                if ((type == "mousedown" || type == "mouseup")) {
                    if (!message.contains("button") || !message["button"].is_number_integer()) {
                        std::cerr << "[WebSocket] Invalid mouse click payload: missing button" << std::endl;
                        return;
                    }
                    int button = message["button"].get<int>();
                    if (button < 0 || button > 2) {
                        std::cerr << "[WebSocket] Mouse button out of range" << std::endl;
                        return;
                    }
                }
            }
        }
        else if (type == "peer-disconnected") {
            std::cout << "[WebSocket] Peer has disconnected. Keeping host alive and closing PeerConnection only." << std::endl;
            try { closePeerConnection(); } catch (...) {}
        }
        else if (type == "offer") {
            std::cout << "[WebSocket] Received offer from server: \n" << message.dump() << std::endl;
            std::string sdp = message.value("sdp", "");
            handleOffer(sdp);
        }
        else if (type == "ice-candidate") {
            std::cout << "[WebSocket] Received ice candidate from server: " << message.dump() << std::endl;
            json candidateJson = message["candidate"];
            handleRemoteIceCandidate(candidateJson);
        }
        else {
            std::cout << "[WebSocket] Received Message: " << message.dump() << std::endl;
        }
    }
    catch (const std::exception& e) {
        std::cerr << "[WebSocket] Error parsing message: " << e.what() << std::endl;
    }
}

void send_message(const json& message) {
    try {
        if (!g_connectionHandle.lock()) {
            std::cerr << "[WebSocket] Cannot send message: Connection is invalid\n";
            return;
        }
        std::string payload = message.dump();
        wsClient.send(g_connectionHandle, payload, websocketpp::frame::opcode::text);
        std::cout << "[WebSocket] Sent message: " << payload << std::endl;
    }
    catch (const std::exception& e) {
        std::cerr << "[WebSocket] Error sending message: " << e.what() << std::endl;
    }
}

void initWebsocket(const std::string& roomId) {
    wsClient.init_asio();
    wsClient.set_open_handler(std::bind(&on_open, &wsClient, std::placeholders::_1));
    wsClient.set_message_handler(&on_message);
    wsClient.set_fail_handler(std::bind(&on_fail, &wsClient, std::placeholders::_1));
    wsClient.set_close_handler(std::bind(&on_close, &wsClient, std::placeholders::_1));

    std::string full_uri = base_uri + "?roomId=" + roomId;
    std::cout << "[WebSocket] Connecting to " << full_uri << std::endl;

    websocketpp::lib::error_code ec;
    client::connection_ptr con = wsClient.get_connection(full_uri, ec);
    if (ec) {
        std::cerr << "[WebSocket] Error getting connection: " << ec.message() << std::endl;
        return;
    }
    wsClient.connect(con);

    g_websocket_thread = std::thread([&]() {
        try {
            wsClient.run();
        }
        catch (const std::exception& ex) {
            std::cerr << "[WebSocket] run() Threw Exception: " << ex.what() << std::endl;
        }
        });

    // Video is sent directly from Encoder::pushPacketToWebRTC -> sendVideoPacket.
    // Disable legacy queue-based sender threads to avoid duplicate RTP sends.
    // g_frame_thread = std::thread(&sendFrames);
    // g_sender_thread = std::thread(&senderThread);
}

void stopWebsocket() {
    static std::atomic<bool> stopped{ false };
    bool expected = false;
    if (!stopped.compare_exchange_strong(expected, true)) {
        std::wcout << L"[Shutdown] stopWebsocket already executed. Skipping.\n";
        return;
    }

    std::wcout << L"[Shutdown] Initiating websocket shutdown...\n";
    // Signal encoder loops to stop producing frames
    Encoder::SignalEncoderShutdown();
    g_packetQueue.shutdown();

    // Close the PeerConnection first to stop RTP/data traffic gracefully
    try {
        closePeerConnection();
    } catch (...) {
        std::wcout << L"[Shutdown] Exception during closePeerConnection (ignored).\n";
    }

    std::wcout << L"[Shutdown] Stopping websocket client...\n";
    try {
        wsClient.stop();
    } catch (...) {
        std::wcout << L"[Shutdown] Exception during wsClient.stop() (ignored).\n";
    }

    std::wcout << L"[Shutdown] Joining websocket thread...\n";
    if (g_websocket_thread.joinable()) {
        g_websocket_thread.join();
    }
    std::wcout << L"[Shutdown] Websocket thread joined.\n";

    std::wcout << L"[Shutdown] Joining frame thread...\n";
    if (g_frame_thread.joinable()) {
        g_frame_thread.join();
    }
    std::wcout << L"[Shutdown] Frame thread joined.\n";

    std::wcout << L"[Shutdown] Joining sender thread...\n";
    if (g_sender_thread.joinable()) {
        g_sender_thread.join();
    }
    std::wcout << L"[Shutdown] Sender thread joined.\n";

    std::wcout << L"[Shutdown] Websocket shutdown complete.\n";
}

// Callback from Go to send ICE candidates
extern "C" void onIceCandidate(const char* candidate) {
    json iceMsg;
    iceMsg["type"] = "ice-candidate";
    iceMsg["candidate"] = std::string(candidate);
    send_message(iceMsg);
    std::cout << "[WebSocket] Sent ICE candidate: " << iceMsg.dump() << std::endl;
}

extern "C" char* getDataChannelMessage() {
    // This function is called from Go to get a message from the data channel
    // For now, it's just a placeholder
    return nullptr;
}

extern "C" char* getMouseChannelMessage() {
    // This function is called from Go to get a message from the data channel
    // For now, it's just a placeholder
    return nullptr;
}