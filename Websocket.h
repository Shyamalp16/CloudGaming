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
#define NOMINMAX
#include <websocketpp/config/asio_client.hpp>
#include <websocketpp/client.hpp>

// Other headers
#include <nlohmann/json.hpp>
#include <iostream>
#include <functional>
#include <memory>

//webrtc headers
#include <api/scoped_refptr.h>
#include <api/peer_connection_interface.h>
#include <api/create_peerconnection_factory.h>
#include <api/jsep.h>
#include <api/data_channel_interface.h>
#include <api/audio_codecs/builtin_audio_decoder_factory.h>
#include <api/audio_codecs/builtin_audio_encoder_factory.h>
#include <api/video_codecs/builtin_video_decoder_factory.h>
#include <api/video_codecs/builtin_video_encoder_factory.h>
#include <rtc_base/thread.h>

void initWebsocket();
