#include "MouseInputHandler.h"
#include "ShutdownManager.h"
#include "pion_webrtc.h"
#include <Windows.h>
#include <atomic>
#include <algorithm>
#include <cstdlib>

using json = nlohmann::json;

namespace MouseInputHandler {
	static std::atomic_bool isRunning;
	static std::thread mouseMessageThread;
	static std::set<int> clientReportedMouseButtonsDown;
	static std::mutex mouseStateMutex;

	void mouseMessagePollingLoop();
	void simulateWindowsMouseEvent(const std::string& eventType, int x, int y, int button);
	void simulateMouseMove(int x, int y);
	void simulateMouseButton(const std::string& type, int button);

	// Helper function to clamp coordinates to virtual desktop bounds
	void clampToVirtualDesktop(int& x, int& y, bool& wasClamped, bool logSignificantChanges = true) {
		int virtualScreenX = GetSystemMetrics(SM_XVIRTUALSCREEN);
		int virtualScreenY = GetSystemMetrics(SM_YVIRTUALSCREEN);
		int virtualScreenWidth = GetSystemMetrics(SM_CXVIRTUALSCREEN);
		int virtualScreenHeight = GetSystemMetrics(SM_CYVIRTUALSCREEN);

		// Safety check: ensure valid dimensions
		if (virtualScreenWidth <= 0 || virtualScreenHeight <= 0) {
			std::cerr << "[MouseInputHandler] Invalid virtual screen dimensions: " << virtualScreenWidth << "x" << virtualScreenHeight << ". Falling back to primary screen." << std::endl;
			virtualScreenX = 0;
			virtualScreenY = 0;
			virtualScreenWidth = GetSystemMetrics(SM_CXSCREEN);
			virtualScreenHeight = GetSystemMetrics(SM_CYSCREEN);
		}

		// Store original values for clamping detection
		int originalX = x;
		int originalY = y;

		// Clamp coordinates to virtual desktop bounds
		x = max(virtualScreenX, min(x, virtualScreenX + virtualScreenWidth - 1));
		y = max(virtualScreenY, min(y, virtualScreenY + virtualScreenHeight - 1));

		// Detect if clamping occurred
		wasClamped = (x != originalX) || (y != originalY);

		// Log significant clamping changes (more than 10 pixels to avoid spam)
		if (logSignificantChanges && wasClamped && ((abs(x - originalX) > 10) || (abs(y - originalY) > 10))) {
			std::cout << "[MouseInputHandler] Clamped coordinates: (" << originalX << ", " << originalY << ") -> (" << x << ", " << y << ")" << std::endl;
		}
	}

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
			// Clamp coordinates to virtual desktop bounds to prevent cursor jumping to unexpected positions
			bool wasClamped = false;
			clampToVirtualDesktop(x, y, wasClamped, true);

			// Get virtual desktop metrics for coordinate normalization
			int virtualScreenX = GetSystemMetrics(SM_XVIRTUALSCREEN);
			int virtualScreenY = GetSystemMetrics(SM_YVIRTUALSCREEN);
			int virtualScreenWidth = GetSystemMetrics(SM_CXVIRTUALSCREEN);
			int virtualScreenHeight = GetSystemMetrics(SM_CYVIRTUALSCREEN);

			// Safety fallback (shouldn't happen due to clampToVirtualDesktop, but just in case)
			if (virtualScreenWidth <= 0 || virtualScreenHeight <= 0) {
				virtualScreenX = 0;
				virtualScreenY = 0;
				virtualScreenWidth = GetSystemMetrics(SM_CXSCREEN);
				virtualScreenHeight = GetSystemMetrics(SM_CYSCREEN);
			}

			// Apply virtual desktop offset and normalize coordinates
			input.mi.dx = (LONG)(((double)(x - virtualScreenX) / virtualScreenWidth) * 65535.0);
			input.mi.dy = (LONG)(((double)(y - virtualScreenY) / virtualScreenHeight) * 65535.0);
			input.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK;

