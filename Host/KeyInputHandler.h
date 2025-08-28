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
    void simulateKeyPress(const std::string& key);
    void cleanup();
    void emergencyReleaseAllKeys(); // For disconnect/reconnect scenarios

    // Internal functions
    void SimulateWindowsKeyEvent(const std::string& eventCode, bool isKeyDown);

    // Counters for observability (reads are racy but acceptable for telemetry)
    struct Stats { unsigned dropped{0}; unsigned injected{0}; unsigned skipped{0}; unsigned resets{0}; };
    const Stats& getStats();
}

extern "C" void initKeyInputHandler();
extern "C" void emergencyReleaseAllKeys();

// Input statistics API
extern "C" const char* getInputStatsSummary();
extern "C" void resetInputStats();

#endif
