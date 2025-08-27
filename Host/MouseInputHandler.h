#pragma once
//#include "Websocket.h"
#include <iostream>
#include <string>
#include <thread>
#include <mutex>
//#include <vector>
//#include <chrono>
#include <stdexcept> 
//#include <windows.h>
#include <set>
#include <nlohmann/json.hpp>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.Capture.h>

#ifndef MOUSE_INPUT_HANDLER_H
#define MOUSE_INPUT_HANDLER_H

namespace MouseInputHandler {
    void initializeMouseChannel();
    void cleanup();
}

extern "C" void initMouseInputHandler();
extern "C" void stopMouseInputHandler();

#endif
