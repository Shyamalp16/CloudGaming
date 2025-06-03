#include "MouseInputHandler.h"
#include <Windows.h>

using json = nlohmann::json;

namespace MouseInputHandler {
	static bool isRunning = false;
	static std::thread mouseMessageThread;
	static std::set<int> clientReportedMouseButtonsDown;
	static std::mutex mouseStateMutex;

	void mouseMessagePollingLoop();
	void simulateWindowsMouseEvent(const std::string& eventType, int x, int y, int button);
	void simulateMouseMove(int x, int y);
	void simulateMouseButton(const std::string& type, int button);

	void simulateWindowsMouseEvent(const std::string& eventType, int x, int y, int button) {
		INPUT input = { 0 };
		input.type = INPUT_MOUSE;
		input.mi.dx = 0;
		input.mi.dy = 0;
		input.mi.mouseData = 0;
		input.mi.dwFlags = 0;
		input.mi.time = 0;
		input.mi.dwExtraInfo = 0;

		if (eventType == "mousemove") {
			int screenWidth = GetSystemMetrics(SM_CXSCREEN);
			int screenHeight = GetSystemMetrics(SM_CYSCREEN);

			input.mi.dx = (LONG)(((double)x / screenWidth) * 65535.0);
			input.mi.dy = (LONG)(((double)y / screenHeight) * 65535.0);
			input.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK;
			std::cout << "[MouseInputHandler] Simulating Mouse Move to: (" << x << ", " << y << ") -> Normalized (" << input.mi.dx << ", " << input.mi.dy << ")" << std::endl;
		}
		else if (eventType == "mousedown" || eventType == "mouseup") {
			switch (button) {
			case 0: //Left
				input.mi.dwFlags = (eventType == "mousedown") ? MOUSEEVENTF_LEFTDOWN : MOUSEEVENTF_LEFTUP;
				break;
			case 1: //Middle
				input.mi.dwFlags = (eventType == "mousedown") ? MOUSEEVENTF_MIDDLEDOWN : MOUSEEVENTF_MIDDLEUP;
				break;
			case 2: //Right
				input.mi.dwFlags = (eventType == "mousedown") ? MOUSEEVENTF_RIGHTDOWN : MOUSEEVENTF_RIGHTUP;
				break;
			default:
				std::cerr << "[MouseInputHandler] Invalid mouse button: " << button << ". Valid values are 0 (Left), 1 (Middle), 2 (Right)." << std::endl;
				return;
			}
			std::cout << "[MouseInputHandler] Simulating Mouse Button " << button << " " << eventType << std::endl;
		}
		else {
			std::cerr << "[MouseInputHandler] Unknown event type for mouse simulation: " << eventType << std::endl;
			return;
		}

		UINT sent = SendInput(1, &input, sizeof(INPUT));
		if (sent != 1) {
			DWORD errorCode = GetLastError();
			std::cerr << "[MouseInputHandler] SendInput failed for mouse event '" << eventType << "'! Error Code: " << errorCode
				<< ", Error Message: " << std::system_category().message(errorCode) << std::endl;
		}
		else {
			std::cout << "[MouseInputHandler] SendInput succeeded for mouse event '" << eventType << "'" << std::endl;
		}
	}