			std::cout << "[MouseInputHandler] Simulating Mouse Move to: (" << x << ", " << y << ") -> Virtual Desktop Offset (" << virtualScreenX << ", " << virtualScreenY << ") -> Normalized (" << input.mi.dx << ", " << input.mi.dy << ")" << std::endl;
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
			case 3: //XButton1 (Back)
				input.mi.dwFlags = (eventType == "mousedown") ? MOUSEEVENTF_XDOWN : MOUSEEVENTF_XUP;
				input.mi.mouseData = XBUTTON1;
				break;
			case 4: //XButton2 (Forward)
				input.mi.dwFlags = (eventType == "mousedown") ? MOUSEEVENTF_XDOWN : MOUSEEVENTF_XUP;
				input.mi.mouseData = XBUTTON2;
				break;
			default:
				std::cerr << "[MouseInputHandler] Invalid mouse button: " << button << ". Valid values are 0 (Left), 1 (Middle), 2 (Right), 3 (XButton1), 4 (XButton2)." << std::endl;
				return;
			}

			// Include cursor movement with the click for accurate positioning
			// Clamp coordinates to virtual desktop bounds to prevent cursor jumping to unexpected positions
			bool wasClamped = false;
			clampToVirtualDesktop(x, y, wasClamped, false); // Use false to avoid duplicate logging

			// Get virtual desktop metrics for coordinate normalization
			int virtualScreenX = GetSystemMetrics(SM_XVIRTUALSCREEN);
			int virtualScreenY = GetSystemMetrics(SM_YVIRTUALSCREEN);
			int virtualScreenWidth = GetSystemMetrics(SM_CXVIRTUALSCREEN);
			int virtualScreenHeight = GetSystemMetrics(SM_CYVIRTUALSCREEN);

			// Safety fallback (shouldn't happen due to clampToVirtualDesktop, but just in case)
			if (virtualScreenWidth <= 0 || virtualScreenHeight <= 0) {
				virtualScreenX = 0;
				virtualScreenY = 0;
				virtualScreenWidth = GetSystemMetrics(SM_CXSCREEN);
				virtualScreenHeight = GetSystemMetrics(SM_CYSCREEN);
			}

			// Apply virtual desktop offset and normalize coordinates
			input.mi.dx = (LONG)(((double)(x - virtualScreenX) / virtualScreenWidth) * 65535.0);
			input.mi.dy = (LONG)(((double)(y - virtualScreenY) / virtualScreenHeight) * 65535.0);
			input.mi.dwFlags |= MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK;

			std::cout << "[MouseInputHandler] Simulating Mouse Button " << button << " " << eventType << " at (" << x << ", " << y << ") -> Virtual Desktop Offset (" << virtualScreenX << ", " << virtualScreenY << ") -> Normalized (" << input.mi.dx << ", " << input.mi.dy << ")" << std::endl;
		}
		else if (eventType == "wheel") {
			// Handle vertical wheel events
			// x parameter contains deltaX (unused for vertical wheel)
			// y parameter contains deltaY (wheel rotation amount)
			// button parameter is unused for wheel events

			// Normalize deltaY to wheel increments (WHEEL_DELTA = 120)
			LONG wheelDelta = (LONG)y;

			input.mi.dwFlags = MOUSEEVENTF_WHEEL;
			input.mi.mouseData = wheelDelta;

			std::cout << "[MouseInputHandler] Simulating Vertical Wheel - DeltaY: " << y << " -> mouseData: " << input.mi.mouseData << std::endl;
		}
		else if (eventType == "hwheel") {
			// Handle horizontal wheel events
			// x parameter contains deltaX (wheel rotation amount)
			// y parameter contains deltaY (unused for horizontal wheel)
			// button parameter is unused for wheel events

			// Normalize deltaX to wheel increments (WHEEL_DELTA = 120)
			LONG wheelDelta = (LONG)x;

			input.mi.dwFlags = MOUSEEVENTF_HWHEEL;
			input.mi.mouseData = wheelDelta;

			std::cout << "[MouseInputHandler] Simulating Horizontal Wheel - DeltaX: " << x << " -> mouseData: " << input.mi.mouseData << std::endl;
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
		while (isRunning.load() && !ShutdownManager::IsShutdown()) {
			std::string message = getMouseChannelMessageString();
			if (!message.empty()) {
				try {
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
									// Validate button number
									if (button < 0 || button > 4) {
										std::cerr << "[MouseInputHandler] Invalid button number: " << button << ". Ignoring mousedown." << std::endl;
										continue;
									}
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
						else if (jsType == "wheel" || jsType == "hwheel") {
							// Handle mouse wheel events
							if (j.contains("deltaY") || j.contains("deltaX")) {
								int deltaX = 0;
								int deltaY = 0;

								if (j.contains("deltaX")) {
									deltaX = j["deltaX"].get<int>();
								}
								if (j.contains("deltaY")) {
									deltaY = j["deltaY"].get<int>();
								}

								std::cout << "[MouseInputHandler] Parsed - Type: " << jsType << ", DeltaX: " << deltaX << ", DeltaY: " << deltaY << std::endl;

								// For wheel events, we pass deltaX/deltaY as x/y parameters
								// and use button parameter to distinguish wheel type (0=vertical, 1=horizontal)
								int wheelType = (jsType == "hwheel") ? 1 : 0;
								simulateWindowsMouseEvent(jsType, deltaX, deltaY, wheelType);
							}
							else {
								std::cerr << "[MouseInputHandler] Malformed wheel message (missing deltaX/deltaY): " << message << std::endl;
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
				}
				catch (const std::exception& e) {
					std::cerr << "[MouseInputHandler] Generic Error Processing Message: " << e.what() << ". Message: " << message << std::endl;
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

		// Copy the set to a vector to avoid invalidating iterator while iterating
		std::vector<int> buttonsToRelease(clientReportedMouseButtonsDown.begin(), clientReportedMouseButtonsDown.end());

		for (int buttonToRelease : buttonsToRelease) {
			std::cout << "[MouseInputHandler] Sending cleanup mouseup for button: " << buttonToRelease << std::endl;
			// You might need to retrieve the last known position to send with the cleanup mouseup.
			// For simplicity here, assuming (0,0) or no specific position is strictly needed for cleanup up.
			// If the client's `mouseup` sends `x`/`y`, the local `SimulateWindowsMouseEvent` will handle it.
			simulateWindowsMouseEvent("mouseup", -1, -1, buttonToRelease); // Pass -1 for x,y as it's a cleanup, not a physical move.
		}
		clientReportedMouseButtonsDown.clear();
		std::cout << "[MouseInputHandler] Cleanup mouseup finished for " << buttonsToRelease.size() << " buttons." << std::endl;
	}

	void initializeMouseChannel() {
		std::cout << "[MouseInputHandler] DEBUG: initializeMouseChannel called." << std::endl; // ADD THIS
		if (!isRunning.load()) {
			isRunning.store(true);
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
		if (isRunning.load()) {
			isRunning.store(false);
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