#include "KeyInputHandler.h"

using json = nlohmann::json;

extern "C" {
	char* getDataChannelMessage();
}
bool isRunning;

namespace KeyInputHandler {
	static std::thread messageThread;

	WORD MapJavaScriptCodeToVK(const std::string& jsCode) {
		static const std::map<std::string, WORD> vkMap = {
			//Letters
			{"KeyA", 'A'}, {"KeyB", 'B'}, {"KeyC", 'C'}, {"KeyD", 'D'},
			{"KeyE", 'E'}, {"KeyF", 'F'}, {"KeyG", 'G'}, {"KeyH", 'H'},
			{"KeyI", 'I'}, {"KeyJ", 'J'}, {"KeyK", 'K'}, {"KeyL", 'L'},
			{"KeyM", 'M'}, {"KeyN", 'N'}, {"KeyO", 'O'}, {"KeyP", 'P'},
			{"KeyQ", 'Q'}, {"KeyR", 'R'}, {"KeyS", 'S'}, {"KeyT", 'T'},
			{"KeyU", 'U'}, {"KeyV", 'V'}, {"KeyW", 'W'}, {"KeyX", 'X'},
			{"KeyY", 'Y'}, {"KeyZ", 'Z'},

			// Numbers
			{"Digit1", '1'}, {"Digit2", '2'}, {"Digit3", '3'}, {"Digit4", '4'},
			{"Digit5", '5'}, {"Digit6", '6'}, {"Digit7", '7'}, {"Digit8", '8'},
			{"Digit9", '9'}, {"Digit0", '0'},

			// Numpad Numbers
			{"Numpad1", VK_NUMPAD1}, {"Numpad2", VK_NUMPAD2}, {"Numpad3", VK_NUMPAD3},
			{"Numpad4", VK_NUMPAD4}, {"Numpad5", VK_NUMPAD5}, {"Numpad6", VK_NUMPAD6},
			{"Numpad7", VK_NUMPAD7}, {"Numpad8", VK_NUMPAD8}, {"Numpad9", VK_NUMPAD9},
			{"Numpad0", VK_NUMPAD0},

			// Numpad Operators
			{"NumpadDecimal", VK_DECIMAL}, {"NumpadAdd", VK_ADD},
			{"NumpadSubtract", VK_SUBTRACT}, {"NumpadMultiply", VK_MULTIPLY},
			{"NumpadDivide", VK_DIVIDE},

			// Function Keys
			{"F1", VK_F1}, {"F2", VK_F2}, {"F3", VK_F3}, {"F4", VK_F4},
			{"F5", VK_F5}, {"F6", VK_F6}, {"F7", VK_F7}, {"F8", VK_F8},
			{"F9", VK_F9}, {"F10", VK_F10}, {"F11", VK_F11}, {"F12", VK_F12},

			// Arrow Keys
			{"ArrowUp", VK_UP}, {"ArrowDown", VK_DOWN},
			{"ArrowLeft", VK_LEFT}, {"ArrowRight", VK_RIGHT},

			// Modifiers
			{"ShiftLeft", VK_LSHIFT}, {"ShiftRight", VK_RSHIFT},
			{"ControlLeft", VK_LCONTROL}, {"ControlRight", VK_RCONTROL},
			{"AltLeft", VK_LMENU}, {"AltRight", VK_RMENU},

			// Other Keys
			{"Enter", VK_RETURN}, {"NumpadEnter", VK_RETURN},
			{"Escape", VK_ESCAPE}, {"Tab", VK_TAB},
			{"Space", VK_SPACE}, {"Backspace", VK_BACK}, {"Delete", VK_DELETE},
			{"Home", VK_HOME}, {"End", VK_END},
			{"PageUp", VK_PRIOR}, {"PageDown", VK_NEXT},
			{"CapsLock", VK_CAPITAL}, {"NumLock", VK_NUMLOCK},
			{"ScrollLock", VK_SCROLL}, {"Insert", VK_INSERT},
			{"ContextMenu", VK_APPS},
			{"MetaLeft", VK_LWIN}, {"MetaRight", VK_RWIN},

			// Punctuation
			{"Backquote", VK_OEM_3}, {"Minus", VK_OEM_MINUS}, {"Equal", VK_OEM_PLUS},
			{"BracketLeft", VK_OEM_4}, {"BracketRight", VK_OEM_6}, {"Backslash", VK_OEM_5},
			{"Semicolon", VK_OEM_1}, {"Quote", VK_OEM_7},
			{"Comma", VK_OEM_COMMA}, {"Period", VK_OEM_PERIOD}, {"Slash", VK_OEM_2}
		};

		auto it = vkMap.find(jsCode);
		if (it != vkMap.end()) {
			return it->second;
		}
		//mapping not found
		std::cerr << "[MapJavaScriptCodeToVK] Warning: No VK mapping for code: " << jsCode << std::endl;
		return 0;
	}

