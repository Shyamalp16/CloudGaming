#pragma once
#define _WEBSOCKETPP_CPP11_STL_
#define BOOST_ALL_NO_LIB
#define NOMINMAX

//#if __cplusplus >= 202002L // C++20 or later
//namespace std {
//    template<typename F, typename... Args>
//    using result_of = invoke_result<F, Args...>;
//}
//#endif

#include <stdint.h>  // For uint64_t
#include <winsock2.h>
//#ifdef _MSC_VER
//#ifndef ntohll  // Only define if not already defined
//inline uint64_t ntohll(uint64_t x) {
//    return ((uint64_t)ntohl((uint32_t)(x & 0xFFFFFFFF)) << 32) | ntohl((uint32_t)(x >> 32));
//}
//#endif
//#ifndef htonll
//inline uint64_t htonll(uint64_t x) {
//    return ((uint64_t)htonl((uint32_t)(x & 0xFFFFFFFF)) << 32) | htonl((uint32_t)(x >> 32));
//}
//#endif
//#endif

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
