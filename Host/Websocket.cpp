#include "Websocket.h"
#include "Encoder.h"
#include <chrono> // For high_resolution_clock
#include "KeyInputHandler.h"
#include "MouseInputHandler.h"
#include "ShutdownManager.h"
#include "PacketQueue.h"
#include "WebRTCWrapper.h"
#include <unordered_set>
#include "Config.h"
#include "Metrics.h"
#include "VideoMetrics.h"
#include "InputIntegrationLayer.h"
#include "LegacyWebSocketCompat.h"
#include "InputConfig.h"

// Forward declarations for blocking queue functions
void enqueueKeyboardMessage(const std::string& message);
void enqueueMouseMessage(const std::string& message);

// Forward declarations for input handler enqueue functions
namespace KeyInputHandler {
    void enqueueMessage(const std::string& message);
}

namespace MouseInputHandler {
    void enqueueMessage(const std::string& message);
}

typedef websocketpp::client<websocketpp::config::asio_client> client;
using json = nlohmann::json;

client wsClient;
websocketpp::connection_hdl g_connectionHandle;
std::string base_uri = "ws://localhost:3002/";
std::thread g_websocket_thread;
// Legacy video sender threads removed; encoder pushes directly to Pion
std::thread g_metrics_thread;
static std::atomic<bool> g_metrics_export_enabled{ false };
// Input poller threads to bridge Go data channels -> C++ input handlers
static std::atomic<bool> g_input_poll_running{ false };
static std::thread g_kb_poller;
static std::thread g_mouse_poller;
static std::mutex g_poller_mutex;
static std::condition_variable g_poller_cv;

// Allowed key codes (must match client-side codes and KeyInputHandler mapping)
static const std::unordered_set<std::string> kValidKeyCodes = {
    // Letters
    "KeyA","KeyB","KeyC","KeyD","KeyE","KeyF","KeyG","KeyH","KeyI","KeyJ","KeyK","KeyL","KeyM","KeyN","KeyO","KeyP","KeyQ","KeyR","KeyS","KeyT","KeyU","KeyV","KeyW","KeyX","KeyY","KeyZ",
    // Numbers
    "Digit0","Digit1","Digit2","Digit3","Digit4","Digit5","Digit6","Digit7","Digit8","Digit9",
    // Numpad
    "Numpad0","Numpad1","Numpad2","Numpad3","Numpad4","Numpad5","Numpad6","Numpad7","Numpad8","Numpad9",
    "NumpadDecimal","NumpadAdd","NumpadSubtract","NumpadMultiply","NumpadDivide","NumpadEnter",
    // Function
    "F1","F2","F3","F4","F5","F6","F7","F8","F9","F10","F11","F12",
    // Arrows
    "ArrowUp","ArrowDown","ArrowLeft","ArrowRight",
    // Modifiers / system
    "ShiftLeft","ShiftRight","ControlLeft","ControlRight","AltLeft","AltRight","MetaLeft","MetaRight",
    // Others
    "Enter","Escape","Tab","Space","Backspace","Delete","Home","End","PageUp","PageDown","CapsLock","NumLock","ScrollLock","Insert","ContextMenu",
    // Punctuation
    "Backquote","Minus","Equal","BracketLeft","BracketRight","Backslash","Semicolon","Quote","Comma","Period","Slash"
};

// Simple token bucket for rate limiting
struct TokenBucket {
    double tokens{0};
    double capacity{0};
    double refillPerSec{0};
    std::chrono::steady_clock::time_point last;
    void init(double cap, double rate) { capacity = cap; refillPerSec = rate; tokens = cap; last = std::chrono::steady_clock::now(); }
    bool consume(double n) {
        auto now = std::chrono::steady_clock::now();
        double dt = std::chrono::duration<double>(now - last).count();
        tokens = std::min(capacity, tokens + dt * refillPerSec);
        last = now;
        if (tokens >= n) { tokens -= n; return true; }
        return false;
    }
};
static TokenBucket g_keyBucket; // e.g., 200 events/sec burst 200
static TokenBucket g_mouseBucket; // e.g., 500 events/sec burst 500
static std::once_flag g_bucketsInit;

