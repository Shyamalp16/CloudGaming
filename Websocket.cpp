#include "Websocket.h"
#include "MySetRemoteObserver.h"
#include "MyCreateSdpObserver.h"

typedef websocketpp::client<websocketpp::config::asio_client> client;
using json = nlohmann::json;

client wsClient;
websocketpp::connection_hdl g_connectionHandle;
std::string uri = "ws://localhost:3000";

//static std::string remoteOfferSDP;
//static std::vector<std::string> remoteIceCandidates;
//static bool answerSent = false;

static webrtc::scoped_refptr<webrtc::PeerConnectionFactoryInterface> peerConnectionFactory;
static webrtc::scoped_refptr<webrtc::PeerConnectionInterface> peerConnection;
static bool answerCreated = false;

void on_open(client* c, websocketpp::connection_hdl hdl);
void on_message(websocketpp::connection_hdl hdl, client::message_ptr msg);
void send_message(const json& message);

class CustomPeerConnectionObserver : public webrtc::PeerConnectionObserver {
public:
	//will be called when new ICE candidate is found
	void OnIceCandidate(const webrtc::IceCandidateInterface* candidate) override {
		std::string candidateString;
		candidate->ToString(&candidateString);

		//send this over via websocket
		json msg;
		msg["type"] = "ice-candidate";
		msg["candidate"] = {
			{"candidate", candidateString },
			{"sdpMid", candidate->sdp_mid()},
			{"sdpMLineIndex", candidate->sdp_mline_index()}
		};

		send_message(msg);
		std::cout << "[C++ Host] OnIceCandidate: " << candidateString << std::endl;
	}

	void OnIceConnectionChange(webrtc::PeerConnectionInterface::IceConnectionState new_state) override {
		std::cout << "[C++ Host] OnIceConnectionChange To: " << new_state << std::endl;
	}

	void OnDataChannel(webrtc::scoped_refptr<webrtc::DataChannelInterface> data_channel) override {
		std::cout << "[C++ Host] OnDataChannel: \n" << data_channel->label() << std::endl;
	}

	void OnSignalingChange(webrtc::PeerConnectionInterface::SignalingState) override {}
	void OnStandardizedIceConnectionChange(webrtc::PeerConnectionInterface::IceConnectionState) override {}
	void OnConnectionChange(webrtc::PeerConnectionInterface::PeerConnectionState) override {}
	void OnIceGatheringChange(webrtc::PeerConnectionInterface::IceGatheringState) override {}
	void OnRenegotiationNeeded() override {}
	void OnTrack(rtc::scoped_refptr<webrtc::RtpTransceiverInterface>) override {}
};

static std::unique_ptr<CustomPeerConnectionObserver> observer;

bool createPeerConnection() {
	if (!peerConnectionFactory) {
		rtc::InitializeSSL();
		//for testing just using a single thread for network, worker, signaling, might expand on them as needed.
		rtc::Thread* networkThread = rtc::Thread::CreateWithSocketServer().release();
		rtc::Thread* workerThread = rtc::Thread::Create().release();
		rtc::Thread* signalingThread = rtc::Thread::Create().release();

		networkThread->Start();
		workerThread->Start();
		signalingThread->Start();

		peerConnectionFactory = webrtc::CreatePeerConnectionFactory(
		networkThread,
		workerThread,
		signalingThread,
		nullptr,
		webrtc::CreateBuiltinAudioEncoderFactory(),
		webrtc::CreateBuiltinAudioDecoderFactory(),
		webrtc::CreateBuiltinVideoEncoderFactory(),
		webrtc::CreateBuiltinVideoDecoderFactory(),
		nullptr, //audio mixer
		nullptr  //audio processing
		);

		if (!peerConnectionFactory) {
			std::cerr << "[C++ Host] Error creating peer connection factory" << std::endl;
			return false;
		}
	}

	//create peer connection
	webrtc::PeerConnectionInterface::RTCConfiguration config;
	config.sdp_semantics = webrtc::SdpSemantics::kUnifiedPlan;
	
	webrtc::PeerConnectionInterface::IceServer stunServer;
	stunServer.uri = "stun:stun.l.google.com:19302";
	config.servers.push_back(stunServer);

	observer = std::make_unique<CustomPeerConnectionObserver>();
	webrtc::PeerConnectionDependencies deps(observer.get());

	auto resultOrError = peerConnectionFactory->CreatePeerConnectionOrError(config, std::move(deps));
	if (!resultOrError.ok()) {
		std::cerr << "[C++ Host] Error creating peer connection: " << resultOrError.error().message() << std::endl;
		return false;
	}

	std::cout << "[C++ Host] Peer Connection Created." << std::endl;
	return true;
}

void handleOffer(const std::string& offer) {
	if (!peerConnection) {
		if (!createPeerConnection()) {
			return;
		}
	}

	webrtc::SdpParseError error;
	auto sessionDescription = webrtc::CreateSessionDescription(webrtc::SdpType::kOffer, offer, &error);
	if (!sessionDescription) {
		std::cerr << "[C++ Host] Error parsing incoming offer: " << error.description << std::endl;
		return;
	}

	//set remote description
	peerConnection->SetRemoteDescription(
		std::move(sessionDescription),
		webrtc::scoped_refptr<webrtc::SetRemoteDescriptionObserverInterface>(
			new rtc::RefCountedObject<MySetRemoteObserver>())
	);


	//create answer
	std::cout << "[C++ Host] Remote Offer Set, Creating Answer" << std::endl;
	webrtc::PeerConnectionInterface::RTCOfferAnswerOptions options;

	peerConnection->CreateAnswer(new rtc::RefCountedObject<MyCreateSdpObserver>(), options);
	std::cout << "[C++ Host] Answer Created and sent" << std::endl;
}

