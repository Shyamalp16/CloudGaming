#include "Websocket.h"
#include "Encoder.h"
#include <chrono> // For high_resolution_clock
#include "KeyInputHandler.h"
#include "MouseInputHandler.h"
#include "ShutdownManager.h"
#include "WebRTCWrapper.h"
#include <optional>
#include <string_view>
#include "InputIntegrationLayer.h"
#include "InputConfig.h"

typedef websocketpp::client<websocketpp::config::asio_client>     plain_client;
typedef websocketpp::client<websocketpp::config::asio_tls_client> tls_client;
// Both config types share the same underlying message type (inherited from core).
using ws_message_ptr = plain_client::message_ptr;
using json = nlohmann::json;

// Plain (ws://) client — used for local dev
plain_client wsClient;
// TLS (wss://) client — used for Railway / production
tls_client   g_tlsClient;
// Set to true when the signaling URL starts with wss://
static bool  g_useTls = false;
static std::atomic<bool> g_websocketStopRequested{false};
static std::atomic<bool> g_signalingConnected{false};
static std::mutex g_connectionMutex;

websocketpp::connection_hdl g_connectionHandle;
std::string base_uri = "ws://localhost:3002/";

static std::shared_ptr<boost::asio::ssl::context>
on_tls_init(websocketpp::connection_hdl hdl) {
    auto ctx = std::make_shared<boost::asio::ssl::context>(
        boost::asio::ssl::context::tls_client);
    ctx->set_default_verify_paths();
    ctx->set_verify_mode(boost::asio::ssl::verify_peer);
    auto connection = g_tlsClient.get_con_from_hdl(hdl);
    ctx->set_verify_callback(boost::asio::ssl::host_name_verification(
        connection->get_uri()->get_host()));
    return ctx;
}
std::thread g_websocket_thread;
// Input poller threads to bridge Go data channels -> C++ input handlers
static std::atomic<bool> g_input_poll_running{ false };

static inline bool isVerboseWebsocketLoggingEnabled() {
    return InputConfig::globalInputConfig.enablePerEventLogging;
}

namespace JsonFastPath {
static inline size_t skipWs(std::string_view s, size_t i) {
    while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) ++i;
    return i;
}

static inline std::optional<std::string_view> getStringField(std::string_view payload, std::string_view field) {
    std::string token = "\"";
    token.append(field);
    token.push_back('"');
    size_t keyPos = payload.find(token);
    if (keyPos == std::string_view::npos) return std::nullopt;

    size_t colon = payload.find(':', keyPos + token.size());
    if (colon == std::string_view::npos) return std::nullopt;
    size_t pos = skipWs(payload, colon + 1);
    if (pos >= payload.size() || payload[pos] != '"') return std::nullopt;

    size_t start = pos + 1;
    size_t end = start;
    while (end < payload.size()) {
        if (payload[end] == '"' && payload[end - 1] != '\\') {
            break;
        }
        ++end;
    }
    if (end >= payload.size()) return std::nullopt;
    return payload.substr(start, end - start);
}

} // namespace JsonFastPath

void on_open(websocketpp::connection_hdl hdl);
void on_fail(websocketpp::connection_hdl hdl);
void on_close(websocketpp::connection_hdl hdl);
void on_message(websocketpp::connection_hdl hdl, ws_message_ptr msg);
void send_message(const json& message);

static void startInputPollers() {
    bool expected = false;
    if (!g_input_poll_running.compare_exchange_strong(expected, true)) return;

    if (InputIntegrationLayer::isRunning()) {
        std::cout << "[WebSocket] Input integration layer already running" << std::endl;
        return;
    }
    if (!InputIntegrationLayer::initialize() || !InputIntegrationLayer::start()) {
        std::cerr << "[WebSocket] Failed to start input integration layer" << std::endl;
        g_input_poll_running.store(false);
        return;
    }
    std::cout << "[WebSocket] Input integration layer started successfully" << std::endl;
}

static void stopInputPollers() {
    g_input_poll_running.store(false);

    // Stop the new input integration layer if it was started
    if (InputIntegrationLayer::isRunning()) {
        InputIntegrationLayer::stop();
        std::cout << "[WebSocket] Input integration layer stopped" << std::endl;
    }

    stopKeyInputHandler();
    stopMouseInputHandler();

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
    std::cout << "[WebSocket] Answer sent" << std::endl;
    freeCString(sdp); // Free C string allocated by Go via exported helper
}

void handleOffer(const std::string& offer) {
    if (!createPeerConnection()) return;
    handleOffer(offer.c_str());
    sendAnswer(); // Trigger sending the answer
    initKeyInputHandler();
    initMouseInputHandler();
    startInputPollers();
}

void handleRemoteIceCandidate(const json& candidateJson) {
    std::string candidateStr = candidateJson.value("candidate", "");
    handleRemoteIceCandidate(candidateStr.c_str());
}

