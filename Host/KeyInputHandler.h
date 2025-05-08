#pragma once
#include "Websocket.h"
#include <windows.h>
#include <nlohmann/json.hpp>
#include <iostream>
#include <thread>
#include <chrono>

#ifndef KEY_INPUT_HANDLER_H
#define KEY_INPUT_HANDLER_H

namespace KeyInputHandler {
    void initializeDataChannel();
    void simulateKeyPress(const std::string& key);
    void cleanup();
}

extern "C" void initKeyInputHandler();

#endif