void on_open(client* c, websocketpp::connection_hdl hdl);
void on_fail(client* c, websocketpp::connection_hdl hdl);
void on_close(client* c, websocketpp::connection_hdl hdl);
void on_message(websocketpp::connection_hdl hdl, client::message_ptr msg);
void send_message(const json& message);

static void metricsExportLoop() {
    auto last = std::chrono::steady_clock::now();
    while (!ShutdownManager::IsShutdown() && g_metrics_export_enabled.load()) {
        auto now = std::chrono::steady_clock::now();
        if (std::chrono::duration_cast<std::chrono::seconds>(now - last).count() >= 1) {
            last = now;
            try {
                json m;
                m["type"] = "video-metrics";
                m["queueDepth"] = VideoMetrics::load(VideoMetrics::queueDepth());
                m["overwriteDrops"] = VideoMetrics::load(VideoMetrics::overwriteDrops());
                m["backpressureSkips"] = VideoMetrics::load(VideoMetrics::backpressureSkips());
                m["outOfOrder"] = VideoMetrics::load(VideoMetrics::outOfOrder());
                m["vpGpuMs"] = VideoMetrics::vpGpuMs().load(std::memory_order_relaxed);
                send_message(m);
            } catch (...) {
                // ignore
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
}

void startMetricsExport(bool enable) {
    g_metrics_export_enabled.store(enable);
    if (enable) {
        if (!g_metrics_thread.joinable()) {
            g_metrics_thread = std::thread(metricsExportLoop);
        }
    }
}

static void startInputPollers() {
    bool expected = false;
    if (!g_input_poll_running.compare_exchange_strong(expected, true)) return;

    // Check if new architecture is enabled in configuration
    if (InputConfig::globalInputConfig.usePionDataChannels) {
        // Initialize and start the new input integration layer
        if (!InputIntegrationLayer::initialize()) {
            std::cerr << "[WebSocket] Failed to initialize input integration layer, falling back to legacy mode" << std::endl;
            g_input_poll_running.store(false);
            return;
        }

        if (!InputIntegrationLayer::start()) {
            std::cerr << "[WebSocket] Failed to start input integration layer" << std::endl;
            g_input_poll_running.store(false);
            return;
        }

        std::cout << "[WebSocket] New input architecture started successfully" << std::endl;

        // Check if legacy compatibility is also enabled
        if (InputConfig::globalInputConfig.enableLegacyWebSocket) {
            std::cout << "[WebSocket] Legacy WebSocket compatibility enabled - starting legacy polling threads" << std::endl;

            if (!LegacyWebSocketCompat::startLegacyPolling()) {
                std::cerr << "[WebSocket] Failed to start legacy polling threads" << std::endl;
                // Continue with new architecture even if legacy fails
            }
        } else {
            std::cout << "[WebSocket] Legacy WebSocket compatibility disabled - using new architecture only" << std::endl;
        }

    } else if (InputConfig::globalInputConfig.enableLegacyWebSocket) {
        // Only legacy WebSocket is enabled
        std::cout << "[WebSocket] Using legacy WebSocket polling only (new architecture disabled)" << std::endl;

        if (!LegacyWebSocketCompat::startLegacyPolling()) {
            std::cerr << "[WebSocket] Failed to start legacy polling threads" << std::endl;
            g_input_poll_running.store(false);
            return;
        }

    } else {
        std::cerr << "[WebSocket] Error: Neither new architecture nor legacy WebSocket is enabled in configuration" << std::endl;
        std::cerr << "[WebSocket] Please set usePionDataChannels=true or enableLegacyWebSocket=true in config.json" << std::endl;
        g_input_poll_running.store(false);
        return;
    }
}

static void stopInputPollers() {
    bool expected = true;
    if (!g_input_poll_running.compare_exchange_strong(expected, false)) return;

    // Stop the new input integration layer if it was started
    if (InputIntegrationLayer::isRunning()) {
        InputIntegrationLayer::stop();
        std::cout << "[WebSocket] Input integration layer stopped" << std::endl;
    }

    // Stop legacy polling threads if they were started
    if (LegacyWebSocketCompat::isLegacyPollingRunning()) {
        LegacyWebSocketCompat::stopLegacyPolling();
        std::cout << "[WebSocket] Legacy WebSocket polling stopped" << std::endl;
    }

    // Legacy compatibility: For any remaining threads, notify and join
    // (This handles edge cases where threads might still be running)
    {
        std::lock_guard<std::mutex> lock(g_poller_mutex);
        g_poller_cv.notify_all();
    }

    // Wait for any remaining legacy threads to complete
    if (g_kb_poller.joinable()) {
        std::cout << "[WebSocket] Waiting for legacy keyboard poller to join..." << std::endl;
        g_kb_poller.join();
        std::cout << "[WebSocket] Legacy keyboard poller joined successfully" << std::endl;
    }

    if (g_mouse_poller.joinable()) {
        std::cout << "[WebSocket] Waiting for legacy mouse poller to join..." << std::endl;
        g_mouse_poller.join();
        std::cout << "[WebSocket] Legacy mouse poller joined successfully" << std::endl;
    }
}

void stopMetricsExport() {
    g_metrics_export_enabled.store(false);
    if (g_metrics_thread.joinable()) {
        g_metrics_thread.join();
    }
}

// Legacy sender path removed; encoder pushes directly via Go API

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
        // Initialize rate-limiters once
        std::call_once(g_bucketsInit, [](){
            InputConfig::initialize();
            g_keyBucket.init(InputConfig::getKeyEventsPerSecond(), InputConfig::getKeyEventsPerSecond());
            g_mouseBucket.init(InputConfig::getMouseEventsPerSecond(), InputConfig::getMouseEventsPerSecond());
        });

        const std::string& payload = msg->get_payload();
        // Hard cap to prevent abuse, but allow large SDP (offers/answers)
        const size_t kMaxWsPayload = 1024 * 1024; // 1 MB
        if (payload.size() > kMaxWsPayload) {
            std::cerr << "[WebSocket] Dropping extremely large message (" << payload.size() << ")" << std::endl;
            return;
        }
        json message = json::parse(payload);

        if (!message.contains("type") || !message["type"].is_string()) {
            std::cerr << "[WebSocket] Received message without a valid 'type' field: " << message.dump() << std::endl;
            return; // Skip processing this message
        }

        std::string type = message["type"];
        std::cout << "[Host] Received message type: " << type << std::endl;

        // Apply tight payload limits only to input/control events, not SDP signaling
        if (type == "keydown" || type == "keyup" || type == "mousemove" || type == "mousedown" || type == "mouseup") {
            if (type == "keydown" || type == "keyup") { InputMetrics::inc(InputMetrics::receivedKeyboard()); }
            else { InputMetrics::inc(InputMetrics::receivedMouse()); }
            if (payload.size() > 1024) {
                // Ignore oversized input payloads
                if (type == "keydown" || type == "keyup") { InputMetrics::inc(InputMetrics::droppedKeyboard()); }
                else { InputMetrics::inc(InputMetrics::droppedMouse()); }
                return;
            }
        }

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

            // std::cout << "[Host] Received ping " << sequence_number
            //           << " from client at " << client_timestamp
            //           << ", host receive time: " << host_receive_time_ms << std::endl;
        }
        else if (type == "keydown" || type == "keyup" || type == "mousemove" || type == "mousedown" || type == "mouseup") {
            // Basic per-type rate limiting
            if (type == "keydown" || type == "keyup") {
                if (!g_keyBucket.consume(1.0)) { InputMetrics::inc(InputMetrics::droppedKeyboard()); return; }
            } else {
                if (!g_mouseBucket.consume(1.0)) { InputMetrics::inc(InputMetrics::droppedMouse()); return; }
            }
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
                // std::cout << "[Host] Received " << type << " event."
                //           << " Client send time: " << client_send_time
                //           << ", Host receive time: " << host_receive_time_ms
                //           << ", One-way latency: " << one_way_latency << " ms" << std::endl;
                lastInputLog = now;
            }

            // Validate key events: code must be known
            if ((type == "keydown" || type == "keyup")) {
                if (!message.contains("code") || !message["code"].is_string()) {
                    return;
                }
                const std::string code = message["code"].get<std::string>();
                if (kValidKeyCodes.find(code) == kValidKeyCodes.end()) {
                    // Unknown key code; ignore
                    return;
                }
                // Enqueue keyboard message directly to C++ queue
                std::string msgStr = message.dump();
                enqueueKeyboardMessage(msgStr);
                InputMetrics::inc(InputMetrics::enqueuedKeyboard());
                return; // Message handled, no need to continue processing
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
                    if (button < 0 || button > 4) {
                        std::cerr << "[WebSocket] Mouse button out of range" << std::endl;
                        return;
                    }
                }
                // Enqueue mouse message directly to C++ queue
                std::string msgStr = message.dump();
                enqueueMouseMessage(msgStr);
                InputMetrics::inc(InputMetrics::enqueuedMouse());
                return; // Message handled, no need to continue processing
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
            // Backward-compat: older schema used nested object
            std::cout << "[WebSocket] Received ice-candidate (legacy schema): " << message.dump() << std::endl;
            json candidateJson = message["candidate"];
            handleRemoteIceCandidate(candidateJson);
        }
        else if (type == "candidate") {
            // New schema: candidate is a top-level string, optional mid/index
            std::cout << "[WebSocket] Received candidate: " << message.dump() << std::endl;
            json candidateJson;
            if (message.contains("candidate") && message["candidate"].is_string()) {
                candidateJson["candidate"] = message["candidate"].get<std::string>();
            } else {
                std::cerr << "[WebSocket] Invalid candidate payload from server" << std::endl;
                return;
            }
            handleRemoteIceCandidate(candidateJson);
        }
        else if (type == "control") {
            if (message.value("action", std::string()) == "schema-error") {
                std::cerr << "[WebSocket] Server reported schema-error for a message sent by host." << std::endl;
            } else {
                std::cout << "[WebSocket] Control message: " << message.dump() << std::endl;
            }
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

    // Video is sent from Encoder::pushPacketToWebRTC -> EnqueueEncodedSample -> SenderLoop -> sendVideoSample (paced path).
    // The sendVideoPacket function exists only for debugging/testing and is gated in production builds.
    // Disable legacy queue-based sender threads to avoid duplicate RTP sends.
    // g_frame_thread = std::thread(&sendFrames);
    // g_sender_thread = std::thread(&senderThread);
}

void stopWebsocket() {
    stopMetricsExport();
    stopInputPollers();
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
    std::cout << "[WebSocket] Sent ICE candidate: " << iceMsg.dump() << std::endl;
}

// =======================================================================
//               DEPRECATED FUNCTIONS (Maintained for backward compatibility)
// =======================================================================

// External declarations for Go-exported functions (for backward compatibility)
extern "C" {
    char* getDataChannelMessage();
    char* getMouseChannelMessage();
    void freeCString(char* p);
}

// DEPRECATED: Use WebRTCWrapper::getDataChannelMessageString() instead
std::string getDataChannelMessageString() {
    try {
        return WebRTCWrapper::getDataChannelMessageString();
    } catch (const std::exception& e) {
        // Log error but don't throw from deprecated function
        std::cerr << "[WebSocket] Error in deprecated getDataChannelMessageString: " << e.what() << std::endl;
        return std::string();
    }
}

// DEPRECATED: Use WebRTCWrapper::getMouseChannelMessageString() instead
std::string getMouseChannelMessageString() {
    try {
        return WebRTCWrapper::getMouseChannelMessageString();
    } catch (const std::exception& e) {
        // Log error but don't throw from deprecated function
        std::cerr << "[WebSocket] Error in deprecated getMouseChannelMessageString: " << e.what() << std::endl;
        return std::string();
    }
}

// Helper functions to free the allocated memory (for backward compatibility)
extern "C" void freeDataChannelMessage(char* msg) {
    if (msg != nullptr) {
        freeCString(msg);
    }
}

extern "C" void freeMouseChannelMessage(char* msg) {
    if (msg != nullptr) {
        freeCString(msg);
    }
}

// Implementation of enqueue functions for blocking queues
void enqueueKeyboardMessage(const std::string& message) {
    KeyInputHandler::enqueueMessage(message);
}

void enqueueMouseMessage(const std::string& message) {
    MouseInputHandler::enqueueMessage(message);
}

// Note: getDataChannelMessage and getMouseChannelMessage are implemented in Go (main.go)
// and exported with //export, making them callable from C++
// The Go functions allocate memory with C.CString() that must be freed by the caller