void handleRemoteIceCandidate(const json& candidateJson) {
	if (!peerConnection) {
		std::cerr << "[C++ Host] No peer connection to add ICE candidate" << std::endl;
		return;
	}

	std::string candidateStr = candidateJson.value("candidate", "");
	std::string sdpMid = candidateJson.value("sdpMid", "");
	int sdpMLineIndex = candidateJson.value("sdpMLineIndex", 0);

	webrtc::SdpParseError error;
	webrtc::scoped_refptr<webrtc::IceCandidateInterface> iceCandidate(webrtc::CreateIceCandidate(sdpMid, sdpMLineIndex, candidateStr, &error));

	if (!iceCandidate) {
		std::cerr << "[C++ Host] Error parsing ICE candidate: " << error.description << std::endl;
		return;
	}

	if (!peerConnection->AddIceCandidate(iceCandidate.get())) {
		std::cerr << "[C++ Host] Error adding ICE candidate" << std::endl;
	}
	else {
		std::cout << "[C++ Host] ICE Candidate added" << std::endl;
	}
}

void sendAnswer() {
	/*if (answerSent) {
		std::cout << "[WebSocket] Answer already sent\n";
		return;
	}*/

	json answerMsg;
	answerMsg["type"] = "answer";
	answerMsg["sdp"] = "fake sdp from C++";

	send_message(answerMsg);
	//answerSent = true;
	std::cout << "[WebSocket] Answer sent\n";
}

void on_open(client* c, websocketpp::connection_hdl hdl) {
	std::cout << "[WebSocket] Connected opened to " << uri << std::endl;
	g_connectionHandle = hdl;
}

//callback for when we get a message from server
void on_message(websocketpp::connection_hdl hdl, client::message_ptr msg) {
	try {
		json message = json::parse(msg->get_payload());
		std::string type = message["type"];

		if (type == "offer") {
			std::cout << "[WebSocket] Received offer from server: \n" << message.dump() << std::endl;
			std::string sdp = message.value("sdp", "");
			handleOffer(sdp);
		}
		else if (type == "answer") {
			std::cout << "[WebSocket] Received answer from server" << message.dump() << std::endl;
			//set remote description for peer connection
		}
		else if (type == "ice-candidate") {
			std::cout << "[WebSocket] Received ice candidate from server" << message.dump() << std::endl;
			json candidateJson = message["candidate"]; //might need checks
			handleRemoteIceCandidate(candidateJson);
		}else {
			std::cout << "[WebSocket] Received Message: " << message.dump() << std::endl;
		}
	}catch (const std::exception& e) {
			std::cerr << "[WebSocket] Error parsing message from server: " << e.what() << std::endl;
	}catch (...) {
		std::cerr << "[WebSocket] Unknown error parsing message from server" << std::endl;
	}
}


//to send json message
void send_message(const json& message) {
	try {
		if (!g_connectionHandle.lock()) {
			std::cerr << "[WebSocket] Cannot send message: Connection is invalid\n";
			return;
		}

		std::string payload = message.dump();
		wsClient.send(g_connectionHandle, payload, websocketpp::frame::opcode::text);
		std::cout << "[WebSocket] Sent message: " << payload << std::endl;

	}catch (const std::exception& e) {
		std::cerr << "[WebSocket] Error sending message: " << e.what() << std::endl;
	}catch (...) {
		std::cerr << "[WebSocket] Unknown error sending message" << std::endl;
	}
}


void initWebsocket() {
	//init asio and handler
	wsClient.init_asio();
	wsClient.set_open_handler(std::bind(&on_open, &wsClient, std::placeholders::_1));
	wsClient.set_message_handler(&on_message);

	//connection pointer
	websocketpp::lib::error_code ec;
	client::connection_ptr con = wsClient.get_connection(uri, ec);
	if (ec) {
		std::cerr << "[WebSocket] Error connecting to because:" << uri << ": " << ec.message() << std::endl;
		return;
	}
	wsClient.connect(con);
	std::wcout << L"[WebSocket] Connection made\n";

	//run in separate thread
	std::thread t([&](){
		try {
			//std::this_thread::sleep_for(std::chrono::seconds(5));
			wsClient.run();
		}
		catch (const std::exception& ex) {
			std::cerr << "[WebSocket] run() Threw Exception: " << ex.what() << std::endl;
		}
		catch (...) {
			std::cerr << "[WebSocket] run() Threw Unknown Exception" << std::endl;
		}
	});
	t.detach();
	std::wcout << L"[WebSocket] Websocket started in a separate thread\n";

	//sleep to allow to connect
	//std::this_thread::sleep_for(std::chrono::seconds(2));

	//just for testing, wont wait for open callback 
	/*json tstMsg;
	tstMsg["type"] = "test";
	tstMsg["data"] = "Test message from C++";
	send_message(tstMsg);

	std::this_thread::sleep_for(std::chrono::seconds(5));
	std::wcout << L"[WebSocket] Closing websocket\n";
	wsClient.stop();*/
	//t.join();
}