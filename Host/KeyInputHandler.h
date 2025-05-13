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
#include <nlohmann/json.hpp>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.Capture.h>

#ifndef KEY_INPUT_HANDLER_H
#define KEY_INPUT_HANDLER_H

namespace KeyInputHandler {
    void initializeDataChannel();
    void simulateKeyPress(const std::string& key);
    void cleanup();
}

extern "C" void initKeyInputHandler();

#endif
