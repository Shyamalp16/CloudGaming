#include "KeyInputHandler.h"
#include "ShutdownManager.h"
#include "pion_webrtc.h"
#include "Logging.h"
#include "Metrics.h"
#include <unordered_map>
#include <unordered_set>
#include <atomic>
#include <cstdlib>
#include <cassert>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <chrono>
#include "WindowUtils.h"
#include "InputSchema.h"
#include "InputInjection.h"
#include "InputStateMachine.h"
#include "InputStats.h"


using json = nlohmann::json;

// Initialize logging system
static inline void InitializeInputLogging() {
    // TODO: Implement proper logging initialization
    // For now, this is a stub
    LOG_INFO("Input logging initialized");
}

// Temporary simple logging to fix compilation
#define LOG_ERROR(msg) std::cout << "[ERROR] " << msg << std::endl
#define LOG_WARN(msg) std::cout << "[WARN] " << msg << std::endl
#define LOG_INFO(msg) std::cout << "[INFO] " << msg << std::endl
#define LOG_DEBUG(msg) std::cout << "[DEBUG] " << msg << std::endl
#define LOG_TRACE(msg) std::cout << "[TRACE] " << msg << std::endl

namespace KeyInputHandler {
	static std::atomic_bool isRunning;
	static std::thread messageThread;
	static std::set<WORD> clientReportedKeysDown;
	static std::mutex clientKeysMutex;
	static std::unordered_map<WORD, std::string> vkDownToJsCode;
	// Canonical identity map keyed by (scancode | 0x8000 if extended)
	static std::unordered_map<uint16_t, std::string> scanIdDownToJs;

	// Function to check if a key should be blocked (never injected)
	static bool shouldBlockKey(const std::string& jsCode) {
		// Block Windows keys to prevent accidental system menu activation
		static std::unordered_set<std::string> blockedKeys = {
			"MetaLeft", "MetaRight", "Meta",
			"OSLeft", "OSRight", "OS",
			"Super_L", "Super_R", "Super"
		};

		return blockedKeys.find(jsCode) != blockedKeys.end();
	}

	// Function to customize the list of blocked keys
	void setBlockedKeys(const std::unordered_set<std::string>& keys) {
		// This would need to be implemented if we want runtime configuration
		// For now, blocked keys are fixed at compile time
	}

	// FSM instance for state management and recovery
	static InputStateMachine::FSMConfig fsmConfig = []() {
		InputStateMachine::FSMConfig config;
		config.modifierKeyTimeout = std::chrono::milliseconds(5000); // 5 seconds for modifiers
		config.regularKeyTimeout = std::chrono::milliseconds(30000); // 30 seconds for regular keys
		config.enableRegularKeyTimeout = false; // Disable regular key timeout by default
		config.onlyRecoverModifiers = true; // Only recover modifier keys
		return config;
	}();
	static InputStateMachine::KeyStateFSM keyStateFSM{fsmConfig};

	// Blocking queue infrastructure
	static std::queue<std::string> keyboardMessageQueue;
	static std::mutex queueMutex;
	static std::condition_variable queueCondition;
	static bool shutdownRequested = false;

	// Canonical extended-key classifier (single source of truth)
	static inline bool IsExtendedKeyCanonical(WORD virtualKeyCode, const std::string& jsCode) {
		// Extended keys per Win32: arrows, Insert/Delete, Home/End, PgUp/PgDn, right Ctrl/Alt, Win keys, Apps
		static const WORD kExtendedVKs[] = {
			VK_UP, VK_DOWN, VK_LEFT, VK_RIGHT,
			VK_INSERT, VK_DELETE, VK_HOME, VK_END,
			VK_PRIOR, VK_NEXT,
			VK_RCONTROL, VK_RMENU,
			VK_LWIN, VK_RWIN, VK_APPS
		};
		static const size_t kExtendedVKsCount = sizeof(kExtendedVKs) / sizeof(kExtendedVKs[0]);

		if (virtualKeyCode == VK_RETURN && jsCode == "NumpadEnter") {
			return true; // numpad enter only
		}

		for (size_t i = 0; i < kExtendedVKsCount; ++i) {
			if (kExtendedVKs[i] == virtualKeyCode) {
				return true;
			}
		}
		return false;
	}