void on_open(websocketpp::connection_hdl hdl) {
    std::cout << "[WebSocket] Connected opened" << std::endl;
    {
        std::lock_guard<std::mutex> lock(g_connectionMutex);
        g_connectionHandle = hdl;
    }
    g_signalingConnected.store(true, std::memory_order_release);
}

void on_fail(websocketpp::connection_hdl hdl) {
    std::string errMsg;
    try {
        if (g_useTls)
            errMsg = g_tlsClient.get_con_from_hdl(hdl)->get_ec().message();
        else
            errMsg = wsClient.get_con_from_hdl(hdl)->get_ec().message();
    } catch (...) { errMsg = "unknown"; }
    g_signalingConnected.store(false, std::memory_order_release);
    try { closePeerConnection(); } catch (...) {}
    std::cerr << "[WebSocket] Connection failed: " << errMsg << std::endl;
}

void on_close(websocketpp::connection_hdl hdl) {
    g_signalingConnected.store(false, std::memory_order_release);
    try { closePeerConnection(); } catch (...) {}
    std::cout << "[WebSocket] Connection closed" << std::endl;
    // Do not propagate Shutdown here; allow manual Stop/Close order only
}

void on_message(websocketpp::connection_hdl hdl, ws_message_ptr msg) {
    try {
        const std::string& payload = msg->get_payload();
        // Hard cap to prevent abuse, but allow large SDP (offers/answers)
        const size_t kMaxWsPayload = 1024 * 1024; // 1 MB
        if (payload.size() > kMaxWsPayload) {
            std::cerr << "[WebSocket] Dropping extremely large message (" << payload.size() << ")" << std::endl;
            return;
        }
        const auto typeOpt = JsonFastPath::getStringField(payload, "type");
        if (!typeOpt) {
            std::cerr << "[WebSocket] Received message without a valid 'type' field. payload_size="
                      << payload.size() << std::endl;
            return; // Skip processing this message
        }

        const std::string_view type = *typeOpt;
        if (isVerboseWebsocketLoggingEnabled()) {
            std::cout << "[Host] Received message type: " << type << std::endl;
        }

        // Lazily parse full JSON only for message types that require proper unescaping/nested fields.
        std::optional<json> parsedMessage;
        auto getParsedMessage = [&]() -> json* {
            if (!parsedMessage.has_value()) {
                json m = json::parse(payload, nullptr, false);
                if (m.is_discarded()) {
                    return nullptr;
                }
                parsedMessage = std::move(m);
            }
            return &parsedMessage.value();
        };

        if (type == "peer-disconnected") {
            std::cout << "[WebSocket] Peer has disconnected. Keeping host alive and closing PeerConnection only." << std::endl;
            try { closePeerConnection(); } catch (...) {}
        }
        else if (type == "offer") {
            if (isVerboseWebsocketLoggingEnabled()) {
                std::cout << "[WebSocket] Received offer from server" << std::endl;
            }
            json* message = getParsedMessage();
            if (!message) return;
            std::string sdp = message->value("sdp", "");
            if (sdp.empty()) return;
            handleOffer(sdp);
        }
        else if (type == "candidate") {
            // New schema: candidate is a top-level string, optional mid/index
            if (isVerboseWebsocketLoggingEnabled()) {
                std::cout << "[WebSocket] Received candidate" << std::endl;
            }
            json* message = getParsedMessage();
            if (!message || !message->contains("candidate") || !(*message)["candidate"].is_string()) {
                std::cerr << "[WebSocket] Invalid candidate payload from server" << std::endl;
                return;
            }
            json candidateJson;
            candidateJson["candidate"] = (*message)["candidate"].get<std::string>();
            handleRemoteIceCandidate(candidateJson);
        }
        else if (type == "control") {
            json* message = getParsedMessage();
            std::string action = message ? message->value("action", std::string()) : std::string();
            if (action == "schema-error") {
                std::cerr << "[WebSocket] Server reported schema-error for a message sent by host." << std::endl;
            } else {
                if (isVerboseWebsocketLoggingEnabled()) {
                    std::cout << "[WebSocket] Control message received" << std::endl;
                }
            }
        }
        else {
            if (isVerboseWebsocketLoggingEnabled()) {
                std::cout << "[WebSocket] Received unsupported message type: " << type << std::endl;
            }
        }
    }
    catch (const std::exception& e) {
        std::cerr << "[WebSocket] Error parsing message: " << e.what() << std::endl;
    }
}

void send_message(const json& message) {
    try {
        if (!g_signalingConnected.load(std::memory_order_acquire)) return;
        websocketpp::connection_hdl handle;
        {
            std::lock_guard<std::mutex> lock(g_connectionMutex);
            handle = g_connectionHandle;
        }
        if (!handle.lock()) return;
        std::string payload = message.dump();
        if (g_useTls)
            g_tlsClient.send(handle, payload, websocketpp::frame::opcode::text);
        else
            wsClient.send(handle, payload, websocketpp::frame::opcode::text);
        if (isVerboseWebsocketLoggingEnabled()) {
            std::cout << "[WebSocket] Sent message of type: " << message.value("type", "unknown") << std::endl;
        }
    }
    catch (const std::exception& e) {
        std::cerr << "[WebSocket] Error sending message: " << e.what() << std::endl;
    }
}

