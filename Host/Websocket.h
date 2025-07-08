#pragma once
#define _WEBSOCKETPP_CPP11_STL_
#define BOOST_ALL_NO_LIB
#define NOMINMAX

#include <stdint.h>  // For uint64_t
#include <winsock2.h>
#include <objbase.h>
#include "pion_webrtc.h"
#include "Encoder.h"


#include <boost/asio.hpp>
#include <websocketpp/config/asio_client.hpp>
#include <websocketpp/client.hpp>

// Other headers
#include <nlohmann/json.hpp>
#include <iostream>
#include <functional>
#include <memory>

using json = nlohmann::json;
void send_message(const json& message);
void initWebsocket(const std::string& roomId);
void stopWebsocket();
