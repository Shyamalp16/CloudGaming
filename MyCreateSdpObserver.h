#pragma once
#include "Websocket.h"
#include <string>
#include <rtc_base/ref_counted_object.h>
//#include <iostream>
//#include <api/jsep.h>
//#include <nlohmann/json.hpp>
//#include <api/peer_connection_interface.h>


extern webrtc::scoped_refptr<webrtc::PeerConnectionInterface> peerConnection;

class MyCreateSdpObserver : public webrtc::CreateSessionDescriptionObserver {
public:
	void OnSuccess(webrtc::SessionDescriptionInterface* desc) override;
	void OnFailure(webrtc::RTCError error) override;

protected:
	~MyCreateSdpObserver() override = default;
};

