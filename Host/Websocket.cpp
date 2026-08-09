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
#include "SessionManager.h"
#include "StreamProfileManager.h"
#include <condition_variable>
#include <random>
#include <unordered_set>

typedef websocketpp::client<websocketpp::config::asio_client>     plain_client;
typedef websocketpp::client<websocketpp::config::asio_tls_client> tls_client;
// Both config types share the same underlying message type (inherited from core).
using ws_message_ptr = plain_client::message_ptr;
using json = nlohmann::json;

// Plain (ws://) client — used for local dev
static std::unique_ptr<plain_client> wsClient;
// TLS (wss://) client — used for Railway / production
static std::unique_ptr<tls_client> g_tlsClient;
// Set to true when the signaling URL starts with wss://
static bool  g_useTls = false;
static std::atomic<bool> g_websocketStopRequested{false};
static std::atomic<bool> g_signalingConnected{false};
static std::atomic<bool> g_connectionOpened{false};
static std::mutex g_connectionMutex;
static std::mutex g_reconnectMutex;
static std::condition_variable g_reconnectCondition;
static SessionManager* g_sessionManager = nullptr;
static StreamProfileManager* g_streamProfileManager = nullptr;
static std::string g_hostSecret;

websocketpp::connection_hdl g_connectionHandle;
std::string base_uri = "ws://localhost:3002/";