	void SimulateWindowsKeyEvent(const std::string& eventCode, bool isKeyDown){
		WORD virtualKeyCode = MapJavaScriptCodeToVK(eventCode);
		if (virtualKeyCode == 0) {
			std::cerr << "[SimulateWindowsKeyEvent] Warning: No VK code mapping for JS code '" << eventCode << "'. Ignoring." << std::endl;
			return;
		}

		INPUT input = { 0 };
		input.type = INPUT_KEYBOARD;
		input.ki.time = 0;
		input.ki.dwExtraInfo = 0;
		//input.ki.wVk = virtualKeyCode;
		//input.ki.dwFlags = 0; //default keydown

		/*if (!isKeyDown) {
			input.ki.dwFlags |= KEYEVENTF_KEYUP;
		}*/

		WORD scanCode = MapVirtualKey(virtualKeyCode, MAPVK_VK_TO_VSC);
		if (scanCode != 0) {
			input.ki.dwFlags = KEYEVENTF_SCANCODE;
			if (!isKeyDown) {
				input.ki.dwFlags |= KEYEVENTF_KEYUP;
			}
			input.ki.wScan = static_cast<WORD>(scanCode);
			input.ki.wVk = 0; 
			std::cout << "[SimulateWindowsKeyEvent] Sending Input (Scan Code) - Code: '" << eventCode << "', Scan: " << scanCode << ", Flags: " << input.ki.dwFlags
				<< (isKeyDown ? " (DOWN)" : " (UP)") << std::endl;
		}else {
			std::cerr << "[SimulateWindowsKeyEvent] Warning: Could not map VK " << virtualKeyCode << " to Scan Code for JS code '" << eventCode << "'. Using VK only." << std::endl;
			input.ki.dwFlags = 0;
			if (!isKeyDown) {
				input.ki.dwFlags |= KEYEVENTF_KEYUP;
			}
			input.ki.wVk = virtualKeyCode;
			std::cout << "[SimulateWindowsKeyEvent] Sending Input (VK Fallback) - Code: '" << eventCode << "', VK: " << virtualKeyCode << ", Flags: " << input.ki.dwFlags
				<< (isKeyDown ? " (DOWN)" : " (UP)") << std::endl;
		}

		UINT sent = SendInput(1, &input, sizeof(INPUT));
		if (sent != 1) {
			std::cerr << "[SimulateWindowsKeyEvent] SendInput failed! Error Code: " << GetLastError() << std::endl;
		}
	}

	void messagePollingLoop() {
		std::cout << "[KeyInputHandler] Starting message polling loop..." << std::endl;
		while (isRunning) {
			char* cMsg = getDataChannelMessage();

			if (cMsg != nullptr) {
				std::string message;
				try {
					message = cMsg;
					//free(cMsg);
					//cMsg = nullptr;
					std::cout << "[KeyInputHandler] Received message string: " << message << std::endl;

					json j = json::parse(message);
					if (j.is_object() && j.contains("code") && j.contains("type")) {
						//std::string jsKey = j["key"].get<std::string>();
						std::string jsCode = j["code"].get<std::string>();
						std::string jsType = j["type"].get<std::string>();

						std::cout << "[KeyInputHandler] Parsed - Code: " << jsCode << ", Type: " << jsType << std::endl;
						bool isKeyDown = (jsType == "keydown");
						SimulateWindowsKeyEvent(jsCode, isKeyDown);
					}else {
						std::cerr << "[KeyInputHandler] Ignoring message: Invalid JSON format or missing 'code'/'type'. Message: " << message << std::endl;
					}
				}
				catch (const json::parse_error& e) {
					std::cerr << "[KeyInputHandler] JSON Parsing Error: " << e.what() << ". Message: " << message << std::endl;
					if (cMsg != nullptr) {
						/*free(cMsg);
						cMsg = nullptr;*/
					}
				}
				catch (const std::exception& e) {
					std::cerr << "[KeyInputHandler] Generic Error Processing Message: " << e.what() << ". Message: " << message << std::endl;
					if (cMsg != nullptr) {
						/*free(cMsg);
						cMsg = nullptr;*/
					}
				}
			}
			else {
				std::this_thread::sleep_for(std::chrono::milliseconds(10)); 
			}
		}
		std::cout << "[KeyInputHandler] Exiting message polling loop." << std::endl;
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