#include "KeyInputHandler.h"
#include "ShutdownManager.h"
#include "pion_webrtc.h"
#include <unordered_map>
#include <atomic>

using json = nlohmann::json;

static std::atomic_bool isRunning;

namespace KeyInputHandler {
	static std::thread messageThread;
	static std::set<WORD> clientReportedKeysDown;
	static std::mutex clientKeysMutex;
	static std::unordered_map<WORD, std::string> vkDownToJsCode;

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

	// Direct mapping from JS key codes to hardware scancodes for layout-independent input
	static const std::map<std::string, WORD> scanCodeMap = {
		// Letters (QWERTY layout scancodes)
		{"KeyA", 0x1E}, {"KeyB", 0x30}, {"KeyC", 0x2E}, {"KeyD", 0x20},
		{"KeyE", 0x12}, {"KeyF", 0x21}, {"KeyG", 0x22}, {"KeyH", 0x23},
		{"KeyI", 0x17}, {"KeyJ", 0x24}, {"KeyK", 0x25}, {"KeyL", 0x26},
		{"KeyM", 0x32}, {"KeyN", 0x31}, {"KeyO", 0x18}, {"KeyP", 0x19},
		{"KeyQ", 0x10}, {"KeyR", 0x13}, {"KeyS", 0x1F}, {"KeyT", 0x14},
		{"KeyU", 0x16}, {"KeyV", 0x2F}, {"KeyW", 0x11}, {"KeyX", 0x2D},
		{"KeyY", 0x15}, {"KeyZ", 0x2C},

		// Numbers (top row)
		{"Digit1", 0x02}, {"Digit2", 0x03}, {"Digit3", 0x04}, {"Digit4", 0x05},
		{"Digit5", 0x06}, {"Digit6", 0x07}, {"Digit7", 0x08}, {"Digit8", 0x09},
		{"Digit9", 0x0A}, {"Digit0", 0x0B},

		// Punctuation keys - using hardware scancodes for layout independence
		{"Backquote", 0x29}, {"Minus", 0x0C}, {"Equal", 0x0D},
		{"BracketLeft", 0x1A}, {"BracketRight", 0x1B}, {"Backslash", 0x2B},
		{"Semicolon", 0x27}, {"Quote", 0x28},
		{"Comma", 0x33}, {"Period", 0x34}, {"Slash", 0x35},

		// Special keys
		{"Space", 0x39}, {"Enter", 0x1C}, {"Backspace", 0x0E}, {"Tab", 0x0F},
		{"Escape", 0x01}, {"Delete", 0x53}, {"Insert", 0x52},
		{"Home", 0x47}, {"End", 0x4F}, {"PageUp", 0x49}, {"PageDown", 0x51},

		// Arrow keys
		{"ArrowUp", 0x48}, {"ArrowDown", 0x50}, {"ArrowLeft", 0x4B}, {"ArrowRight", 0x4D},

		// Function keys
		{"F1", 0x3B}, {"F2", 0x3C}, {"F3", 0x3D}, {"F4", 0x3E},
		{"F5", 0x3F}, {"F6", 0x40}, {"F7", 0x41}, {"F8", 0x42},
		{"F9", 0x43}, {"F10", 0x44}, {"F11", 0x57}, {"F12", 0x58},

		// Modifiers
		{"ShiftLeft", 0x2A}, {"ShiftRight", 0x36},
		{"ControlLeft", 0x1D}, {"ControlRight", 0x1D}, // Right Ctrl is extended
		{"AltLeft", 0x38}, {"AltRight", 0x38}, // Right Alt is extended

		// Lock keys
		{"CapsLock", 0x3A}, {"NumLock", 0x45}, {"ScrollLock", 0x46},

		// Windows keys
		{"MetaLeft", 0x5B}, {"MetaRight", 0x5C}, {"ContextMenu", 0x5D}
	};

