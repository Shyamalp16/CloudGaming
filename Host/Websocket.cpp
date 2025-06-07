#include "Websocket.h"
#include "Encoder.h"
#include "KeyInputHandler.h"
#include "MouseInputHandler.h"

typedef websocketpp::client<websocketpp::config::asio_client> client;
using json = nlohmann::json;

client wsClient;
websocketpp::connection_hdl g_connectionHandle;
//std::string uri = "ws://10.0.0.134:3000";
std::string uri = "ws://localhost:3000";

void on_open(client* c, websocketpp::connection_hdl hdl);
void on_message(websocketpp::connection_hdl hdl, client::message_ptr msg);
void send_message(const json& message);

void sendFrames() {
    while (true) {
        std::vector<uint8_t> frameData;
        int64_t pts = 0;

        // getEncodedFrame will block until a frame is ready due to condition variable
        if (Encoder::getEncodedFrame(frameData, pts)) {
            // Check if PeerConnection is initialized
            // Note: getIceConnectionState() might not be the best check,
            // maybe check peerConnection.ConnectionState() == webrtc.PeerConnectionStateConnected in Go?
            // For now, assuming getIceConnectionState >= 0 means ready enough.
            if (getIceConnectionState() >= 0) { 
                if (!frameData.empty() && frameData.data() != nullptr) {
                    // Pass data pointer and size directly
                    int result = sendVideoPacket(frameData.data(), static_cast<int>(frameData.size()), pts);
                    if (result != 0) {
                        std::cerr << "[WebSocket] Failed to send video packet: " << result << std::endl;
                    }
                    else {
                         std::cout << "[WebSocket] Sent video packet successfully (PTS: " << pts << ")" << std::endl;
                    }
                }
                else {
                    std::cerr << "[WebSocket] Invalid frame data buffer retrieved" << std::endl;
                }
            }
            else {
                 std::cout << "[WebSocket] Waiting for PeerConnection to initialize..." << std::endl;
                 //Avoid busy-waiting if PC is not ready, sleep a bit longer
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
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
    initMouseInputHandler();
}

void handleRemoteIceCandidate(const json& candidateJson) {
    std::string candidateStr = candidateJson.value("candidate", "");
    handleRemoteIceCandidate(candidateStr.c_str());
}

void on_open(client* c, websocketpp::connection_hdl hdl) {
    std::cout << "[WebSocket] Connected opened to " << uri << std::endl;
    g_connectionHandle = hdl;
}

void on_message(websocketpp::connection_hdl hdl, client::message_ptr msg) {
    try {
        json message = json::parse(msg->get_payload());
        std::string type = message["type"];

        if (type == "offer") {
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

void initWebsocket() {
    wsClient.init_asio();
    wsClient.set_open_handler(std::bind(&on_open, &wsClient, std::placeholders::_1));
    wsClient.set_message_handler(&on_message);

    websocketpp::lib::error_code ec;
    client::connection_ptr con = wsClient.get_connection(uri, ec);
    if (ec) {
        std::cerr << "[WebSocket] Error connecting to " << uri << ": " << ec.message() << std::endl;
        return;
    }
    wsClient.connect(con);
    std::wcout << L"[WebSocket] Connection made\n";

    std::thread t([&]() {
        try {
            wsClient.run();
        }
        catch (const std::exception& ex) {
            std::cerr << "[WebSocket] run() Threw Exception: " << ex.what() << std::endl;
        }
        });
    t.detach();
    std::wcout << L"[WebSocket] Websocket started in a separate thread\n";

    std::thread frameThread(&sendFrames);
    frameThread.detach();
}