#include "Websocket.h"

typedef websocketpp::client<websocketpp::config::asio_client> client;
using json = nlohmann::json;

client wsClient;
websocketpp::connection_hdl g_connectionHandle;
std::string uri = "ws://localhost:3000";

void on_open(client* c, websocketpp::connection_hdl hdl);
void on_message(websocketpp::connection_hdl hdl, client::message_ptr msg);
void send_message(const json& message);

void on_open(client* c, websocketpp::connection_hdl hdl) {
	std::cout << "[WebSocket] Connected opened to " << uri << std::endl;
	g_connectionHandle = hdl;
	json helloMsg;
	helloMsg["type"] = "hello";
	helloMsg["data"] = "Hello from C++";
	send_message(helloMsg);
}

//callback for when we get a message from server
void on_message(websocketpp::connection_hdl hdl, client::message_ptr msg) {
	try {
		json message = json::parse(msg->get_payload());
		std::string type = message["type"];
		if (type == "offer") {
			//handle offer
		}
		else if (type == "answer") {
			//set remote description for peer connection
			std::cout << "[WebSocket] Received answer from server" << message.dump() << std::endl;
		}
		else if (type == "ice-candidate") {
			//add candidate to peer connection
			std::cout << "[WebSocket] Received ice candidate from server" << message.dump() << std::endl;
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
		wsClient.run();
	});
	std::wcout << L"[WebSocket] Websocket started in a separate thread\n";

	//sleep to allow to connect
	std::this_thread::sleep_for(std::chrono::seconds(2));

	//just for testing, wont wait for open callback 
	json tstMsg;
	tstMsg["type"] = "test";
	tstMsg["data"] = "Test message from C++";
	send_message(tstMsg);

	std::this_thread::sleep_for(std::chrono::seconds(5));
	std::wcout << L"[WebSocket] Closing websocket\n";
	wsClient.stop();
	t.join();
}