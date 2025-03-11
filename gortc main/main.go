package main

/*
#cgo CFLAGS: -I.
#include <stdlib.h>
#include <string.h>
*/
import "C"
import (
	"fmt"
	"log"
	"sync"
	"time"
	"unsafe"

	"github.com/pion/webrtc/v3"
	"github.com/pion/webrtc/v3/pkg/media"
)

// Global variables to mimic C++ state
var (
	peerConnection *webrtc.PeerConnection
	pcMutex        sync.Mutex
	videoTrack     *webrtc.TrackLocalStaticSample // For sending frames
	lastAnswerSDP  string
)

//export createPeerConnectionGo
func createPeerConnectionGo() C.int {
	pcMutex.Lock()
	defer pcMutex.Unlock()

	// Close existing PeerConnection if any
	if peerConnection != nil {
		_ = peerConnection.Close()
		peerConnection = nil
	}

	// Configure the API with a MediaEngine (replaces PeerConnectionFactory)
	mediaEngine := &webrtc.MediaEngine{}
	if err := mediaEngine.RegisterDefaultCodecs(); err != nil {
		log.Printf("[Go/Pion] Error registering codecs: %v\n", err)
		return 0
	}

	// Create a SettingEngine (optional, for advanced configuration)
	settingEngine := webrtc.SettingEngine{}

	// Create the API
	api := webrtc.NewAPI(webrtc.WithMediaEngine(mediaEngine), webrtc.WithSettingEngine(settingEngine))

	// Configure STUN server and SDP semantics (matches C++ config)
	config := webrtc.Configuration{
		ICEServers: []webrtc.ICEServer{
			{URLs: []string{"stun:stun.l.google.com:19302"}},
		},
		SDPSemantics: webrtc.SDPSemanticsUnifiedPlan,
	}

	var err error
	peerConnection, err = api.NewPeerConnection(config)
	if err != nil {
		log.Printf("[Go/Pion] Error creating PeerConnection: %v\n", err)
		return 0
	}

	// Register callbacks to mimic CustomPeerConnectionObserver
	peerConnection.OnICECandidate(func(candidate *webrtc.ICECandidate) {
		if candidate != nil {
			log.Printf("[Go/Pion] OnICECandidate: %s\n", candidate.ToJSON().Candidate)
			// In C++, this was sent via WebSocket; you’ll need to pass it back to C++ (e.g., via a callback function)
		}
	})
	peerConnection.OnICEConnectionStateChange(func(state webrtc.ICEConnectionState) {
		log.Printf("[Go/Pion] ICE Connection State changed to: %s\n", state.String())
	})
	peerConnection.OnConnectionStateChange(func(state webrtc.PeerConnectionState) {
		log.Printf("[Go/Pion] PeerConnection State changed to: %s\n", state.String())
	})

	log.Println("[Go/Pion] PeerConnection created.")
	return 1
}

//export handleOffer
func handleOffer(offerSDP *C.char) {
	pcMutex.Lock()
	defer pcMutex.Unlock()

	if peerConnection == nil {
		log.Println("[Go/Pion] handleOffer: no PeerConnection, creating one.")
		if createPeerConnectionGo() == 0 {
			return
		}
	}

	sdpGoString := C.GoString(offerSDP)
	log.Printf("[Go/Pion] handleOffer: %s\n", sdpGoString)

	offer := webrtc.SessionDescription{
		Type: webrtc.SDPTypeOffer,
		SDP:  sdpGoString,
	}
	if err := peerConnection.SetRemoteDescription(offer); err != nil {
		log.Printf("[Go/Pion] Error setting remote offer: %v\n", err)
		return
	}

	log.Println("[Go/Pion] Remote Offer Set, creating Answer...")
	answer, err := peerConnection.CreateAnswer(nil)
	if err != nil {
		log.Printf("[Go/Pion] CreateAnswer error: %v\n", err)
		return
	}

	// Wait for ICE gathering to complete
	gatherComplete := webrtc.GatheringCompletePromise(peerConnection)
	if err := peerConnection.SetLocalDescription(answer); err != nil {
		log.Printf("[Go/Pion] SetLocalDescription error: %v\n", err)
		return
	}
	<-gatherComplete

	localDesc := peerConnection.LocalDescription()
	if localDesc == nil {
		log.Printf("[Go/Pion] localDesc is nil after gathering.")
		return
	}
	log.Printf("[Go/Pion] Answer created: %s\n", localDesc.SDP)
	// In C++, this was sent via sendAnswer; you’ll need to pass it back to C++ (e.g., via GetLocalDescription)
}

