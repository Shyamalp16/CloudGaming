#include "Websocket.h"
#include "pion_webrtc.h"
//#include "MySetRemoteObserver.h"
//#include "MyCreateSdpObserver.h"

typedef websocketpp::client<websocketpp::config::asio_client> client;
using json = nlohmann::json;

client wsClient;
websocketpp::connection_hdl g_connectionHandle;
std::string uri = "ws://localhost:3000";

static bool answerCreated = false;

void on_open(client* c, websocketpp::connection_hdl hdl);
void on_message(websocketpp::connection_hdl hdl, client::message_ptr msg);
void send_message(const json& message);

void sendFrames() {
    while (true) {
        // Fetch frames in real-time (replace with your frame fetching logic)
        std::vector<char> frameData(1024); // Placeholder
        sendFrame(frameData.data(), frameData.size());
        std::this_thread::sleep_for(std::chrono::milliseconds(33)); // ~30 FPS
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
    sendAnswerGo();

    char* sdp = getAnswerSDP();
    if (!sdp) {
        std::cerr << "[WebSocket] Error getting answer SDP\n";
        return;
    }
    // No need for answerCreated check; handled in Go
    json answerMsg;
    answerMsg["type"] = "answer";
    answerMsg["sdp"] = std::string(sdp);
    // Fetch SDP from Go
    send_message(answerMsg); // C++ handles WebSocket sending
    std::cout << "[WebSocket] Answer sent with SDP:" << answerMsg.dump() << "\n";
}

void handleOffer(const std::string& offer) {
    if (!createPeerConnection()) return;
    handleOffer(offer.c_str());
    sendAnswer(); // Trigger sending the answer
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
            std::cout << "[WebSocket] Received ice candidate from server" << message.dump() << std::endl;
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
    //std::this_thread::sleep_for(std::chrono::hours(24)); // Placeholder
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

    // Start frame sending in a separate thread
    std::thread frameThread(&sendFrames);
    frameThread.detach();
}

//int main() {
//    initWebsocket();
//    // Keep main running (e.g., wait for WebSocket events)
//    std::this_thread::sleep_for(std::chrono::hours(24)); // Placeholder
//    return 0;
//}