	// Debug-only verification of extended-bit consistency for critical keys
	static inline void DebugAssertExtendedConsistency(WORD virtualKeyCode,
		WORD scanCode,
		bool isExtendedKey,
		DWORD flags,
		const std::string& jsCode,
		bool usingScanCode)
	{
		(void)scanCode; (void)usingScanCode; (void)jsCode; (void)isExtendedKey;
#ifdef _DEBUG
		bool hasExtendedFlag = (flags & KEYEVENTF_EXTENDEDKEY) != 0;
		// Right Ctrl/Alt must always be extended
		if (virtualKeyCode == VK_RCONTROL || virtualKeyCode == VK_RMENU) {
			assert(isExtendedKey && hasExtendedFlag);
		}
		// Left Ctrl/Alt/Shift must not be extended
		if (virtualKeyCode == VK_LCONTROL || virtualKeyCode == VK_LMENU ||
			virtualKeyCode == VK_LSHIFT || virtualKeyCode == VK_RSHIFT) {
			assert(!hasExtendedFlag);
		}
		// Numpad Enter must be extended; normal Enter must not
		if (virtualKeyCode == VK_RETURN) {
			if (jsCode == "NumpadEnter") {
				assert(isExtendedKey && hasExtendedFlag);
			} else {
				assert(!hasExtendedFlag);
			}
		}
		// Ensure no E1-path keys are being synthesized (Pause/Break), which we don't support here
		// Scan code 0x45 with E1 sequence should not appear in our paths
		if (usingScanCode) {
			assert(!(scanCode == 0x45 && virtualKeyCode != VK_NUMLOCK));
		}
#endif
	}

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
		{"MetaLeft", 0x5B}, {"MetaRight", 0x5C}, {"ContextMenu", 0x5D},

		// Numpad digits and operators
		{"Numpad1", 0x4F}, {"Numpad2", 0x50}, {"Numpad3", 0x51},
		{"Numpad4", 0x4B}, {"Numpad5", 0x4C}, {"Numpad6", 0x4D},
		{"Numpad7", 0x47}, {"Numpad8", 0x48}, {"Numpad9", 0x49},
		{"Numpad0", 0x52}, {"NumpadDecimal", 0x53},
		{"NumpadAdd", 0x4E}, {"NumpadSubtract", 0x4A}, {"NumpadMultiply", 0x37},
		{"NumpadDivide", 0x35}
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

	// Blocking queue functions
	void enqueueKeyboardMessage(const std::string& message) {
		std::unique_lock<std::mutex> lock(queueMutex);
		keyboardMessageQueue.push(message);
		lock.unlock();
		queueCondition.notify_one();
	}

	void wakeKeyboardThreadInternal() {
		std::unique_lock<std::mutex> lock(queueMutex);
		shutdownRequested = true;
		lock.unlock();
		queueCondition.notify_one();
	}

	// Public function to enqueue messages from WebSocket handler
	void enqueueMessage(const std::string& message) {
		std::unique_lock<std::mutex> lock(queueMutex);
		keyboardMessageQueue.push(message);
		lock.unlock();
		queueCondition.notify_one();
	}

    // Observability
    static Stats g_stats;
    const Stats& getStats() { return g_stats; }

	void simulateKeyPress(const std::string& key) {
		LOG_DEBUG("Simulating key press for: '" + key + "'");

		// Validate the key code
		WORD virtualKeyCode = MapJavaScriptCodeToVK(key);
		if (virtualKeyCode == 0) {
			LOG_ERROR("Invalid key code '" + key + "'. Cannot simulate key press.");
			return;
		}

		// Simulate key down
		SimulateWindowsKeyEvent(key, true);

		// Small delay between down and up (optional, helps with some applications)
		std::this_thread::sleep_for(std::chrono::milliseconds(10));

		// Simulate key up
		SimulateWindowsKeyEvent(key, false);

		LOG_DEBUG("Key press simulation completed for: '" + key + "'");
	}

