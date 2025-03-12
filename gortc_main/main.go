package main

/*
#cgo CFLAGS: -I.
#include <stdlib.h>
#include <string.h>
*/
import "C"
import (
	"log"
	"sync"
	"unsafe"

	"github.com/pion/rtp"
	"github.com/pion/webrtc/v3"
)

// Global variables
var (
	peerConnection *webrtc.PeerConnection
	pcMutex        sync.Mutex
	videoTrack     *webrtc.TrackLocalStaticRTP // For sending H.264 RTP packets
	lastAnswerSDP  string                      // Store answer SDP for C++ retrieval
)

//export createPeerConnectionGo
func createPeerConnectionGo() C.int {
	pcMutex.Lock()
	defer pcMutex.Unlock()

	// Close existing PeerConnection if any
	if peerConnection != nil {
		_ = peerConnection.Close()
		peerConnection = nil
		videoTrack = nil
	}

	// Configure MediaEngine with H.264 codec
	mediaEngine := &webrtc.MediaEngine{}
	codec := webrtc.RTPCodecParameters{
		RTPCodecCapability: webrtc.RTPCodecCapability{
			MimeType:    "video/h264",
			ClockRate:   90000,
			SDPFmtpLine: "level-asymmetry-allowed=1;packetization-mode=1;profile-level-id=42e01f",
		},
		PayloadType: 96, // Default payload type for H.264
	}
	if err := mediaEngine.RegisterCodec(codec, webrtc.RTPCodecTypeVideo); err != nil {
		log.Printf("[Go/Pion] Error registering H.264 codec: %v\n", err)
		return 0
	}

	// Create API
	api := webrtc.NewAPI(webrtc.WithMediaEngine(mediaEngine))

	// Configure STUN server
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

	// Create H.264 video track
	videoTrack, err = webrtc.NewTrackLocalStaticRTP(
		webrtc.RTPCodecCapability{
			MimeType:    "video/h264",
			ClockRate:   90000,
			SDPFmtpLine: "level-asymmetry-allowed=1;packetization-mode=1;profile-level-id=42e01f",
		},
		"video",
		"game-stream",
	)
	if err != nil {
		log.Printf("[Go/Pion] Error creating video track: %v\n", err)
		return 0
	}
	if _, err := peerConnection.AddTrack(videoTrack); err != nil {
		log.Printf("[Go/Pion] Error adding video track: %v\n", err)
		return 0
	}

	// Set up callbacks
	peerConnection.OnICECandidate(func(candidate *webrtc.ICECandidate) {
		if candidate != nil {
			log.Printf("[Go/Pion] OnICECandidate: %s\n", candidate.ToJSON().Candidate)
		}
	})
	peerConnection.OnICEConnectionStateChange(func(state webrtc.ICEConnectionState) {
		log.Printf("[Go/Pion] ICE Connection State: %s\n", state.String())
	})
	peerConnection.OnConnectionStateChange(func(state webrtc.PeerConnectionState) {
		log.Printf("[Go/Pion] PeerConnection State: %s\n", state.String())
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

	answer, err := peerConnection.CreateAnswer(nil)
	if err != nil {
		log.Printf("[Go/Pion] Error creating answer: %v\n", err)
		return
	}
	gatherComplete := webrtc.GatheringCompletePromise(peerConnection)
	if err := peerConnection.SetLocalDescription(answer); err != nil {
		log.Printf("[Go/Pion] Error setting local description: %v\n", err)
		return
	}
	<-gatherComplete

	lastAnswerSDP = peerConnection.LocalDescription().SDP
	log.Printf("[Go/Pion] Answer created: %s\n", lastAnswerSDP)
}

//export getAnswerSDP
func getAnswerSDP() *C.char {
	pcMutex.Lock()
	defer pcMutex.Unlock()

	if lastAnswerSDP == "" {
		log.Println("[Go/Pion] getAnswerSDP: no SDP available!")
		return nil
	}
	return C.CString(lastAnswerSDP)
}

//export handleRemoteIceCandidate
func handleRemoteIceCandidate(candidateStr *C.char) {
	pcMutex.Lock()
	defer pcMutex.Unlock()

	if peerConnection == nil {
		log.Println("[Go/Pion] handleRemoteIceCandidate: no PeerConnection!")
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

//export sendVideoPacket
func sendVideoPacket(data unsafe.Pointer, size C.int, pts C.longlong) C.int {
	pcMutex.Lock()
	defer pcMutex.Unlock()

	if peerConnection == nil || videoTrack == nil {
		log.Println("[Go/Pion] sendVideoPacket: PeerConnection or videoTrack not initialized!")
		return -1
	}

	buf := C.GoBytes(data, size)
	timestamp := uint32(pts * 90000 / 1000000)

	// Manually packetize the H.264 data into RTP packets
	// Since we can't rely on a specific H264Payloader, we'll use a simple packetization
	// This is a basic approach; for production, you might want to use pion/webrtc's internal payloader
	const maxPacketSize = 1200 // MTU size
	offset := 0
	for offset < len(buf) {
		chunkSize := len(buf) - offset
		if chunkSize > maxPacketSize {
			chunkSize = maxPacketSize
		}

		// Create an RTP packet
		rtpPacket := &rtp.Packet{
			Header: rtp.Header{
				Version:        2,
				PayloadType:    96,                             // Matches the payload type in RegisterCodec
				SequenceNumber: uint16(offset / maxPacketSize), // Increment per packet
				Timestamp:      timestamp,
				SSRC:           12345, // Arbitrary SSRC
			},
			Payload: buf[offset : offset+chunkSize],
		}

		// Write the RTP packet to the track
		if err := videoTrack.WriteRTP(rtpPacket); err != nil {
			log.Printf("[Go/Pion] Error sending RTP packet: %v\n", err)
			return -1
		}

		offset += chunkSize
	}

	log.Printf("[Go/Pion] Sent H.264 packet (size: %d, PTS: %d)\n", size, pts)
	return 0
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

//export closePeerConnection
func closePeerConnection() {
	pcMutex.Lock()
	defer pcMutex.Unlock()

	if peerConnection != nil {
		_ = peerConnection.Close()
		peerConnection = nil
		videoTrack = nil
		lastAnswerSDP = ""
		log.Println("[Go/Pion] PeerConnection closed.")
	}
}

func main() {
	log.Println("[Go/Pion] main() in DLL. Doing nothing.")
}
