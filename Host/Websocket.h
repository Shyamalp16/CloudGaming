#pragma once
#define _WEBSOCKETPP_CPP11_STL_
#define BOOST_ALL_NO_LIB
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <stdint.h>  // For uint64_t
#include <winsock2.h>
#include <objbase.h>
#include "pion_webrtc.h"
#include "Encoder.h"


#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>
#include <websocketpp/config/asio_client.hpp>
#include <websocketpp/client.hpp>

// Other headers
#include <nlohmann/json.hpp>
#include <iostream>
#include <functional>
#include <memory>
#include <cstddef>
class SessionManager;
class StreamProfileManager;

using json = nlohmann::json;
void send_message(const json& message);
void initWebsocket(const std::string& roomId, const std::string& hostId, const std::string& signalingUrl,
                   const std::string& hostSecret, const std::string& networkMode,
                   SessionManager* sessionManager, StreamProfileManager* streamProfileManager);
void stopWebsocket();

namespace WebsocketPolicy {
int ComputeReconnectDelayMs(unsigned attempt, double jitterFraction);
}
