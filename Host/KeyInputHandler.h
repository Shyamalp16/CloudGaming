#pragma once
#include "Websocket.h"
#include <iostream>
#include <string>
#include <vector>
#include <thread>
#include <chrono>
#include <stdexcept> 
#include <windows.h>
#include <map>
#include <set>
#include <mutex>
#include <nlohmann/json.hpp>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.Capture.h>

#ifndef KEY_INPUT_HANDLER_H
#define KEY_INPUT_HANDLER_H

namespace KeyInputHandler {
    void initializeDataChannel();
    void cleanup();
    void wakeKeyboardThreadInternal();
    void releaseAllKeysEmergency();
    void enqueueMessage(const std::string& message);

    // Internal functions
    void SimulateWindowsKeyEvent(const std::string& eventCode, bool isKeyDown);

    // Key mapping functions for testing and external access
    WORD MapJavaScriptCodeToVK(const std::string& jsCode);
    WORD MapJavaScriptCodeToScanCode(const std::string& jsCode);
    bool IsExtendedKey(WORD vkCode);
    bool IsExtendedKeyCanonical(WORD virtualKeyCode, const std::string& jsCode);
    UINT MapVKToScanCodeWithLayout(WORD vkCode, HKL hkl);



}



extern "C" void initKeyInputHandler();
extern "C" void stopKeyInputHandler();
extern "C" void emergencyReleaseAllKeys();

#endif
