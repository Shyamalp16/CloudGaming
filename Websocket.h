#pragma once
#define _WEBSOCKETPP_CPP11_STL_
#define BOOST_ALL_NO_LIB
#define NOMINMAX

#include <boost/asio.hpp>
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
#include "rtc_base/ssl_adapter.h"

//#include "MySetRemoteObserver.h"
//#include "MyCreateSdpObserver.h"

using json = nlohmann::json;
void send_message(const json& message);
void initWebsocket();
