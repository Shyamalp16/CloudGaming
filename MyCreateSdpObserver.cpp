#include "MyCreateSdpObserver.h"

extern webrtc::scoped_refptr<webrtc::PeerConnectionInterface> peerConnection;

void MyCreateSdpObserver::OnSuccess(webrtc::SessionDescriptionInterface* desc) {
	if (!desc) {
		std::cerr << "[C++ Host] SDP creation failed." << std::endl;
		return;
	}
	peerConnection->SetLocalDescription(nullptr , desc);
	std::string sdpOut;
	desc->ToString(&sdpOut);

	json answerMsg;
	answerMsg["type"] = "answer";
	answerMsg["sdp"] = sdpOut;

	send_message(answerMsg);
}


void MyCreateSdpObserver::OnFailure(webrtc::RTCError error) {
	std::cerr << "[C++ Host] SDP creation failed: " << error.message() << std::endl;
}