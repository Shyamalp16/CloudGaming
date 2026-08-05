#include "MouseInputHandler.h"
#include "ShutdownManager.h"
#include "pion_webrtc.h"
#include <Windows.h>
#include <atomic>
#include <cstdlib>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <chrono>
#include <vector>
#include "WindowUtils.h"
#include "InputSchema.h"
#include "InputInjection.h"
#include "MouseCoordinateTransform.h"
#include "InputSequenceManager.h"
#include "InputConfig.h"
#include "ThreadPriorityManager.h"


using json = nlohmann::json;

namespace MouseInputHandler {

	constexpr int kClientViewWidth = 1920;
	constexpr int kClientViewHeight = 1080;
    // Simple configurable logging (0=ERROR,1=WARN,2=INFO,3=DEBUG)
    static std::atomic<int> gMouseLogLevel{1};
    static inline void SetMouseLogLevelFromEnv() {
        const char* lvl = std::getenv("INPUT_LOG_LEVEL");
        if (lvl) {
            int v = std::atoi(lvl);
            if (v < 0) v = 0; if (v > 3) v = 3;
            gMouseLogLevel.store(v);
        }
    }
    static inline bool MouseShouldLog(int level) { return level <= gMouseLogLevel.load(); }
    static inline void MouseLogDebug(const std::string& s) { if (MouseShouldLog(3)) std::cout << s << std::endl; }
    static inline void MouseLogWarn(const std::string& s) { if (MouseShouldLog(1)) std::cout << s << std::endl; }

    // Wheel accumulators
    static int gWheelAccumY = 0;
    static int gWheelAccumX = 0;
    static int gInvalidButtonWarns = 0;
	static std::atomic_bool isRunning;
	static std::thread mouseMessageThread;
	static std::set<int> clientReportedMouseButtonsDown;
	static std::mutex mouseStateMutex;
	// Store last known cursor position for cleanup mouseups
	static int lastKnownCursorX = 0;
	static int lastKnownCursorY = 0;

	// Blocking queue infrastructure
	static std::queue<std::string> mouseMessageQueue;
	static std::mutex queueMutex;
	static std::condition_variable queueCondition;
	static bool shutdownRequested = false;
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
		x = (x < virtualScreenX) ? virtualScreenX : ((x > virtualScreenX + virtualScreenWidth - 1) ? virtualScreenX + virtualScreenWidth - 1 : x);
		y = (y < virtualScreenY) ? virtualScreenY : ((y > virtualScreenY + virtualScreenHeight - 1) ? virtualScreenY + virtualScreenHeight - 1 : y);

		// Detect if clamping occurred
		wasClamped = (x != originalX) || (y != originalY);