void initWebsocket(const std::string& roomId, const std::string& signalingUrl) {
    if (!signalingUrl.empty()) {
        base_uri = signalingUrl;
        if (base_uri.back() != '/') base_uri += '/';
    }

    // Detect scheme: wss:// → TLS client, ws:// → plain client
    g_useTls = (base_uri.rfind("wss://", 0) == 0);

    std::string full_uri = base_uri + "?roomId=" + roomId;
    std::cout << "[WebSocket] Connecting to " << full_uri
              << (g_useTls ? " (TLS)" : " (plain)") << std::endl;

    g_websocketStopRequested.store(false, std::memory_order_release);

    if (g_useTls) {
        g_tlsClient.init_asio();
        g_tlsClient.set_open_handler(&on_open);
        g_tlsClient.set_message_handler(&on_message);
        g_tlsClient.set_fail_handler(&on_fail);
        g_tlsClient.set_close_handler(&on_close);
        g_tlsClient.set_tls_init_handler(&on_tls_init);

        g_websocket_thread = std::thread([full_uri]() {
            while (!g_websocketStopRequested.load(std::memory_order_acquire) &&
                   !ShutdownManager::IsShutdown()) {
                websocketpp::lib::error_code connectError;
                auto connection = g_tlsClient.get_connection(full_uri, connectError);
                if (!connectError) {
                    g_tlsClient.connect(connection);
                    try { g_tlsClient.run(); }
                    catch (const std::exception& ex) {
                        std::cerr << "[WebSocket] TLS run() threw: " << ex.what() << std::endl;
                    }
                } else {
                    std::cerr << "[WebSocket] TLS connection setup failed: " << connectError.message() << std::endl;
                }
                if (g_websocketStopRequested.load() || ShutdownManager::IsShutdown()) break;
                g_tlsClient.reset();
                for (int i = 0; i < 20 && !g_websocketStopRequested.load() && !ShutdownManager::IsShutdown(); ++i)
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
        });
    } else {
        wsClient.init_asio();
        wsClient.set_open_handler(&on_open);
        wsClient.set_message_handler(&on_message);
        wsClient.set_fail_handler(&on_fail);
        wsClient.set_close_handler(&on_close);

        g_websocket_thread = std::thread([full_uri]() {
            while (!g_websocketStopRequested.load(std::memory_order_acquire) &&
                   !ShutdownManager::IsShutdown()) {
                websocketpp::lib::error_code connectError;
                auto connection = wsClient.get_connection(full_uri, connectError);
                if (!connectError) {
                    wsClient.connect(connection);
                    try { wsClient.run(); }
                    catch (const std::exception& ex) {
                        std::cerr << "[WebSocket] run() threw: " << ex.what() << std::endl;
                    }
                } else {
                    std::cerr << "[WebSocket] Connection setup failed: " << connectError.message() << std::endl;
                }
                if (g_websocketStopRequested.load() || ShutdownManager::IsShutdown()) break;
                wsClient.reset();
                for (int i = 0; i < 20 && !g_websocketStopRequested.load() && !ShutdownManager::IsShutdown(); ++i)
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
        });
    }
}

void stopWebsocket() {
    stopInputPollers();
    static std::atomic<bool> stopped{ false };
    bool expected = false;
    if (!stopped.compare_exchange_strong(expected, true)) {
        std::wcout << L"[Shutdown] stopWebsocket already executed. Skipping.\n";
        return;
    }

    std::wcout << L"[Shutdown] Initiating websocket shutdown...\n";
    g_websocketStopRequested.store(true, std::memory_order_release);
    g_signalingConnected.store(false, std::memory_order_release);
    // Signal encoder loops to stop producing frames

    // Close the PeerConnection first to stop RTP/data traffic gracefully
    try {
        closePeerConnection();
    } catch (...) {
        std::wcout << L"[Shutdown] Exception during closePeerConnection (ignored).\n";
    }

    std::wcout << L"[Shutdown] Stopping websocket client...\n";
    try {
        if (g_useTls) g_tlsClient.stop();
        else          wsClient.stop();
    } catch (...) {
        std::wcout << L"[Shutdown] Exception during wsClient.stop() (ignored).\n";
    }

    std::wcout << L"[Shutdown] Joining websocket thread...\n";
    if (g_websocket_thread.joinable()) {
        g_websocket_thread.join();
    }
    std::wcout << L"[Shutdown] Websocket thread joined.\n";

    // Legacy frame/sender threads removed

    std::wcout << L"[Shutdown] Websocket shutdown complete.\n";
}

// Callback from Go to send ICE candidates
extern "C" void onIceCandidate(const char* candidate) {
    json iceMsg;
    // Send using server's schema: top-level string candidate
    iceMsg["type"] = "candidate";
    iceMsg["candidate"] = std::string(candidate);
    send_message(iceMsg);
}
