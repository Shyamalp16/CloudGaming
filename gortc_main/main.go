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
	peerConnection        *webrtc.PeerConnection
	pcMutex               sync.Mutex
	videoTrack            *webrtc.TrackLocalStaticRTP // For sending H.264 RTP packets
	lastAnswerSDP         string                      // Store answer SDP for C++ retrieval
	currentSequenceNumber uint16
	currentTimestamp      uint32
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

	// Convert C data to Go byte slice
	buf := C.GoBytes(data, size)

	// Convert PTS from microseconds to 90kHz timestamp
	// pts is in microseconds from C++, scale to 90kHz ticks
	if pts != 0 { // Only update timestamp on new frame
		currentTimestamp = uint32(pts / 1000 * 90) // Convert microseconds to 90kHz ticks
	}

	const maxPacketSize = 1200 // MTU size minus RTP overhead
	offset := 0
	for offset < len(buf) {
		chunkSize := len(buf) - offset
		if chunkSize > maxPacketSize {
			chunkSize = maxPacketSize
		}

		chunk := buf[offset : offset+chunkSize]
		isNewNALU := offset == 0 || (len(buf) > offset+3 && (buf[offset-3] == 0x00 && buf[offset-2] == 0x00 && buf[offset-1] == 0x01) ||
			(len(buf) > offset+4 && buf[offset-4] == 0x00 && buf[offset-3] == 0x00 && buf[offset-2] == 0x00 && buf[offset-1] == 0x01))

		rtpPacket := &rtp.Packet{
			Header: rtp.Header{
				Version:        2,
				PayloadType:    109, // Matches the negotiated H.264 payload type from SDP
				SequenceNumber: currentSequenceNumber,
				Timestamp:      currentTimestamp,
				SSRC:           3925656573, // Matches the SSRC from the answer SDP
				Marker:         isNewNALU,  // Set Marker bit for the start of a new NAL unit
			},
			Payload: chunk,
		}

		currentSequenceNumber++
		if err := videoTrack.WriteRTP(rtpPacket); err != nil {
			log.Printf("[Go/Pion] Error sending RTP packet: %v, offset: %d, chunkSize: %d\n", err, offset, chunkSize)
			return -1
		}

		log.Printf("[Go/Pion] Sent H.264 RTP packet (size: %d, offset: %d, PTS: %d, Seq: %d, Marker: %v)\n",
			chunkSize, offset, currentTimestamp, currentSequenceNumber-1, isNewNALU)

		offset += chunkSize
	}

	log.Printf("[Go/Pion] Sent H.264 frame (total size: %d, PTS: %d)\n", size, currentTimestamp)
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