static std::shared_ptr<boost::asio::ssl::context>
on_tls_init(websocketpp::connection_hdl hdl) {
    auto ctx = std::make_shared<boost::asio::ssl::context>(
        boost::asio::ssl::context::tls_client);
    ctx->set_default_verify_paths();
    ctx->set_verify_mode(boost::asio::ssl::verify_peer);
	ctx->set_options(boost::asio::ssl::context::no_sslv2 |
		boost::asio::ssl::context::no_sslv3 |
		boost::asio::ssl::context::no_tlsv1 |
		boost::asio::ssl::context::no_tlsv1_1 |
		boost::asio::ssl::context::no_compression);
	if (SSL_CTX_set_min_proto_version(ctx->native_handle(), TLS1_2_VERSION) != 1)
		throw std::runtime_error("Could not enforce TLS 1.2 or newer");
    auto connection = g_tlsClient->get_con_from_hdl(hdl);
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

namespace SignalingSchema {
static bool HasOnly(const json& value, const std::unordered_set<std::string>& fields) {
    if (!value.is_object() || value.size() > fields.size()) return false;
    for (auto it = value.begin(); it != value.end(); ++it) if (!fields.count(it.key())) return false;
    return true;
}

static bool IsSessionId(const json& value) {
    if (!value.is_string()) return false;
    const auto& id = value.get_ref<const std::string&>();
    if (id.size() != 36 || id[8] != '-' || id[13] != '-' || id[18] != '-' || id[23] != '-') return false;
    for (size_t i = 0; i < id.size(); ++i) {
        if (i == 8 || i == 13 || i == 18 || i == 23) continue;
        if (!std::isxdigit(static_cast<unsigned char>(id[i]))) return false;
    }
    return true;
}

static bool IsIntInRange(const json& value, int minimum, int maximum) {
    return value.is_number_integer() && value.get<long long>() >= minimum && value.get<long long>() <= maximum;
}

static bool Validate(const json& message, std::string& error) {
    if (!message.is_object() || !message.contains("type") || !message["type"].is_string()) {
        error = "message must be an object with a string type"; return false;
    }
    const auto type = message["type"].get<std::string>();
    if (type == "offer") {
        if (!HasOnly(message, {"type", "sessionId", "sdp"}) || !message.contains("sessionId") ||
            !IsSessionId(message["sessionId"]) || !message.contains("sdp") || !message["sdp"].is_string() ||
            message["sdp"].get_ref<const std::string&>().empty() ||
            message["sdp"].get_ref<const std::string&>().size() > 256 * 1024) {
            error = "invalid offer"; return false;
        }
    } else if (type == "candidate") {
        if (!HasOnly(message, {"type", "sessionId", "candidate", "sdpMid", "sdpMLineIndex"}) ||
            !message.contains("sessionId") || !IsSessionId(message["sessionId"]) ||
            !message.contains("candidate") || !message["candidate"].is_string() ||
            message["candidate"].get_ref<const std::string&>().empty() ||
            message["candidate"].get_ref<const std::string&>().size() > 4096 ||
            (message.contains("sdpMid") && (!message["sdpMid"].is_string() ||
             message["sdpMid"].get_ref<const std::string&>().size() > 64)) ||
            (message.contains("sdpMLineIndex") && !IsIntInRange(message["sdpMLineIndex"], 0, 64))) {
            error = "invalid candidate"; return false;
        }
    } else if (type == "stream-profile") {
        if (!HasOnly(message, {"type", "sessionId", "width", "height", "fps", "bitrate", "capabilities"}) ||
            !message.contains("sessionId") || !IsSessionId(message["sessionId"]) ||
            !message.contains("width") || !IsIntInRange(message["width"], 640, 3840) ||
            !message.contains("height") || !IsIntInRange(message["height"], 360, 2160) ||
            !message.contains("fps") || !IsIntInRange(message["fps"], 30, 120) ||
            !message.contains("bitrate") || !IsIntInRange(message["bitrate"], 500000, 50000000) ||
            !message.contains("capabilities") || !message["capabilities"].is_object()) {
            error = "invalid stream profile"; return false;
        }
        const auto& caps = message["capabilities"];
        if (!HasOnly(caps, {"maxWidth", "maxHeight", "maxFps", "maxBitrate", "h264"}) ||
            !caps.contains("maxWidth") || !IsIntInRange(caps["maxWidth"], 640, 7680) ||
            !caps.contains("maxHeight") || !IsIntInRange(caps["maxHeight"], 360, 4320) ||
            !caps.contains("maxFps") || !IsIntInRange(caps["maxFps"], 30, 240) ||
            !caps.contains("maxBitrate") || !IsIntInRange(caps["maxBitrate"], 500000, 100000000) ||
            !caps.contains("h264") || !caps["h264"].is_boolean()) {
            error = "invalid stream capabilities"; return false;
        }
    } else if (type == "control") {
        if (!HasOnly(message, {"type", "sessionId", "action", "payload"}) ||
            !message.contains("action") || !message["action"].is_string() ||
            message["action"].get_ref<const std::string&>().size() > 64 ||
            (message.contains("sessionId") && !IsSessionId(message["sessionId"])) ||
            (message.contains("payload") && (!message["payload"].is_object() || message["payload"].size() > 16))) {
            error = "invalid control message"; return false;
        }
        static const std::unordered_set<std::string> actions = {
            "terminate", "replace", "profile-request", "profile-accepted", "profile-rejected",
            "schema-error", "session-ready", "ping"};
        if (!actions.count(message["action"].get<std::string>())) {
            error = "unknown control action"; return false;
        }
    } else if (type == "peer-disconnected") {
        if (!HasOnly(message, {"type", "sessionId"}) ||
            (message.contains("sessionId") && !IsSessionId(message["sessionId"]))) {
            error = "invalid disconnect notification"; return false;
        }
    } else {
        error = "unsupported signaling type"; return false;
    }
    return true;
}
} // namespace SignalingSchema

static bool ConsumeSignalingBudget() {
    static std::mutex mutex;
    static auto last = std::chrono::steady_clock::now();
    static double tokens = 120.0;
    std::lock_guard<std::mutex> lock(mutex);
    const auto now = std::chrono::steady_clock::now();
    tokens = (std::min)(120.0, tokens + std::chrono::duration<double>(now - last).count() * 60.0);
    last = now;
    if (tokens < 1.0) return false;
    tokens -= 1.0;
    return true;
}

void on_open(websocketpp::connection_hdl hdl);
void on_fail(websocketpp::connection_hdl hdl);
void on_close(websocketpp::connection_hdl hdl);
void on_message(websocketpp::connection_hdl hdl, ws_message_ptr msg);
void send_message(const json& message);

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

void handleOffer(const std::string& offer, const std::string& sessionId) {
    if (!g_sessionManager || sessionId.empty()) return;
    const auto previous = g_sessionManager->GetStatus();
    if (previous.sessionId != sessionId) {
        const int decision = MessageBoxW(nullptr,
            L"A remote player is requesting access to this computer.\n\n"
            L"If approved, they can see and hear the selected game and control its keyboard and mouse.\n\n"
            L"Approve this session?",
            L"Cloud Gaming Host - Remote access request",
            MB_YESNO | MB_ICONWARNING | MB_TOPMOST | MB_SETFOREGROUND | MB_DEFBUTTON2);
        if (decision != IDYES) {
            send_message({{"type", "control"}, {"sessionId", sessionId}, {"action", "terminate"},
                          {"payload", {{"reason", "host_denied"}}}});
            return;
        }
    }
    if (!previous.sessionId.empty() && previous.sessionId != sessionId) {
        if (g_streamProfileManager) g_streamProfileManager->ClearSession(previous.sessionId);
        InputIntegrationLayer::clearAuthorizedSession("session_replaced");
        try { closePeerConnection(); } catch (...) {}
    }
    if (!g_sessionManager->Authorize(sessionId)) return;
    InputIntegrationLayer::authorizeSession(sessionId);
    if (!createPeerConnection()) return;
    handleOffer(offer.c_str());
    sendAnswer(); // Trigger sending the answer
    g_sessionManager->MarkConnected(sessionId);
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
    g_connectionOpened.store(true, std::memory_order_release);
}

void on_fail(websocketpp::connection_hdl hdl) {
    std::string errMsg;
    try {
        if (g_useTls)
            errMsg = g_tlsClient->get_con_from_hdl(hdl)->get_ec().message();
        else
            errMsg = wsClient->get_con_from_hdl(hdl)->get_ec().message();
    } catch (...) { errMsg = "unknown"; }
    g_signalingConnected.store(false, std::memory_order_release);
    if (g_sessionManager) g_sessionManager->MarkReconnecting();
    InputIntegrationLayer::resetAllInput("signaling_connection_failed");
    try { closePeerConnection(); } catch (...) {}
    std::cerr << "[WebSocket] Connection failed: " << errMsg << std::endl;
}

void on_close(websocketpp::connection_hdl hdl) {
    g_signalingConnected.store(false, std::memory_order_release);
    if (g_sessionManager) g_sessionManager->MarkReconnecting();
    InputIntegrationLayer::resetAllInput("signaling_connection_closed");
    try { closePeerConnection(); } catch (...) {}
    std::cout << "[WebSocket] Connection closed" << std::endl;
    // Do not propagate Shutdown here; allow manual Stop/Close order only
}

void on_message(websocketpp::connection_hdl hdl, ws_message_ptr msg) {
    try {
        const std::string& payload = msg->get_payload();
        const size_t kMaxWsPayload = 300 * 1024;
        if (payload.size() > kMaxWsPayload) {
            std::cerr << "[WebSocket] Dropping oversized signaling message" << std::endl;
            return;
        }
        if (!ConsumeSignalingBudget()) {
            std::cerr << "[WebSocket] Signaling rate limit exceeded" << std::endl;
            return;
        }
        json parsedMessage = json::parse(payload, nullptr, false);
        std::string schemaError;
        if (parsedMessage.is_discarded() || !SignalingSchema::Validate(parsedMessage, schemaError)) {
            std::cerr << "[WebSocket] Rejected signaling message: " << schemaError << std::endl;
            return;
        }
        const std::string type = parsedMessage["type"].get<std::string>();
        if (isVerboseWebsocketLoggingEnabled()) {
            std::cout << "[Host] Received message type: " << type << std::endl;
        }

        auto getParsedMessage = [&]() -> json* { return &parsedMessage; };

        if (type == "peer-disconnected") {
            std::cout << "[WebSocket] Peer has disconnected. Keeping host alive and closing PeerConnection only." << std::endl;
            const auto session = g_sessionManager ? g_sessionManager->GetStatus() : SessionManager::Status{};
            try { closePeerConnection(); } catch (...) {}
            InputIntegrationLayer::clearAuthorizedSession("peer_disconnected");
            if (g_streamProfileManager && !session.sessionId.empty()) {
                g_streamProfileManager->ClearSession(session.sessionId);
            }
            if (g_sessionManager) g_sessionManager->Terminate("peer_disconnected");
        }
        else if (type == "offer") {
            if (isVerboseWebsocketLoggingEnabled()) {
                std::cout << "[WebSocket] Received offer from server" << std::endl;
            }
            json* message = getParsedMessage();
            if (!message) return;
            std::string sdp = message->value("sdp", "");
            std::string sessionId = message->value("sessionId", "");
            if (sdp.empty() || sessionId.empty()) return;
            handleOffer(sdp, sessionId);
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
            const std::string sessionId = message->value("sessionId", std::string{});
            if (!g_sessionManager || !g_sessionManager->Accepts(sessionId)) return;
            json candidateJson;
            candidateJson["candidate"] = (*message)["candidate"].get<std::string>();
            handleRemoteIceCandidate(candidateJson);
        }
        else if (type == "stream-profile") {
            json* message = getParsedMessage();
            if (!message || !g_sessionManager || !g_streamProfileManager) return;
            const std::string sessionId = message->value("sessionId", std::string{});
            if (!g_sessionManager->Accepts(sessionId) || !message->contains("capabilities") ||
                !(*message)["capabilities"].is_object()) return;
            const auto& caps = (*message)["capabilities"];
            StreamProfileManager::Profile profile{
                message->value("width", 0), message->value("height", 0),
                message->value("fps", 0), message->value("bitrate", 0)};
            StreamProfileManager::ClientCapabilities capabilities{
                caps.value("maxWidth", 0), caps.value("maxHeight", 0),
                caps.value("maxFps", 0), caps.value("maxBitrate", 0), caps.value("h264", false)};
            std::string error;
            const bool accepted = g_streamProfileManager->Request(sessionId, profile, capabilities, error);
            json response{{"type", "control"},
                          {"action", accepted ? "profile-accepted" : "profile-rejected"}};
            response["payload"] = accepted
                ? json{{"width", profile.width}, {"height", profile.height},
                       {"fps", profile.fps}, {"bitrate", profile.bitrate}}
                : json{{"reason", error}};
            send_message(response);
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
        json outbound = message;
        if (!outbound.contains("sessionId") && g_sessionManager) {
            const auto session = g_sessionManager->GetStatus();
            if (!session.sessionId.empty()) outbound["sessionId"] = session.sessionId;
        }
        std::string payload = outbound.dump();
        if (g_useTls)
            g_tlsClient->send(handle, payload, websocketpp::frame::opcode::text);
        else
            wsClient->send(handle, payload, websocketpp::frame::opcode::text);
        if (isVerboseWebsocketLoggingEnabled()) {
            std::cout << "[WebSocket] Sent message of type: " << message.value("type", "unknown") << std::endl;
        }
    }
    catch (const std::exception& e) {
        std::cerr << "[WebSocket] Error sending message: " << e.what() << std::endl;
    }
}

int WebsocketPolicy::ComputeReconnectDelayMs(unsigned attempt, double jitterFraction) {
    const unsigned shift = (std::min)(attempt, 6u);
    const int capMs = (std::min)(30000, 500 * (1 << shift));
    const double boundedJitter = std::clamp(jitterFraction, 0.0, 0.25);
    return capMs + static_cast<int>(capMs * boundedJitter);
}

static bool waitForReconnect(unsigned attempt) {
    const int capMs = WebsocketPolicy::ComputeReconnectDelayMs(attempt, 0.0);
    thread_local std::mt19937 generator(std::random_device{}());
    std::uniform_int_distribution<int> jitter(0, capMs / 4);
    std::unique_lock<std::mutex> lock(g_reconnectMutex);
    return !g_reconnectCondition.wait_for(lock, std::chrono::milliseconds(
        WebsocketPolicy::ComputeReconnectDelayMs(attempt, static_cast<double>(jitter(generator)) / capMs)), [] {
        return g_websocketStopRequested.load() || ShutdownManager::IsShutdown();
    });
}

void initWebsocket(const std::string& roomId, const std::string& hostId, const std::string& signalingUrl,
                   const std::string& hostSecret, const std::string& networkMode,
                   SessionManager* sessionManager, StreamProfileManager* streamProfileManager) {
    (void)networkMode;
    g_sessionManager = sessionManager;
    g_streamProfileManager = streamProfileManager;
    g_hostSecret = hostSecret;
    if (!signalingUrl.empty()) {
        base_uri = signalingUrl;
        if (base_uri.back() != '/') base_uri += '/';
    }

    // Detect scheme: wss:// → TLS client, ws:// → plain client
    g_useTls = (base_uri.rfind("wss://", 0) == 0);

    const std::string full_uri = base_uri;
    std::cout << "[WebSocket] Connecting to signaling service"
              << (g_useTls ? " (TLS)" : " (plain)") << std::endl;

    g_websocketStopRequested.store(false, std::memory_order_release);

    if (g_useTls) {
        g_tlsClient = std::make_unique<tls_client>();
		g_tlsClient->clear_access_channels(websocketpp::log::alevel::all);
		g_tlsClient->set_access_channels(websocketpp::log::alevel::connect |
			websocketpp::log::alevel::disconnect | websocketpp::log::alevel::fail);
        g_tlsClient->init_asio();
        g_tlsClient->set_open_handler(&on_open);
        g_tlsClient->set_message_handler(&on_message);
        g_tlsClient->set_fail_handler(&on_fail);
        g_tlsClient->set_close_handler(&on_close);
        g_tlsClient->set_tls_init_handler(&on_tls_init);

        g_websocket_thread = std::thread([full_uri, roomId, hostId]() {
            unsigned reconnectAttempt = 0;
            while (!g_websocketStopRequested.load(std::memory_order_acquire) &&
                   !ShutdownManager::IsShutdown()) {
                websocketpp::lib::error_code connectError;
                auto connection = g_tlsClient->get_connection(full_uri, connectError);
                if (!connectError) {
                    if (!g_hostSecret.empty()) connection->append_header("Authorization", "Bearer " + g_hostSecret);
                    connection->add_subprotocol("cloud-gaming-v1");
                    connection->add_subprotocol("cg-room." + roomId);
                    connection->add_subprotocol("cg-role.host");
                    connection->add_subprotocol("cg-host." + hostId);
                    g_tlsClient->connect(connection);
                    try { g_tlsClient->run(); }
                    catch (const std::exception& ex) {
                        std::cerr << "[WebSocket] TLS run() threw: " << ex.what() << std::endl;
                    }
                } else {
                    std::cerr << "[WebSocket] TLS connection setup failed: " << connectError.message() << std::endl;
                }
                if (g_websocketStopRequested.load() || ShutdownManager::IsShutdown()) break;
                g_tlsClient->reset();
                if (g_connectionOpened.exchange(false)) reconnectAttempt = 0;
                if (!waitForReconnect(reconnectAttempt++)) break;
            }
        });
    } else {
        wsClient = std::make_unique<plain_client>();
		wsClient->clear_access_channels(websocketpp::log::alevel::all);
		wsClient->set_access_channels(websocketpp::log::alevel::connect |
			websocketpp::log::alevel::disconnect | websocketpp::log::alevel::fail);
        wsClient->init_asio();
        wsClient->set_open_handler(&on_open);
        wsClient->set_message_handler(&on_message);
        wsClient->set_fail_handler(&on_fail);
        wsClient->set_close_handler(&on_close);

        g_websocket_thread = std::thread([full_uri, roomId, hostId]() {
            unsigned reconnectAttempt = 0;
            while (!g_websocketStopRequested.load(std::memory_order_acquire) &&
                   !ShutdownManager::IsShutdown()) {
                websocketpp::lib::error_code connectError;
                auto connection = wsClient->get_connection(full_uri, connectError);
                if (!connectError) {
                    if (!g_hostSecret.empty()) connection->append_header("Authorization", "Bearer " + g_hostSecret);
                    connection->add_subprotocol("cloud-gaming-v1");
                    connection->add_subprotocol("cg-room." + roomId);
                    connection->add_subprotocol("cg-role.host");
                    connection->add_subprotocol("cg-host." + hostId);
                    wsClient->connect(connection);
                    try { wsClient->run(); }
                    catch (const std::exception& ex) {
                        std::cerr << "[WebSocket] run() threw: " << ex.what() << std::endl;
                    }
                } else {
                    std::cerr << "[WebSocket] Connection setup failed: " << connectError.message() << std::endl;
                }
                if (g_websocketStopRequested.load() || ShutdownManager::IsShutdown()) break;
                wsClient->reset();
                if (g_connectionOpened.exchange(false)) reconnectAttempt = 0;
                if (!waitForReconnect(reconnectAttempt++)) break;
            }
        });
    }
}

void stopWebsocket() {
    std::wcout << L"[Shutdown] Initiating websocket shutdown...\n";
    g_websocketStopRequested.store(true, std::memory_order_release);
    g_reconnectCondition.notify_all();
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
        if (g_useTls && g_tlsClient) g_tlsClient->stop();
        else if (wsClient)           wsClient->stop();
    } catch (...) {
        std::wcout << L"[Shutdown] Exception during wsClient.stop() (ignored).\n";
    }

    std::wcout << L"[Shutdown] Joining websocket thread...\n";
    if (g_websocket_thread.joinable()) {
        g_websocket_thread.join();
    }
    g_tlsClient.reset();
    wsClient.reset();
	if (!g_hostSecret.empty()) SecureZeroMemory(g_hostSecret.data(), g_hostSecret.size());
	g_hostSecret.clear();
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
