#pragma once

// Remove ASIO_STANDALONE if it was previously set
// #define ASIO_STANDALONE  // <-- Make sure this is gone or commented out.

// Optional: This tells WebSocket++ to use C++11 STL features
#define _WEBSOCKETPP_CPP11_STL_

// Bring in Boost.Asio
#define BOOST_ALL_NO_LIB
#include <boost/asio.hpp>

// Bring in WebSocket++ Boost-based config
//   - asio_client.hpp is the client variant 
//   - asio.hpp is typically the server variant
//#include <websocketpp/config/asio.hpp>
#include <websocketpp/config/asio_client.hpp>
#include <websocketpp/client.hpp>

// Other headers
#include <nlohmann/json.hpp>
#include <iostream>
#include <functional>

void initWebsocket();
