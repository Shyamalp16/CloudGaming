#include "KeyInputHandler.h"

using json = nlohmann::json;

extern "C" {
	char* getDataChannelMessage();
}

namespace KeyInputHandler {
	static bool isRunning = false;
	static std::thread messageThread;

	void simulateKeyPress(const std::string& key) {
		INPUT input = { 0 };
		input.type = INPUT_KEYBOARD;
		input.ki.wVk = VkKeyScan(key[0] & 0xFF);
		input.ki.dwFlags = 0; // Key down
		SendInput(1, &input, sizeof(INPUT));

		input.ki.dwFlags = KEYEVENTF_KEYUP; // Key up
		SendInput(1, &input, sizeof(INPUT));
		std::wcout << L"[KeyInputHandler] Simulated keypress: " << key.c_str() << L"\n";
	}

	void messagePollingLoop() {
		while (isRunning) {
			//std::cout << "[KeyInputHandler] Polling for message..." << std::endl;
			char* msg = getDataChannelMessage();
			if (msg != nullptr) {
				try {
					std::string message(msg);
					free(msg);
					json j = json::parse(message);
					if (j.is_object() && j.contains("key")) {
						std::string key = j["key"].get<std::string>();
						simulateKeyPress(key);
					}else {
						std::cout << "[KeyInputHandler] Ignoring non-key message: " << message << std::endl;
					}
				}
				catch (const std::exception& e) {
					std::cerr << "[KeyInputHandler] Error parsing keypress data: " << e.what() << std::endl;
				}
			}
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(100000));
	}

	void initializeDataChannel() {
		if (!isRunning) {
			isRunning = true;
			messageThread = std::thread(messagePollingLoop);
			std::cout << "[KeyInputHandler] Polling started for data channel messages" << std::endl;
		}
	}

	void cleanup() {
		if (isRunning) {
			isRunning = false;
			if (messageThread.joinable()) {
				messageThread.join();
			}
			std::cout << "[KeyInputHandler] Polling stopped" << std::endl;
		}
	}
}

extern "C" void initKeyInputHandler() {
	std::cout << "[KeyInputHandler] initKeyInputHandler called" << std::endl;
	KeyInputHandler::initializeDataChannel();
}