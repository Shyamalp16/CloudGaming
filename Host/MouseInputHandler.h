#pragma once
#include <iostream>
#include <string>
#include <thread>
#include <mutex>
#include <stdexcept> 
#include <set>
#include <nlohmann/json.hpp>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.Capture.h>

#ifndef MOUSE_INPUT_HANDLER_H
#define MOUSE_INPUT_HANDLER_H

namespace MouseInputHandler {
    void initializeMouseChannel();
    void cleanup();
    void enqueueMessage(const std::string& message);
    void releaseAllButtonsEmergency();

}

extern "C" void initMouseInputHandler();
extern "C" void stopMouseInputHandler();

#endif
