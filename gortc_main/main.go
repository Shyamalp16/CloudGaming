package main

/*
#cgo CFLAGS: -I.
#include <stdlib.h>
#include <string.h>
*/
import "C"
import (
	"log"
	"math/rand"
	"sync"
	"time"
	"unsafe"

	"github.com/pion/rtp"
	"github.com/pion/webrtc/v3"
)

// Global variables
var (
	peerConnection        *webrtc.PeerConnection
	pcMutex               sync.Mutex
	videoTrack            *webrtc.TrackLocalStaticRTP // For sending H.264 RTP packets
	trackSSRC             uint32
	lastAnswerSDP         string // Store answer SDP for C++ retrieval
	currentSequenceNumber uint16
	currentTimestamp      uint32
)

func init() {
	rand.Seed(time.Now().UnixNano())
}

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

	trackSSRC = rand.Uint32()
	log.Print("[Go/Pion] Generated SSRC for video track: %d\n", trackSSRC)

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
	log.Printf("[Go/Pion] Raw H.264 buffer (first 20 bytes): %x\n", buf[:min(20, len(buf))])

	// Convert PTS from microseconds to 90kHz timestamp
	currentTimestamp = uint32(pts * 90 / 1000)

	const maxPacketSize = 1200

	// Parse NAL units from the buffer
	nalUnits := splitNALUnits(buf)
	if len(nalUnits) == 0 {
		log.Println("[Go/Pion] sendVideoPacket: No NAL units found in buffer")
		return -1
	}

	// Process each NAL unit
	for i, nal := range nalUnits {
		// Log the first few bytes of the NAL unit for debugging
		if i == 0 {
			log.Printf("[Go/Pion] First 4 bytes of frame: %x\n", nal[:min(4, len(nal))])
		}

		// If the NAL unit is smaller than the MTU, send it as a single RTP packet
		if len(nal) <= maxPacketSize {
			rtpPacket := &rtp.Packet{
				Header: rtp.Header{
					Version:        2,
					PayloadType:    96,
					SequenceNumber: currentSequenceNumber,
					Timestamp:      currentTimestamp,
					SSRC:           trackSSRC,
					Marker:         i == len(nalUnits)-1,
				},
				Payload: nal,
			}

			currentSequenceNumber++
			if err := videoTrack.WriteRTP(rtpPacket); err != nil {
				log.Printf("[Go/Pion] Error sending RTP packet: %v\n", err)
				return -1
			}

			log.Printf("[Go/Pion] Sent H.264 RTP packet (size: %d, PTS: %d, Seq: %d, Marker: %v)\n",
				len(nal), currentTimestamp, currentSequenceNumber-1, rtpPacket.Header.Marker)
		} else {
			// Fragment the NAL unit into FU-A packets
			err := sendFragmentedNALUnit(nal, maxPacketSize, i == len(nalUnits)-1)
			if err != nil {
				log.Printf("[Go/Pion] Error sending fragmented NAL unit: %v\n", err)
				return -1
			}
		}
	}

	log.Printf("[Go/Pion] Sent H.264 frame (total size: %d, PTS: %d)\n", size, currentTimestamp)
	return 0
}

// splitNALUnits splits the H.264 bitstream into individual NAL units
// splitNALUnits splits the H.264 bitstream into individual NAL units
func splitNALUnits(buf []byte) [][]byte {
	var nalUnits [][]byte
	start := 0
	i := 0

	for i < len(buf) {
		// Look for the start code (0x00000001 or 0x000001)
		if i+3 < len(buf) && buf[i] == 0 && buf[i+1] == 0 && buf[i+2] == 0 && buf[i+3] == 1 {
			if i > start {
				// Add the NAL unit (excluding the start code)
				nalUnits = append(nalUnits, buf[start:i])
			}
			start = i + 4 // Skip the 4-byte start code
			i += 4
		} else if i+2 < len(buf) && buf[i] == 0 && buf[i+1] == 0 && buf[i+2] == 1 {
			if i > start {
				// Add the NAL unit (excluding the start code)
				nalUnits = append(nalUnits, buf[start:i])
			}
			start = i + 3 // Skip the 3-byte start code
			i += 3
		} else {
			i++
		}
	}

	// Add the last NAL unit
	if start < len(buf) {
		nalUnits = append(nalUnits, buf[start:])
	}

	// Filter out any empty or invalid NAL units
	var filteredNALs [][]byte
	for _, nal := range nalUnits {
		if len(nal) > 0 {
			// Log the NAL unit type for debugging
			nalType := nal[0] & 0x1F
			log.Printf("[Go/Pion] Parsed NAL Unit Type: %d, Size: %d\n", nalType, len(nal))
			filteredNALs = append(filteredNALs, nal)
		}
	}

	return filteredNALs
}

// sendFragmentedNALUnit sends a large NAL unit as FU-A packets
func sendFragmentedNALUnit(nal []byte, maxPacketSize int, isLastNAL bool) error {
	// FU-A header: [FU indicator (1 byte)] [FU header (1 byte)] [NAL fragment]
	// FU indicator: (NAL type = 28 for FU-A)
	// FU header: [S(1 bit)][E(1 bit)][R(1 bit)][Type(5 bits)]
	// S = 1 for start, E = 1 for end, R = 0 (reserved), Type = NAL unit type

	nalType := nal[0] & 0x1F            // Extract the NAL unit type (5 bits)
	fuIndicator := (nal[0] & 0xE0) | 28 // FU-A type (28)
	maxPayloadSize := maxPacketSize - 2 // Account for FU indicator and FU header

	offset := 0
	firstFragment := true

	for offset < len(nal) {
		chunkSize := len(nal) - offset
		if chunkSize > maxPayloadSize {
			chunkSize = maxPayloadSize
		}

		//FU Header
		fuHeader := byte(0)
		if firstFragment {
			fuHeader |= 0x80 //set start bit
		}

		if offset+chunkSize >= len(nal) {
			fuHeader |= 0x40 //set end bit
		}
		fuHeader |= nalType //set nal type

		//create the FU-A payload
		payload := make([]byte, 2+chunkSize)
		payload[0] = fuIndicator
		payload[1] = fuHeader
		copy(payload[2:], nal[offset:offset+chunkSize])
		log.Printf("[Go/Pion] NAL Unit Type: %d\n", nalType)
		//SendRTP packet
		rtpPacket := &rtp.Packet{
			Header: rtp.Header{
				Version:        2,
				PayloadType:    96,
				SequenceNumber: currentSequenceNumber,
				Timestamp:      currentTimestamp,
				SSRC:           trackSSRC,
				Marker:         (offset+chunkSize >= len(nal)) && isLastNAL, // Marker on last packet of last NAL
			},
			Payload: payload,
		}

		currentSequenceNumber++
		if err := videoTrack.WriteRTP(rtpPacket); err != nil {
			return err
		}

		log.Printf("[Go/Pion] Sent H.264 FU-A packet (size: %d, offset: %d, PTS: %d, Seq: %d, Marker: %v)\n",
			len(payload), offset, currentTimestamp, currentSequenceNumber-1, rtpPacket.Header.Marker)

		offset += chunkSize
		firstFragment = false
	}

	return nil
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