//export handleRemoteIceCandidate
func handleRemoteIceCandidate(candidateStr *C.char) {
	pcMutex.Lock()
	defer pcMutex.Unlock()

	if peerConnection == nil {
		log.Println("[Go/Pion] handleRemoteIceCandidate: no PeerConnection yet!")
		return
	}

	cGoStr := C.GoString(candidateStr)
	log.Printf("[Go/Pion] handleRemoteIceCandidate: %s\n", cGoStr)

	candidate := webrtc.ICECandidateInit{Candidate: cGoStr}
	if err := peerConnection.AddICECandidate(candidate); err != nil {
		log.Printf("[Go/Pion] Error adding ICE candidate: %v\n", err)
	} else {
		log.Println("[Go/Pion] ICE Candidate added successfully.")
	}
}

//export sendAnswerGo
func sendAnswerGo() {
	pcMutex.Lock()
	defer pcMutex.Unlock()

	if peerConnection == nil {
		log.Println("[Go/Pion] sendAnswer: no PeerConnection yet!")
		return
	}

	localDesc := peerConnection.LocalDescription()
	if localDesc == nil {
		log.Println("[Go/Pion] sendAnswer: no local description yet!")
		return
	}

	// // Convert to C string for C++ to use (mimics C++ send_message)
	// cStr := C.CString(localDesc.SDP)
	// // In C++, this would be sent via WebSocket; you’ll need to pass cStr to C++ for sending
	// // For now, log it (C++ will handle the actual sending)
	// log.Printf("[Go/Pion] sendAnswer: %s\n", localDesc.SDP)
	// C.free(unsafe.Pointer(cStr)) // Free the C string to avoid memory leaks

	lastAnswerSDP = localDesc.SDP
	log.Printf("[Go/Pion] sendAnswer: %s\n", lastAnswerSDP)
}

//export getAnswerSDP
func getAnswerSDP() *C.char {
	pcMutex.Lock()
	defer pcMutex.Unlock()

	if lastAnswerSDP == "" {
		log.Println("[Go/Pion] getAnswerSDP: no SDP available!")
		return nil
	}

	// Convert to C string and return
	return C.CString(lastAnswerSDP)
}

//export sendFrame
func sendFrame(frameData *C.char, frameLen C.int) {
	pcMutex.Lock()
	defer pcMutex.Unlock()

	if peerConnection == nil {
		log.Println("[Go/Pion] sendFrame: no PeerConnection yet!")
		return
	}

	// Convert C frame data to Go bytes
	data := C.GoBytes(unsafe.Pointer(frameData), frameLen)
	log.Printf("[Go/Pion] sendFrame: received frame of length %d\n", len(data))

	// Initialize video track if not already done
	if videoTrack == nil {
		var err error
		videoTrack, err = webrtc.NewTrackLocalStaticSample(webrtc.RTPCodecCapability{MimeType: webrtc.MimeTypeVP8}, "video", "pion")
		if err != nil {
			log.Printf("[Go/Pion] Error creating video track: %v\n", err)
			return
		}
		if _, err := peerConnection.AddTrack(videoTrack); err != nil {
			log.Printf("[Go/Pion] Error adding video track: %v\n", err)
			return
		}
	}

	// Send the frame
	sample := media.Sample{Data: data, Duration: 33 * time.Millisecond}
	if err := videoTrack.WriteSample(sample); err != nil {
		log.Printf("[Go/Pion] Error sending frame: %v\n", err)
	} else {
		log.Println("[Go/Pion] Frame sent successfully")
	}
}

//export getIceConnectionState
func getIceConnectionState() C.int {
	pcMutex.Lock()
	defer pcMutex.Unlock()

	if peerConnection == nil {
		return C.int(-1)
	}
	return C.int(peerConnection.ICEConnectionState())
}

// main is required for -buildmode=c-shared
func main() {
	fmt.Println("[Go/Pion] main() in DLL. Doing nothing.")
}