	void SimulateWindowsKeyEvent(const std::string& eventCode, bool isKeyDown){
		WORD virtualKeyCode = MapJavaScriptCodeToVK(eventCode);
		if (virtualKeyCode == 0) {
			LOG_WARN("No VK code mapping for JS code '" + eventCode + "'. Ignoring.");
			return;
		}

		INPUT input = { 0 };
		input.type = INPUT_KEYBOARD;
		input.ki.time = 0;
		input.ki.dwExtraInfo = 0;

		bool isExtendedKey = IsExtendedKeyCanonical(virtualKeyCode, eventCode);

		// Decide whether to use scancodes or virtual keys based on key type
		// Prioritize scancodes for layout-sensitive keys (punctuation, etc.)
		bool preferScanCode = true;

		// Use virtual keys only for keys that are universally consistent across layouts
		if (virtualKeyCode == VK_SPACE ||
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
				DebugAssertExtendedConsistency(virtualKeyCode, scanCode, isExtendedKey, input.ki.dwFlags, eventCode, true);
				std::cout << "[SimulateWindowsKeyEvent] Sending Input (Direct Scan Code) - JSCode: '" << eventCode
					<< "', VK_Mapped: " << virtualKeyCode << ", Scan: 0x" << std::hex << scanCode << std::dec
					<< ", Flags: 0x" << std::hex << input.ki.dwFlags << std::dec
					<< (isKeyDown ? " (DOWN)" : " (UP)") << std::endl;
			} else {
				// Fallback: try to convert VK to scancode with current foreground layout
				HKL hkl = GetKeyboardLayout(GetWindowThreadProcessId(GetForegroundWindow(), nullptr));
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
					DebugAssertExtendedConsistency(virtualKeyCode, scanCode, isExtendedKey, input.ki.dwFlags, eventCode, true);
					LOG_TRACE("Sending Input (VK->Scan Fallback) - JSCode: '" + eventCode + "', VK: " + std::to_string(virtualKeyCode) + ", Scan: " + std::to_string(scanCode) + ", Flags: " + std::to_string(input.ki.dwFlags) + " (" + (isKeyDown ? "DOWN" : "UP") + ")");
				} else {
					LOG_WARN("Could not map to scan code for JS code '" + eventCode + "'. Using VK.");
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
			LOG_TRACE("Sending Input (Virtual Key) - JSCode: '" + eventCode + "', VK: " + std::to_string(virtualKeyCode) + ", Flags: " + std::to_string(input.ki.dwFlags) + " (" + (isKeyDown ? "DOWN" : "UP") + ")");
		}

		// Check injection preconditions before sending input
		if (!InputInjection::shouldInjectInput(InputInjection::getDefaultPolicy(), "keyboard")) {
			return; // Skip injection based on policy
		}

		UINT sent = SendInput(1, &input, sizeof(INPUT));
		if (sent != 1) {
			DWORD errorCode = GetLastError();
			char errorMsg[256];
			FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
				NULL, errorCode, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
				errorMsg, sizeof(errorMsg), NULL);
			LOG_ERROR("SendInput failed for '" + eventCode + "' (" + (isKeyDown ? "DOWN" : "UP") + "). Error Code: " + std::to_string(errorCode) + ", Message: " + std::string(errorMsg));
		} else {
			LOG_TRACE("SendInput succeeded for '" + eventCode + "' (" + (isKeyDown ? "DOWN" : "UP") + ")");
			InputInjection::markInjectionSuccess();
		}
	}

	void messagePollingLoop() {
		LOG_INFO("Starting blocking queue message loop");

		while (isRunning.load() && !ShutdownManager::IsShutdown()) {
			std::string message;

			// Blocking wait for message or shutdown signal
			{
				std::unique_lock<std::mutex> lock(queueMutex);
				queueCondition.wait(lock, [&]() {
					return shutdownRequested || !keyboardMessageQueue.empty();
				});

				if (shutdownRequested || !isRunning.load() || ShutdownManager::IsShutdown()) {
					break;
				}

				if (!keyboardMessageQueue.empty()) {
					message = keyboardMessageQueue.front();
					keyboardMessageQueue.pop();
				}
			}

			if (!message.empty()) {
				try {
					LOG_DEBUG("Processing message from queue: " + message);

					json j = json::parse(message);
					if (j.is_object() && j.contains(InputSchema::kCode) && j.contains(InputSchema::kType)) {
						// Track keyboard event received
						InputStats::Track::keyboardEventReceived();

						std::string jsCode = j[InputSchema::kCode].get<std::string>();
						std::string jsType = j[InputSchema::kType].get<std::string>();
						bool isClientKeyDown = (jsType == "keydown");

						// Check if this key should be blocked (never injected)
						if (shouldBlockKey(jsCode)) {
							InputStats::Track::keyboardEventBlocked();
							std::cout << "[KeyInput] BLOCKED " << (isClientKeyDown ? "DOWN" : "UP  ")
								<< " code=" << jsCode << " (Windows key blocked)" << std::endl;
							continue; // Skip processing this key entirely
						}

						LOG_DEBUG("Parsed - Code: " + jsCode + ", Type: " + jsType);
						// Emit concise input log for each key event
						std::cout << "[KeyInput] " << (isClientKeyDown ? "DOWN" : "UP  ")
							<< " code=" << jsCode << std::endl;

						WORD vkCode = MapJavaScriptCodeToVK(jsCode);
						if (vkCode == 0) {
							continue;
						}

						// Collect action parameters inside minimal lock
						bool simulateAction = false;
						bool actionIsDown = false;
						std::string jsForInject;
						uint16_t scanIdentity = 0;

						{
							std::lock_guard<std::mutex> lock(clientKeysMutex);
							if (isClientKeyDown) {
								actionIsDown = true;
								if (clientReportedKeysDown.find(vkCode) == clientReportedKeysDown.end()) {
									clientReportedKeysDown.insert(vkCode);
									vkDownToJsCode[vkCode] = jsCode;
									// Track scancode+extended identity
									WORD sc = MapJavaScriptCodeToScanCode(jsCode);
									bool ext = IsExtendedKeyCanonical(vkCode, jsCode);
									scanIdentity = static_cast<uint16_t>((sc & 0x7FFF) | (ext ? 0x8000 : 0));
									scanIdDownToJs[scanIdentity] = jsCode;
									simulateAction = true;
									jsForInject = jsCode;
									LOG_DEBUG("State: New keydown for '" + jsCode + "' (VK:" + std::to_string(vkCode) + "). Simulating press.");
								} else {
									vkDownToJsCode[vkCode] = jsCode;
									simulateAction = true;
									jsForInject = jsCode;
									LOG_DEBUG("State: Held keydown for '" + jsCode + "' (VK:" + std::to_string(vkCode) + "). Re-simulating press.");
								}
							} else {
								actionIsDown = false;
								if (clientReportedKeysDown.count(vkCode)) {
									clientReportedKeysDown.erase(vkCode);
									vkDownToJsCode.erase(vkCode);
									// Remove scancode identity
									WORD sc = MapJavaScriptCodeToScanCode(jsCode);
									bool ext = IsExtendedKeyCanonical(vkCode, jsCode);
									scanIdentity = static_cast<uint16_t>((sc & 0x7FFF) | (ext ? 0x8000 : 0));
									scanIdDownToJs.erase(scanIdentity);
									simulateAction = true;
									jsForInject = jsCode;
									LOG_DEBUG("State: Keyup for '" + jsCode + "' (VK:" + std::to_string(vkCode) + "). Simulating release.");
								} else {
									LOG_DEBUG("State: Ignoring keyup for '" + jsCode + "' (VK:" + std::to_string(vkCode) + ").");
								}
							}
						}

						// Process through FSM for state management and recovery
						if (simulateAction && !jsForInject.empty()) {
							auto eventTime = std::chrono::steady_clock::now();
							auto transitionResult = keyStateFSM.processKeyEvent(jsForInject, actionIsDown, eventTime);

							switch (transitionResult) {
								case InputStateMachine::TransitionResult::ACCEPTED:
									// Check injection preconditions before injecting
									if (!InputInjection::shouldInjectInput(InputInjection::getDefaultPolicy(), "keyboard")) {
										// Skip injection based on policy (logged by shouldInjectInput)
										InputStats::Track::keyboardEventSkippedForeground();
										break;
									}

									// Inject the key event
									SimulateWindowsKeyEvent(jsForInject, actionIsDown);
									InputInjection::markInjectionSuccess();

									// Track successful injection
									InputStats::Track::keyboardEventInjected();

									// Update legacy metrics for compatibility
									if (!actionIsDown) {
										InputMetrics::inc(InputMetrics::injectedKeys());
									}
									break;

								case InputStateMachine::TransitionResult::IGNORED_INVALID:
									InputStats::Track::keyboardEventDroppedInvalid();
									std::cout << "[KeyInput] FSM ignored invalid transition for '" << jsForInject << "' (" << (actionIsDown ? "down" : "up") << ")" << std::endl;
									break;

								case InputStateMachine::TransitionResult::IGNORED_STALE:
									InputStats::Track::keyboardEventStaleIgnored();
									std::cout << "[KeyInput] FSM ignored stale event for '" << jsForInject << "' (" << (actionIsDown ? "down" : "up") << ")" << std::endl;
									break;

								case InputStateMachine::TransitionResult::RECOVERED:
									InputStats::Track::keyboardRecoverySuccess();
									std::cout << "[KeyInput] FSM recovered stuck key '" << jsForInject << "'" << std::endl;
									break;
							}
						}
					} else {
						LOG_WARN("Ignoring message: Invalid JSON format or missing 'code'/'type'. Message: " + message);
					}
				}
				catch (const json::parse_error& e) {
					LOG_ERROR("JSON Parsing Error: " + std::string(e.what()) + ". Message: " + message);
				}
				catch (const std::exception& e) {
					LOG_ERROR("Generic Error Processing Message: " + std::string(e.what()) + ". Message: " + message);
				}
			}
		}
		LOG_INFO("Exiting blocking queue message loop");

		LOG_INFO("Loop exited. Sending keyup for all tracked keys");	
		// Copy required data under lock
		std::vector<std::string> codesToRelease;
		{
			std::lock_guard<std::mutex> lock(clientKeysMutex);
			for (WORD vkCodeToRelease : clientReportedKeysDown) {
				std::string jsCodeToRelease;
				auto it = vkDownToJsCode.find(vkCodeToRelease);
				if (it != vkDownToJsCode.end()) {
					jsCodeToRelease = it->second;
				} else {
					for (const auto& pair : vkMap) {
						if (pair.second == vkCodeToRelease) { jsCodeToRelease = pair.first; break; }
					}
				}
				if (!jsCodeToRelease.empty()) codesToRelease.push_back(jsCodeToRelease);
			}
			clientReportedKeysDown.clear();
			vkDownToJsCode.clear();
		}
		// Inject outside lock
		for (const auto& jsCodeToRelease : codesToRelease) {
			LOG_INFO("Cleanup: Releasing '" + jsCodeToRelease + "'");
			SimulateWindowsKeyEvent(jsCodeToRelease, false);
		}
		LOG_INFO("Cleanup keyup finished");
	}

	void initializeDataChannel() {
		if (!isRunning.load()) {
			InitializeInputLogging();
			isRunning.store(true);
			std::lock_guard<std::mutex> lock(clientKeysMutex);
			clientReportedKeysDown.clear();
			vkDownToJsCode.clear();
			scanIdDownToJs.clear();

			// Reset blocking queue state
			{
				std::lock_guard<std::mutex> queueLock(queueMutex);
				shutdownRequested = false;
				while (!keyboardMessageQueue.empty()) {
					keyboardMessageQueue.pop();
				}
			}

			// Initialize FSM with injection callback
			keyStateFSM.initialize([](const std::string& jsCode, bool isDown) {
				SimulateWindowsKeyEvent(jsCode, isDown);
			});
			keyStateFSM.startWatchdog();

			messageThread = std::thread(messagePollingLoop);
			LOG_INFO("Blocking queue started for data channel messages");
		}else {
			LOG_INFO("Polling thread already running");
		}
	}

	void cleanup() {
		if (isRunning.load()) {
			isRunning.store(false);
			// Stop FSM watchdog
			keyStateFSM.stopWatchdog();
			// Wake the blocking thread for shutdown
			wakeKeyboardThreadInternal();
			if (messageThread.joinable()) {
				messageThread.join();
			}
			LOG_INFO("Blocking queue stopped");
		}else {
			LOG_INFO("Cleanup called, but polling was not running");
		}
	}

	void emergencyReleaseAllKeys() {
		std::cout << "[KeyInputHandler] Emergency release of all keys requested" << std::endl;
		keyStateFSM.releaseAllKeys();
		InputStats::Track::keyboardEmergencyRelease();
		InputMetrics::inc(InputMetrics::fsmEmergencyReleases());
	}

}

extern "C" void initKeyInputHandler() {
	KeyInputHandler::initializeDataChannel();
}

extern "C" void stopKeyInputHandler() {
	KeyInputHandler::cleanup();
}

extern "C" void emergencyReleaseAllKeys() {
	KeyInputHandler::emergencyReleaseAllKeys();
}

extern "C" const char* getInputStatsSummary() {
	static std::string statsString;
	statsString = InputStats::getStatsSummary();
	return statsString.c_str();
}

extern "C" void resetInputStats() {
	InputStats::resetAllStats();
}

extern "C" void wakeKeyboardThread() {
	KeyInputHandler::wakeKeyboardThreadInternal();
}