	WORD MapJavaScriptCodeToVK(const std::string& jsCode) {
		auto it = vkMap.find(jsCode);
		if (it != vkMap.end()) {
			return it->second;
		}
		//mapping not found
		std::cerr << "[MapJavaScriptCodeToVK] Warning: No VK mapping for code: " << jsCode << std::endl;
		return 0;
	}

	WORD MapJavaScriptCodeToScanCode(const std::string& jsCode) {
		auto it = scanCodeMap.find(jsCode);
		if (it != scanCodeMap.end()) {
			return it->second;
		}
		// No direct scancode mapping, fall back to virtual key conversion
		WORD vk = MapJavaScriptCodeToVK(jsCode);
		if (vk != 0) {
			// Use MapVirtualKeyEx with current keyboard layout for better compatibility
			HKL hkl = GetKeyboardLayout(0);
			return MapVirtualKeyEx(vk, MAPVK_VK_TO_VSC_EX, hkl);
		}
		std::cerr << "[MapJavaScriptCodeToScanCode] Warning: No scan code mapping for code: " << jsCode << std::endl;
		return 0;
	}

	void simulateKeyPress(const std::string& key) {
		std::cout << "[KeyInputHandler] Simulating key press for: '" << key << "'" << std::endl;

		// Validate the key code
		WORD virtualKeyCode = MapJavaScriptCodeToVK(key);
		if (virtualKeyCode == 0) {
			std::cerr << "[KeyInputHandler] Error: Invalid key code '" << key << "'. Cannot simulate key press." << std::endl;
			return;
		}

		// Simulate key down
		SimulateWindowsKeyEvent(key, true);

		// Small delay between down and up (optional, helps with some applications)
		std::this_thread::sleep_for(std::chrono::milliseconds(10));

		// Simulate key up
		SimulateWindowsKeyEvent(key, false);

		std::cout << "[KeyInputHandler] Key press simulation completed for: '" << key << "'" << std::endl;
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

		bool isExtendedKey = (
			virtualKeyCode == VK_UP || virtualKeyCode == VK_DOWN ||
			virtualKeyCode == VK_LEFT || virtualKeyCode == VK_RIGHT ||
			virtualKeyCode == VK_HOME || virtualKeyCode == VK_END ||
			virtualKeyCode == VK_PRIOR || virtualKeyCode == VK_NEXT ||
			virtualKeyCode == VK_INSERT || virtualKeyCode == VK_DELETE ||
			virtualKeyCode == VK_RCONTROL || virtualKeyCode == VK_RMENU ||
			virtualKeyCode == VK_LWIN || virtualKeyCode == VK_RWIN ||
			virtualKeyCode == VK_APPS ||
			(virtualKeyCode == VK_RETURN && eventCode == "NumpadEnter")
		);

		// Decide whether to use scancodes or virtual keys based on key type
		// Prioritize scancodes for layout-sensitive keys (punctuation, etc.)
		bool preferScanCode = true;

		// Use virtual keys only for keys that are universally consistent across layouts
		if ((virtualKeyCode >= 'A' && virtualKeyCode <= 'Z') ||
			(virtualKeyCode >= '0' && virtualKeyCode <= '9') ||
			virtualKeyCode == VK_SPACE ||
			virtualKeyCode == VK_RETURN ||
			virtualKeyCode == VK_BACK ||
			virtualKeyCode == VK_TAB ||
			virtualKeyCode == VK_ESCAPE ||
			virtualKeyCode == VK_UP || virtualKeyCode == VK_DOWN ||
			virtualKeyCode == VK_LEFT || virtualKeyCode == VK_RIGHT ||
			virtualKeyCode == VK_HOME || virtualKeyCode == VK_END ||
			virtualKeyCode == VK_PRIOR || virtualKeyCode == VK_NEXT ||
			virtualKeyCode == VK_INSERT || virtualKeyCode == VK_DELETE ||
			virtualKeyCode == VK_F1 || virtualKeyCode == VK_F2 || virtualKeyCode == VK_F3 ||
			virtualKeyCode == VK_F4 || virtualKeyCode == VK_F5 || virtualKeyCode == VK_F6 ||
			virtualKeyCode == VK_F7 || virtualKeyCode == VK_F8 || virtualKeyCode == VK_F9 ||
			virtualKeyCode == VK_F10 || virtualKeyCode == VK_F11 || virtualKeyCode == VK_F12 ||
			virtualKeyCode == VK_CAPITAL || virtualKeyCode == VK_NUMLOCK || virtualKeyCode == VK_SCROLL) {
			preferScanCode = false;
		}

		if (preferScanCode) {
			// Try to use direct scancode mapping first for better layout independence
			WORD scanCode = MapJavaScriptCodeToScanCode(eventCode);
			if (scanCode != 0) {
				input.ki.wScan = scanCode;
				input.ki.dwFlags = KEYEVENTF_SCANCODE;
				if (isExtendedKey) {
					input.ki.dwFlags |= KEYEVENTF_EXTENDEDKEY;
				}
				if (!isKeyDown) {
					input.ki.dwFlags |= KEYEVENTF_KEYUP;
				}
				input.ki.wVk = 0;
				std::cout << "[SimulateWindowsKeyEvent] Sending Input (Direct Scan Code) - JSCode: '" << eventCode
					<< "', VK_Mapped: " << virtualKeyCode << ", Scan: 0x" << std::hex << scanCode << std::dec
					<< ", Flags: 0x" << std::hex << input.ki.dwFlags << std::dec
					<< (isKeyDown ? " (DOWN)" : " (UP)") << std::endl;
			} else {
				// Fallback: try to convert VK to scancode with current layout
				HKL hkl = GetKeyboardLayout(0);
				scanCode = MapVirtualKeyEx(virtualKeyCode, MAPVK_VK_TO_VSC_EX, hkl);
				if (scanCode != 0) {
					input.ki.wScan = scanCode;
					input.ki.dwFlags = KEYEVENTF_SCANCODE;
					if (isExtendedKey) {
						input.ki.dwFlags |= KEYEVENTF_EXTENDEDKEY;
					}
					if (!isKeyDown) {
						input.ki.dwFlags |= KEYEVENTF_KEYUP;
					}
					input.ki.wVk = 0;
					std::cout << "[SimulateWindowsKeyEvent] Sending Input (VK->Scan Fallback) - JSCode: '" << eventCode
						<< "', VK: " << virtualKeyCode << ", Scan: 0x" << std::hex << scanCode << std::dec
						<< ", Flags: 0x" << std::hex << input.ki.dwFlags << std::dec
						<< (isKeyDown ? " (DOWN)" : " (UP)") << std::endl;
				} else {
					std::cerr << "[SimulateWindowsKeyEvent] Warning: Could not map to scan code for JS code '" << eventCode << "'. Using VK." << std::endl;
					goto useVirtualKey;
				}
			}
		} else {
			useVirtualKey:
			input.ki.wVk = virtualKeyCode;
			input.ki.dwFlags = 0;
			if (isExtendedKey) {
				input.ki.dwFlags |= KEYEVENTF_EXTENDEDKEY;
			}
			if (!isKeyDown) {
				input.ki.dwFlags |= KEYEVENTF_KEYUP;
			}
			std::cout << "[SimulateWindowsKeyEvent] Sending Input (Virtual Key) - JSCode: '" << eventCode
				<< "', VK: " << virtualKeyCode
				<< ", Flags: 0x" << std::hex << input.ki.dwFlags << std::dec
				<< (isKeyDown ? " (DOWN)" : " (UP)") << std::endl;
		}

		UINT sent = SendInput(1, &input, sizeof(INPUT));
		if (sent != 1) {
			DWORD errorCode = GetLastError();
			std::cerr << "[SimulateWindowsKeyEvent] SendInput failed for '" << eventCode << "' ("
				<< (isKeyDown ? "DOWN" : "UP") << ")! Error Code: " << errorCode
				<< ", Error Message: ";
			char errorMsg[256];
			FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
				NULL, errorCode, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
				errorMsg, sizeof(errorMsg), NULL);
			std::cerr << errorMsg << std::endl;
		}
		else {
			std::cout << "[SimulateWindowsKeyEvent] SendInput succeeded for '" << eventCode << "' ("
				<< (isKeyDown ? "DOWN" : "UP") << ")" << std::endl;
		}
	}

	void messagePollingLoop() {
		std::cout << "[KeyInputHandler] Starting message polling loop..." << std::endl;

		// Exponential backoff for polling efficiency
		// Reduced MAX_SLEEP_MS for more responsive shutdown
		const int MIN_SLEEP_MS = 1;
		const int MAX_SLEEP_MS = 50;
		const int BACKOFF_MULTIPLIER = 2;
		int currentSleepMs = MIN_SLEEP_MS;
		int consecutiveEmptyPolls = 0;

		while (isRunning.load() && !ShutdownManager::IsShutdown()) {
			// Check shutdown condition frequently for responsive shutdown
			if (!isRunning.load() || ShutdownManager::IsShutdown()) {
				break;
			}

			std::string message = getDataChannelMessageString();

			if (!message.empty()) {
				// Reset backoff on successful message reception
				currentSleepMs = MIN_SLEEP_MS;
				consecutiveEmptyPolls = 0;

				try {
					std::cout << "[KeyInputHandler] Received message string: " << message << std::endl;

					json j = json::parse(message);
					if (j.is_object() && j.contains("code") && j.contains("type")) {
						std::string jsCode = j["code"].get<std::string>();
						std::string jsType = j["type"].get<std::string>();
						bool isClientKeyDown = (jsType == "keydown");
						std::cout << "[KeyInputHandler] Parsed - Code: " << jsCode << ", Type: " << jsType << std::endl;
						
						WORD vkCode = MapJavaScriptCodeToVK(jsCode);
						if (vkCode == 0) {
							continue;
						}

						bool simulateAction = false;
						bool actionIsDown = false;

						std::lock_guard<std::mutex> lock(clientKeysMutex);
						if (isClientKeyDown) {
							actionIsDown = true;
							if (clientReportedKeysDown.find(vkCode) == clientReportedKeysDown.end()) {
								clientReportedKeysDown.insert(vkCode);
								vkDownToJsCode[vkCode] = jsCode;
								simulateAction = true;
								std::cout << "[KeyInputHandler] State: New keydown for '" << jsCode << "' (VK:" << vkCode << "). Simulating press." << std::endl;
							}
							else {
								vkDownToJsCode[vkCode] = jsCode;
								simulateAction = true;
								std::cout << "[KeyInputHandler] State: Held keydown for '" << jsCode << "' (VK:" << vkCode << "). Re-simulating press." << std::endl;
							}
						}
						else {
							actionIsDown = false;
							if (clientReportedKeysDown.count(vkCode)) {
								clientReportedKeysDown.erase(vkCode);
								vkDownToJsCode.erase(vkCode);
								simulateAction = true;
								std::cout << "[KeyInputHandler] State: Keyup for '" << jsCode << "' (VK:" << vkCode << "). Simulating release." << std::endl;
							}
							else {
								std::cout << "[KeyInputHandler] State: Ignoring keyup for '" << jsCode << "' (VK:" << vkCode << ")." << std::endl;
							}
						}
						if (simulateAction) {
							SimulateWindowsKeyEvent(jsCode, actionIsDown);
						}
					}else {
						std::cerr << "[KeyInputHandler] Ignoring message: Invalid JSON format or missing 'code'/'type'. Message: " << message << std::endl;
					}
				}
				catch (const json::parse_error& e) {
					std::cerr << "[KeyInputHandler] JSON Parsing Error: " << e.what() << ". Message: " << message << std::endl;
				}
				catch (const std::exception& e) {
					std::cerr << "[KeyInputHandler] Generic Error Processing Message: " << e.what() << ". Message: " << message << std::endl;
				}
			}
			else {
				// Exponential backoff when no messages are available
				consecutiveEmptyPolls++;

				// Log backoff only occasionally to avoid spam
				if (consecutiveEmptyPolls % 100 == 0 && currentSleepMs > MIN_SLEEP_MS) {
					std::cout << "[KeyInputHandler] Polling backoff: " << consecutiveEmptyPolls
							  << " empty polls, sleeping " << currentSleepMs << "ms" << std::endl;
				}

				std::this_thread::sleep_for(std::chrono::milliseconds(currentSleepMs));

				// Increase sleep time exponentially, but cap at maximum
				if (currentSleepMs < MAX_SLEEP_MS) {
					int oldSleepMs = currentSleepMs;
					int newSleepMs = currentSleepMs * BACKOFF_MULTIPLIER;
					if (newSleepMs > MAX_SLEEP_MS) {
						newSleepMs = MAX_SLEEP_MS;
					}
					currentSleepMs = newSleepMs;

					// Yield to make shutdown more responsive when sleep time increases significantly
					if (currentSleepMs >= 10 && currentSleepMs != oldSleepMs) {
						std::this_thread::yield();
					}
				}
			}
		}
		std::cout << "[KeyInputHandler] Exiting message polling loop." << std::endl;
		
		std::cout << "[KeyInputHandler] Loop exited. Sending keyup for all tracked keys..." << std::endl;
		std::lock_guard<std::mutex> lock(clientKeysMutex);

		for (WORD vkCodeToRelease : clientReportedKeysDown) {
			std::string jsCodeToRelease = "";
			auto it = vkDownToJsCode.find(vkCodeToRelease);
			if (it != vkDownToJsCode.end()) {
				jsCodeToRelease = it->second;
			}
			if (!jsCodeToRelease.empty()) {
				std::cout << "[KeyInputHandler] Cleanup: Releasing '" << jsCodeToRelease << "' (VK:" << vkCodeToRelease << ")" << std::endl;
				SimulateWindowsKeyEvent(jsCodeToRelease, false);
			}
			else {
				std::cout << "[KeyInputHandler] Cleanup: No original jsCode for VK:" << vkCodeToRelease << ". Falling back to first mapping." << std::endl;
				// Fallback: pick first mapping as before
				for (const auto& pair : vkMap) {
					if (pair.second == vkCodeToRelease) {
						jsCodeToRelease = pair.first;
						break;
					}
				}
				if (!jsCodeToRelease.empty()) {
					SimulateWindowsKeyEvent(jsCodeToRelease, false);
				}
			}
		}
		clientReportedKeysDown.clear();
		vkDownToJsCode.clear();
		std::cout << "[KeyInputHandler] Cleanup keyup finished." << std::endl;
	}

	void initializeDataChannel() {
		if (!isRunning.load()) {
			isRunning.store(true);
			std::lock_guard<std::mutex> lock(clientKeysMutex);
			clientReportedKeysDown.clear();
			vkDownToJsCode.clear();

			messageThread = std::thread(messagePollingLoop);
			std::cout << "[KeyInputHandler] Polling started for data channel messages" << std::endl;
		}else {
			std::cout << "[KeyInputHandler] Polling thread already running." << std::endl;
		}
	}

	void cleanup() {
		if (isRunning.load()) {
			isRunning.store(false);
			if (messageThread.joinable()) {
				messageThread.join();
			}
			std::cout << "[KeyInputHandler] Polling stopped" << std::endl;
		}else {
			std::cout << "[KeyInputHandler] Cleanup called, but polling was not running." << std::endl;
		}
	}
}

extern "C" void initKeyInputHandler() {
	std::cout << "[KeyInputHandler] initKeyInputHandler called" << std::endl;
	KeyInputHandler::initializeDataChannel();
}

extern "C" void stopKeyInputHandler() {
	std::cout << "[KeyInputHandler] stopKeyInputHandler called (C export)" << std::endl;
	KeyInputHandler::cleanup();
}