	void mouseMessagePollingLoop() {
		std::cout << "[MouseInputHandler] Starting mouse message polling loop..." << std::endl;
		while (isRunning) {
			char* cMsg = getMouseChannelMessage();
			if (cMsg != nullptr) {
				std::string message;
				try {
					message = cMsg;
					//free(cMsg);
					//cMsg = nullptr;

					std::cout << "[MouseInputHandler] Received raw mouse message: " << message << std::endl;
					json j = json::parse(message);
					if (j.is_object() && j.contains("type")) {
						std::string jsType = j["type"].get<std::string>();
						int x = -1, y = -1, button = -1;

						if (jsType == "mousemove") {
							if (j.contains("x") && j.contains("y")) {
								x = j["x"].get<int>();
								y = j["y"].get<int>();
								std::cout << "[MouseInputHandler] Parsed mouse move to: (" << x << ", " << y << ")" << std::endl;
								simulateWindowsMouseEvent(jsType, x, y, -1);
							}
							else {
								std::cerr << "[MouseInputHandler] Missing 'x' or 'y' in mouse move message." << std::endl;
							}
						}
						else if (jsType == "mousedown" || jsType == "mouseup") {
							if (j.contains("x") && j.contains("y") && j.contains("button")) {
								x = j["x"].get<int>();
								y = j["y"].get<int>();
								button = j["button"].get<int>();
								std::cout << "[MouseInputHandler] Parsed - Type: " << jsType << ", X: " << x << ", Y: " << y << ", Button: " << button << std::endl;

								std::lock_guard<std::mutex> lock(mouseStateMutex);
								bool simulateAction = false;
								if (jsType == "mousedown") {
									if (clientReportedMouseButtonsDown.find(button) == clientReportedMouseButtonsDown.end()) {
										clientReportedMouseButtonsDown.insert(button);
										simulateAction = true;
									}
									else {
										std::cout << "[MouseInputHandler] State: Mouse button " << button << " already down. Ignoring re-press." << std::endl;
									}
								}
								else {
									if (clientReportedMouseButtonsDown.count(button)) {
										clientReportedMouseButtonsDown.erase(button);
										simulateAction = true;
									}
									else {
										std::cout << "[MouseInputHandler] State: Mouse button " << button << " was not reported down. Ignoring release." << std::endl;
									}
								}

								if (simulateAction) {
									simulateWindowsMouseEvent(jsType, x, y, button);
								}
							}
							else {
								std::cerr << "[MouseInputHandler] Malformed mouse click message (missing x/y/button): " << message << std::endl;
							}
						}
						else {
							std::cerr << "[MouseInputHandler] Unknown event type in mouse channel: " << jsType << " Message: " << message << std::endl;
						}
					}
					else {
						std::cerr << "[MouseInputHandler] Ignoring message: Invalid JSON format or missing 'type'. Message: " << message << std::endl;
					}
				}
				catch (const json::parse_error& e) {
					std::cerr << "[MouseInputHandler] JSON Parsing Error: " << e.what() << ". Message: " << message << std::endl;
					//free(cMsg);
				}
				catch (const std::exception& e) {
					std::cerr << "[MouseInputHandler] Generic Error Processing Message: " << e.what() << ". Message: " << message << std::endl;
					//free(cMsg);
				}
			}
			else {
				std::this_thread::sleep_for(std::chrono::milliseconds(1));
			}
		}
		std::cout << "[MouseInputHandler] Exiting mouse message polling loop." << std::endl;

		//cleanup
		std::cout << "[MouseInputHandler] Loop exited. Sending mouseup for all tracked buttons..." << std::endl;
		std::lock_guard<std::mutex> lock(mouseStateMutex);
		for (int buttonToRelease : clientReportedMouseButtonsDown) {
			std::cout << "[MouseInputHandler] Sending cleanup mouseup for button: " << buttonToRelease << std::endl;
			// You might need to retrieve the last known position to send with the cleanup mouseup.
			// For simplicity here, assuming (0,0) or no specific position is strictly needed for cleanup up.
			// If the client's `mouseup` sends `x`/`y`, the local `SimulateWindowsMouseEvent` will handle it.
			simulateWindowsMouseEvent("mouseup", -1, -1, buttonToRelease); // Pass -1 for x,y as it's a cleanup, not a physical move.
			clientReportedMouseButtonsDown.clear();
			std::cout << "[MouseInputHandler] Cleanup mouseup finished." << std::endl;
		}
	}

	void initializeMouseChannel() {
		std::cout << "[MouseInputHandler] DEBUG: initializeMouseChannel called." << std::endl; // ADD THIS
		if (!isRunning) {
			isRunning = true;
			std::lock_guard<std::mutex> lock(mouseStateMutex);
			clientReportedMouseButtonsDown.clear();

			mouseMessageThread = std::thread(mouseMessagePollingLoop);
			std::cout << "[MouseInputHandler] Polling started for mouse channel messages" << std::endl;
		}
		else {
			std::cout << "[MouseInputHandler] Mouse polling thread already running." << std::endl;
		}
	}

	void cleanup() {
		if (isRunning) {
			isRunning = false;
			if (mouseMessageThread.joinable()) {
				mouseMessageThread.join();
			}
			std::cout << "[MouseInputHandler] Mouse polling stopped" << std::endl;
		}
		else {
			std::cout << "[MouseInputHandler] Cleanup called, but mouse polling was not running." << std::endl;
		}
	}

	extern "C" void initMouseInputHandler() {
		std::cout << "[MouseInputHandler] initMouseInputHandler called" << std::endl;
		MouseInputHandler::initializeMouseChannel();
	}

	extern "C" void stopMouseInputHandler() {
		std::cout << "[MouseInputHandler] stopMouseInputHandler called (C export)" << std::endl;
		MouseInputHandler::cleanup();
	}
}