		// Log significant clamping changes (more than 10 pixels to avoid spam)
		if (logSignificantChanges && wasClamped && ((abs(x - originalX) > 10) || (abs(y - originalY) > 10))) {
			// Log coordinate clamping only if per-event logging is enabled
			if (InputConfig::globalInputConfig.enablePerEventLogging) {
				std::cout << "[MouseInputHandler] Clamped coordinates: (" << originalX << ", " << originalY << ") -> (" << x << ", " << y << ")" << std::endl;
			}
		}
	}

	// Blocking queue functions
	void wakeMouseThreadInternal() {
		std::unique_lock<std::mutex> lock(queueMutex);
		shutdownRequested = true;
		lock.unlock();
		queueCondition.notify_one();
	}

	// Public function to enqueue messages from WebSocket handler
	void enqueueMessage(const std::string& message) {
		bool overflowed = false;
		{
			std::lock_guard<std::mutex> lock(queueMutex);
			const size_t maxPending = static_cast<size_t>((std::max)(16, InputConfig::globalInputConfig.maxPendingMessages));
			if (mouseMessageQueue.size() >= maxPending) {
				std::queue<std::string> empty;
				mouseMessageQueue.swap(empty);
				overflowed = true;
			}
			mouseMessageQueue.push(message);
		}

		if (overflowed) {
			std::vector<int> buttonsToRelease;
			int releaseX = 0;
			int releaseY = 0;
			{
				std::lock_guard<std::mutex> stateLock(mouseStateMutex);
				buttonsToRelease.assign(clientReportedMouseButtonsDown.begin(), clientReportedMouseButtonsDown.end());
				clientReportedMouseButtonsDown.clear();
				releaseX = lastKnownCursorX;
				releaseY = lastKnownCursorY;
			}
			for (int button : buttonsToRelease) {
				simulateWindowsMouseEvent("mouseup", releaseX, releaseY, button);
			}
			MouseLogWarn("[MouseInputHandler] Queue overflow: discarded stale events and released held buttons");
		}
		queueCondition.notify_one();
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

			// Use new DPI-aware coordinate transformation
			HWND targetWindow = WindowUtils::GetTargetWindow();
			auto transformResult = MouseCoordinateTransform::transformClientToAbsolute(
				x, y, targetWindow, kClientViewWidth, kClientViewHeight);

			if (transformResult.isValid) {
				input.mi.dx = transformResult.absoluteX;
				input.mi.dy = transformResult.absoluteY;
				input.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK;

				// Log transformation details only if per-event logging is enabled
				if (InputConfig::globalInputConfig.enablePerEventLogging) {
					std::cout << "[MouseInputHandler] Transformed Mouse Move: (" << x << ", " << y << ") -> "
							 << "Virtual Desktop (" << transformResult.virtualDesktopX << ", " << transformResult.virtualDesktopY << ") -> "
							 << "Absolute (" << input.mi.dx << ", " << input.mi.dy << ")"
							 << (transformResult.wasClipped ? " [CLIPPED]" : "") << std::endl;
				}
			} else {
				std::cerr << "[MouseInputHandler] Coordinate transformation failed: " << transformResult.errorMessage << std::endl;
				return;
			}
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
				if (++gInvalidButtonWarns <= 10) {
					std::cerr << "[MouseInputHandler] Invalid mouse button: " << button << ". Valid values are 0 (Left), 1 (Middle), 2 (Right), 3 (XButton1), 4 (XButton2). (" << gInvalidButtonWarns << "/10 warnings)" << std::endl;
				}
				return;
			}

			// Coordinate transformation is now handled in the individual branches above

			// Move and click are submitted in one ordered SendInput batch so the
			// button event always lands at the requested streamed coordinate.
				// First, move absolutely to the requested position
				INPUT moveInput = { 0 };
				moveInput.type = INPUT_MOUSE;

				// Use new DPI-aware coordinate transformation for move
				HWND targetWindow = WindowUtils::GetTargetWindow();
				auto moveTransformResult = MouseCoordinateTransform::transformClientToAbsolute(
					x, y, targetWindow, kClientViewWidth, kClientViewHeight);

				if (moveTransformResult.isValid) {
					moveInput.mi.dx = moveTransformResult.absoluteX;
					moveInput.mi.dy = moveTransformResult.absoluteY;
					moveInput.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK;
				} else {
					std::cerr << "[MouseInputHandler] Move coordinate transformation failed: " << moveTransformResult.errorMessage << std::endl;
					return;
				}

				// Then, button down/up as a separate event
				INPUT btnInput = { 0 };
				btnInput.type = INPUT_MOUSE;
				btnInput.mi.dwFlags = input.mi.dwFlags;
				btnInput.mi.mouseData = input.mi.mouseData;

				INPUT inputs[2] = { moveInput, btnInput };

				// Check injection preconditions before sending input
				if (!InputInjection::shouldInjectInput("mouse")) {
					return; // Skip injection based on policy
				}

				UINT sent = SendInput(2, inputs, sizeof(INPUT));
				if (sent != 2) {
					DWORD errorCode = GetLastError();
					std::cerr << "[MouseInputHandler] SendInput split click failed! Error Code: " << errorCode
						<< ", Error Message: " << std::system_category().message(errorCode) << std::endl;
				}
				return;
		}
		else if (eventType == "wheel") {
			// Handle vertical wheel events
			// x parameter contains deltaX (unused for vertical wheel)
			// y parameter contains deltaY (wheel rotation amount)
			// button parameter is unused for wheel events

			// Accumulate and normalize using configurable wheel scale (threshold),
			// but send multiples of WHEEL_DELTA to Windows
			const int wheelScale = InputConfig::getWheelScale();
			gWheelAccumY += y;
			int steps = gWheelAccumY / wheelScale;
			gWheelAccumY = gWheelAccumY % wheelScale;
			if (steps == 0) {
				return; // keep accumulating until we have a full step
			}

			input.mi.dwFlags = MOUSEEVENTF_WHEEL;
			input.mi.mouseData = steps * WHEEL_DELTA;

			// Log wheel simulation details only if per-event logging is enabled
			if (InputConfig::globalInputConfig.enablePerEventLogging) {
				std::cout << "[MouseInputHandler] Simulating Vertical Wheel - DeltaY: " << y << " -> mouseData: " << input.mi.mouseData << std::endl;
			}
		}
		else if (eventType == "hwheel") {
			// Handle horizontal wheel events
			// x parameter contains deltaX (wheel rotation amount)
			// y parameter contains deltaY (unused for horizontal wheel)
			// button parameter is unused for wheel events

			// Accumulate and normalize using configurable wheel scale (threshold),
			// but send multiples of WHEEL_DELTA to Windows
			const int wheelScaleX = InputConfig::getWheelScale();
			gWheelAccumX += x;
			int steps = gWheelAccumX / wheelScaleX;
			gWheelAccumX = gWheelAccumX % wheelScaleX;
			if (steps == 0) {
				return;
			}

			input.mi.dwFlags = MOUSEEVENTF_HWHEEL;
			input.mi.mouseData = steps * WHEEL_DELTA;

			// Log horizontal wheel simulation details only if per-event logging is enabled
			if (InputConfig::globalInputConfig.enablePerEventLogging) {
				std::cout << "[MouseInputHandler] Simulating Horizontal Wheel - DeltaX: " << x << " -> mouseData: " << input.mi.mouseData << std::endl;
			}
		}
		else {
			std::cerr << "[MouseInputHandler] Unknown event type for mouse simulation: " << eventType << std::endl;
			return;
		}

		// Check injection preconditions before sending input
		if (!InputInjection::shouldInjectInput("mouse")) {
			return; // Skip injection based on policy
		}

		UINT sent = SendInput(1, &input, sizeof(INPUT));
		if (sent != 1) {
			DWORD errorCode = GetLastError();
			std::cerr << "[MouseInputHandler] SendInput failed for mouse event '" << eventType << "'! Error Code: " << errorCode
				<< ", Error Message: " << std::system_category().message(errorCode) << std::endl;
		}
		else {
			// Log SendInput success only if per-event logging is enabled
			if (InputConfig::globalInputConfig.enablePerEventLogging) {
				std::cout << "[MouseInputHandler] SendInput succeeded for mouse event '" << eventType << "'" << std::endl;
			}
		}
	}

	void mouseMessagePollingLoop() {
		std::cout << "[MouseInputHandler] Starting blocking queue mouse message loop..." << std::endl;

		// Elevate thread priority for input injection
		ThreadPriorityManager::ScopedPriorityElevation priorityElevation;
		if (!priorityElevation.isElevated()) {
			std::cerr << "[MouseInputHandler] Failed to elevate thread priority for input injection" << std::endl;
		} else {
			std::cout << "[MouseInputHandler] Successfully elevated thread priority for low-latency input" << std::endl;
		}

		while (isRunning.load() && !ShutdownManager::IsShutdown()) {
			std::string message;

			// Blocking wait for message or shutdown signal
			{
				std::unique_lock<std::mutex> lock(queueMutex);
				queueCondition.wait(lock, [&]() {
					return shutdownRequested || !mouseMessageQueue.empty();
				});

				if (shutdownRequested || !isRunning.load() || ShutdownManager::IsShutdown()) {
					break;
				}

				if (!mouseMessageQueue.empty()) {
					message = mouseMessageQueue.front();
					mouseMessageQueue.pop();
				}
			}

			if (!message.empty()) {
				try {
					MouseLogDebug(std::string("[MouseInputHandler] Processing mouse message from queue: ") + message);
					json j = json::parse(message);
					if (j.is_object() && j.contains(InputSchema::kType)) {
						// Extract sequence ID if present
						uint64_t sequenceId = 0;
						if (j.contains("sequenceId")) {
							sequenceId = j["sequenceId"].get<uint64_t>();
						}

						// Process sequence ID through sequence manager (if enabled for mouse events)
						if (InputConfig::globalInputConfig.enableMouseSequencing && sequenceId > 0) {
							InputSequenceManager::globalSequenceManager.processSequence(sequenceId);
						}

						// Track mouse event received

						std::string jsType = j[InputSchema::kType].get<std::string>();
						int x = -1, y = -1, button = -1;

						if (jsType == InputSchema::kMouseMove) {
							if (j.contains(InputSchema::kX) && j.contains(InputSchema::kY)) {
								x = j[InputSchema::kX].get<int>();
								y = j[InputSchema::kY].get<int>();

								// Conditional logging - only log if detailed logging is enabled
								if (InputConfig::globalInputConfig.enablePerEventLogging) {
									std::cout << "[MouseInput] MOVE x=" << x << " y=" << y << std::endl;
								}
								// Update last known cursor position
								{
									std::lock_guard<std::mutex> lock(mouseStateMutex);
									lastKnownCursorX = x;
									lastKnownCursorY = y;
								}
								{
									HWND target = WindowUtils::GetTargetWindow();
									if (target && GetForegroundWindow() == target) {
										simulateWindowsMouseEvent(jsType, x, y, -1);
									} else {
										MouseLogDebug("[MouseInput] Skipping MOVE inject; target window not foreground");
									}
								}
							}
							else {
								std::cerr << "[MouseInputHandler] Missing 'x' or 'y' in mouse move message." << std::endl;
							}
						}
						else if (jsType == InputSchema::kMouseDown || jsType == InputSchema::kMouseUp) {
							if (j.contains(InputSchema::kX) && j.contains(InputSchema::kY) && j.contains(InputSchema::kButton)) {
								x = j[InputSchema::kX].get<int>();
								y = j[InputSchema::kY].get<int>();
								button = j[InputSchema::kButton].get<int>();
								// Log mouse button events only if per-event logging is enabled
								if (InputConfig::globalInputConfig.enablePerEventLogging) {
									std::cout << "[MouseInput] " << (jsType == "mousedown" ? "DOWN" : "UP  ")
											  << " btn=" << button << " x=" << x << " y=" << y << std::endl;
								}

								// Fast-path validation and action determination
								bool simulateAction = false;
								std::string actionType = jsType;
								int actionX = x, actionY = y, actionButton = button;

								// Pre-validate button number to avoid lock if invalid
								if ((jsType == "mousedown" || jsType == "mouseup") && (button < 0 || button > 4)) {
									if (InputConfig::globalInputConfig.enablePerEventLogging) {
										std::cerr << "[MouseInputHandler] Invalid button number: " << button << ". Ignoring event." << std::endl;
									}
									continue;
								}

								{
									std::lock_guard<std::mutex> lock(mouseStateMutex);
									// Update last known cursor position for button events too
									lastKnownCursorX = x;
									lastKnownCursorY = y;

									if (jsType == "mousedown") {
										if (clientReportedMouseButtonsDown.find(button) == clientReportedMouseButtonsDown.end()) {
											clientReportedMouseButtonsDown.insert(button);
											simulateAction = true;
										}
										else {
											// Only log if enabled and not too frequent to avoid spam
											static int duplicateDownCount = 0;
											if (InputConfig::globalInputConfig.enablePerEventLogging && ++duplicateDownCount % 100 == 0) {
												std::cout << "[MouseInputHandler] State: Mouse button " << button << " already down (" << duplicateDownCount << " duplicates)" << std::endl;
											}
										}
									}
									else { // mouseup
										if (clientReportedMouseButtonsDown.count(button)) {
											clientReportedMouseButtonsDown.erase(button);
											simulateAction = true;
										}
										else {
											// Only log if enabled and not too frequent to avoid spam
											static int duplicateUpCount = 0;
											if (InputConfig::globalInputConfig.enablePerEventLogging && ++duplicateUpCount % 100 == 0) {
												std::cout << "[MouseInputHandler] State: Mouse button " << button << " was not reported down (" << duplicateUpCount << " duplicates)" << std::endl;
											}
										}
									}
								}

								// SendInput outside the lock
								if (simulateAction) 								{
									HWND target = WindowUtils::GetTargetWindow();
									if (target && GetForegroundWindow() == target) {
										simulateWindowsMouseEvent(actionType, actionX, actionY, actionButton);
									} else {
										MouseLogDebug("[MouseInput] Skipping click inject; target window not foreground");
									}
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
		}
		std::cout << "[MouseInputHandler] Exiting blocking queue mouse message loop." << std::endl;

		//cleanup
		std::cout << "[MouseInputHandler] Loop exited. Sending mouseup for all tracked buttons..." << std::endl;

		// Collect buttons to release and last known position inside minimal lock
		std::vector<int> buttonsToRelease;
		int cleanupX, cleanupY;
		{
			std::lock_guard<std::mutex> lock(mouseStateMutex);
			// Copy the set to a vector to avoid invalidating iterator while iterating
			buttonsToRelease.assign(clientReportedMouseButtonsDown.begin(), clientReportedMouseButtonsDown.end());
			cleanupX = lastKnownCursorX;
			cleanupY = lastKnownCursorY;
			clientReportedMouseButtonsDown.clear();
		}

		// Send cleanup mouseups outside the lock to avoid holding mutex during SendInput
		for (int buttonToRelease : buttonsToRelease) {
			std::cout << "[MouseInputHandler] Sending cleanup mouseup for button: " << buttonToRelease
					  << " at last known position (" << cleanupX << ", " << cleanupY << ")" << std::endl;
			simulateWindowsMouseEvent("mouseup", cleanupX, cleanupY, buttonToRelease);
		}
		std::cout << "[MouseInputHandler] Cleanup mouseup finished for " << buttonsToRelease.size() << " buttons." << std::endl;
	}

	void initializeMouseChannel() {
		SetMouseLogLevelFromEnv();

		// Initialize thread priority manager for low-latency input
		ThreadPriorityManager::initializeGlobalConfig();

		std::cout << "[MouseInputHandler] DEBUG: initializeMouseChannel called." << std::endl;
		// DPI awareness note: SendInput absolute uses virtual desktop 0..65535. This is DPI-agnostic
		// when the process is DPI-aware. Log system DPI for diagnostics.
		UINT systemDpi = 96;
		HMODULE hUser32 = LoadLibraryA("User32.dll");
		if (hUser32) {
			auto pGetDpiForSystem = (UINT(WINAPI*)())GetProcAddress(hUser32, "GetDpiForSystem");
			if (pGetDpiForSystem) { systemDpi = pGetDpiForSystem(); }
			FreeLibrary(hUser32);
		}
		std::cout << "[MouseInputHandler] System DPI (diagnostic): " << systemDpi << std::endl;
		if (!isRunning.load()) {
			isRunning.store(true);
			std::lock_guard<std::mutex> lock(mouseStateMutex);
			clientReportedMouseButtonsDown.clear();
			// Initialize last known cursor position to screen center as reasonable default
			lastKnownCursorX = GetSystemMetrics(SM_CXSCREEN) / 2;
			lastKnownCursorY = GetSystemMetrics(SM_CYSCREEN) / 2;

			// Reset blocking queue state
			{
				std::lock_guard<std::mutex> queueLock(queueMutex);
				shutdownRequested = false;
				while (!mouseMessageQueue.empty()) {
					mouseMessageQueue.pop();
				}
			}

			mouseMessageThread = std::thread(mouseMessagePollingLoop);
			std::cout << "[MouseInputHandler] Blocking queue started for mouse channel messages" << std::endl;
		}
		else {
			std::cout << "[MouseInputHandler] Mouse polling thread already running." << std::endl;
		}
	}

	void cleanup() {
		if (isRunning.load()) {
			isRunning.store(false);
			// Wake the blocking thread for shutdown
			wakeMouseThreadInternal();
			if (mouseMessageThread.joinable()) {
				mouseMessageThread.join();
			}
			std::cout << "[MouseInputHandler] Blocking queue stopped" << std::endl;
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
