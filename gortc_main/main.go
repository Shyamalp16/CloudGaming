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
	dataChannel           *webrtc.DataChannel
	messageQueue          []string
	queueMutex            sync.Mutex
)

func init() {
	rand.Seed(time.Now().UnixNano())
}

func enqueueMessage(msg string) {
	queueMutex.Lock()
	defer queueMutex.Unlock()
	log.Printf("[Go/Pion] --> Enqueueing message: '%s'. Queue size BEFORE: %d\n", msg, len(messageQueue))
	messageQueue = append(messageQueue, msg)
	log.Printf("[Go/Pion] --> Enqueued message: '%s'. Queue size AFTER: %d\n", msg, len(messageQueue))
}

//export getDataChannelMessage
func getDataChannelMessage() *C.char {
	queueMutex.Lock()
	defer queueMutex.Unlock()
	if len(messageQueue) == 0 {
		// log.Println("[Go/Pion] No messages in queue")
		return nil
	}
	msg := messageQueue[0]
	messageQueue = messageQueue[1:]
	log.Printf("[Go/Pion] <-- getDataChannelMessage: Dequeued: '%s'. Queue size AFTER: %d\n", msg, len(messageQueue))
	return C.CString(msg)
}

func newTrue() *bool {
	b := true
	return &b
}

func newFalse() *bool {
	b := false
	return &b
}

func newProtocol() *string {
	b := ""
	return &b
}

//export createPeerConnectionGo
func createPeerConnectionGo() C.int {
	pcMutex.Lock()
	// defer pcMutex.Unlock()

	// Close existing PeerConnection if any
	if peerConnection != nil {
		_ = peerConnection.Close()
		peerConnection = nil
		videoTrack = nil
		if dataChannel != nil {
			if err := dataChannel.Close(); err != nil {
				log.Printf("[Go/Pion] Error closing existing DataChannel: %v\n", err)
			}
		}
		dataChannel = nil
		messageQueue = []string{}
		lastAnswerSDP = ""
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

	if err := mediaEngine.RegisterDefaultCodecs(); err != nil {
		log.Printf("[Go/Pion] Error registering default codecs: %v\n", err)
		return 0
	}
	log.Println("[Go/Pion] createPeerConnectionGo: MediaEngine configured.")

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
		peerConnection = nil
		pcMutex.Unlock()
		return 0
	}
	log.Println("[Go/Pion] createPeerConnectionGo: New PeerConnection instance created successfully.")

	peerConnection.OnDataChannel(func(dc *webrtc.DataChannel) {
		// This callback (OnDataChannel) is invoked by Pion when SetRemoteDescription
		// processes an offer containing an m=application line for a data channel.
		actualLabel := dc.Label()
		var idStr string
		if dc.ID() != nil {
			idStr = fmt.Sprintf("%d", *dc.ID())
		} else {
			idStr = "nil" // Should not happen for non-negotiated (in-band) channels
		}

		log.Printf("[Go/Pion] OnDataChannel FIRED! Actual Label RECEIVED: '%s', ID: %s, Negotiated: %v, ReadyState: %s\n",
			actualLabel, idStr, dc.Negotiated(), dc.ReadyState().String())

		if actualLabel == "keyPressChannel" {
			log.Printf("[Go/Pion] OnDataChannel: Label MATCHED ('%s'). Assigning to global dataChannel and attaching handlers.\n", actualLabel)

			// pcMutex is already held by the calling function (createPeerConnectionGo or handleOffer)
			// when OnDataChannel is being set up.
			// However, when this *callback* fires later, pcMutex might not be held.
			// So, for modifying the global 'dataChannel', we need to lock here.
			pcMutex.Lock()
			// If there was an old global dataChannel, and it's different from the new one, close the old one.
			if dataChannel != nil && dataChannel != dc {
				log.Printf("[Go/Pion] OnDataChannel: Closing previous global data channel '%s' before assigning new one.\n", dataChannel.Label())
				if errClose := dataChannel.Close(); errClose != nil {
					log.Printf("[Go/Pion] OnDataChannel: Error closing previous global dataChannel: %v\n", errClose)
				}
			}
			dataChannel = dc // Assign the new dc (from the callback parameter) to the global variable
			log.Printf("[Go/Pion] OnDataChannel: Global 'dataChannel' variable assigned to new DC with label '%s'.", dataChannel.Label())
			pcMutex.Unlock()

			// Attach handlers to the 'dc' instance received in this OnDataChannel callback parameter
			dc.OnOpen(func() {
				// For logging, let's check the state of the global dataChannel too
				pcMutex.Lock()
				gdcLabel := "nil (global)"
				gdcID := "nil"
				if dataChannel != nil {
					gdcLabel = dataChannel.Label()
					if dataChannel.ID() != nil {
						gdcID = fmt.Sprintf("%d", *dataChannel.ID())
					}
				}
				pcMutex.Unlock()
				log.Printf("[Go/Pion] Data channel '%s' (local, ID: %s) OnOpen event. Current Global DC: '%s' (ID: %s). Local DC ReadyState: %s\n",
					dc.Label(), idStr, gdcLabel, gdcID, dc.ReadyState().String())
			})

			dc.OnMessage(func(msg webrtc.DataChannelMessage) {
				log.Printf("[Go/Pion] >>> DataChannel '%s' (ID: %s) OnMessage RECEIVED: %s\n", dc.Label(), idStr, string(msg.Data))
				enqueueMessage(string(msg.Data)) // This function handles its own locking for messageQueue
			})

			dc.OnClose(func() {
				log.Printf("[Go/Pion] Data channel '%s' (ID: %s) OnClose event. ReadyState: %s\n", dc.Label(), idStr, dc.ReadyState().String())
				pcMutex.Lock()
				if dataChannel == dc { // If the closed channel is the one currently assigned to global
					log.Printf("[Go/Pion] OnClose: Global dataChannel ('%s', ID: %s) is being closed. Setting global to nil.\n", dc.Label(), idStr)
					dataChannel = nil
				} else if dataChannel != nil {
					// This case means 'dc' closed, but our global 'dataChannel' was pointing to something else (or was recently reassigned)
					log.Printf("[Go/Pion] OnClose: A dataChannel ('%s', ID: %s) closed, but it wasn't the current global DC ('%s'). Global DC remains.\n",
						dc.Label(), idStr, dataChannel.Label())
				} else {
					// 'dc' closed, and global 'dataChannel' was already nil.
					log.Printf("[Go/Pion] OnClose: A dataChannel ('%s', ID: %s) closed, and global DC was already nil.\n", dc.Label(), idStr)
				}
				pcMutex.Unlock()
			})

			dc.OnError(func(err error) {
				log.Printf("[Go/Pion] Data channel '%s' (ID: %s) OnError event: %v\n", dc.Label(), idStr, err)
			})
			log.Printf("[Go/Pion] OnDataChannel: All handlers (OnOpen, OnMessage, OnClose, OnError) attached for DC '%s'.\n", actualLabel)

		} else {
			log.Printf("[Go/Pion] OnDataChannel: Label MISMATCH. Expected 'keyPressChannel' but received '%s' (ID: %s). Handlers NOT attached for this DC.\n",
				actualLabel, idStr)
		}
	})
	log.Println("[Go/Pion] createPeerConnectionGo: OnDataChannel handler has been set up on the PeerConnection.")

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
		if pcErr := peerConnection.Close(); pcErr != nil {
			log.Printf("[Go/Pion] createPeerConnectionGo: Error closing PeerConnection after track failure: %v\n", pcErr)
		}
		peerConnection = nil
		pcMutex.Unlock()
		return 0
	}

	trackSSRC = rand.Uint32()
	// log.Print("[Go/Pion] Generated SSRC for video track: %d\n", trackSSRC)

	if _, err := peerConnection.AddTrack(videoTrack); err != nil {
		log.Printf("[Go/Pion] Error adding video track: %v\n", err)
		if pcErr := peerConnection.Close(); pcErr != nil {
			log.Printf("[Go/Pion] createPeerConnectionGo: Error closing PeerConnection after AddTrack failure: %v\n", pcErr)
		}
		peerConnection = nil // Nullify
		pcMutex.Unlock()     // Unlock before returning on error
		return 0
	}

	// peerConnection.OnDataChannel(func(dc *webrtc.DataChannel) {
	// 	// THIS LOG IS CRITICAL
	// 	log.Printf("[Go/Pion] Received data channel: '%s', ID: %d, Negotiated: %v, ReadyState: %s\n",
	// 		dc.Label(), *dc.ID(), dc.Negotiated(), dc.ReadyState().String())

	// 	if dc.Label() == "keyPressChannel" {
	// 		pcMutex.Lock() // Ensure thread safety if 'dataChannel' is global
	// 		// If there was an old one, you might want to close it first
	// 		if dataChannel != nil && dataChannel != dc {
	// 			log.Printf("[Go/Pion] Closing previous data channel '%s'\n", dataChannel.Label())
	// 			_ = dataChannel.Close()
	// 		}
	// 		dataChannel = dc // Assign to the global variable
	// 		pcMutex.Unlock()

	// 		// THIS LOG IS CRITICAL
	// 		log.Printf("[Go/Pion] Assigned data channel '%s' from client. Attaching handlers.\n", dc.Label())

	// 		dc.OnOpen(func() {
	// 			// THIS LOG IS CRITICAL
	// 			log.Printf("[Go/Pion] Data channel '%s' OnOpen event. ReadyState: %s\n", dc.Label(), dc.ReadyState().String())
	// 		})

	// 		dc.OnMessage(func(msg webrtc.DataChannelMessage) {
	// 			// THIS LOG IS CRITICAL
	// 			log.Printf("[Go/Pion] >>> DataChannel '%s' OnMessage RECEIVED: %s\n", dc.Label(), string(msg.Data))
	// 			enqueueMessage(string(msg.Data))
	// 		})

	// 		dc.OnClose(func() {
	// 			log.Printf("[Go/Pion] Data channel '%s' OnClose event. ReadyState: %s\n", dc.Label(), dc.ReadyState().String())
	// 			pcMutex.Lock()
	// 			if dataChannel == dc { // Clear global if this is the one
	// 				dataChannel = nil
	// 			}
	// 			pcMutex.Unlock()
	// 		})

	// 		dc.OnError(func(err error) {
	// 			log.Printf("[Go/Pion] Data channel '%s' OnError event: %v\n", dc.Label(), err)
	// 		})
	// 	} else {
	// 		log.Printf("[Go/Pion] Received unexpected data channel: %s\n", dc.Label())
	// 	}
	// })
	// // log.Println("[Go/Pion] Data channel created for keypresses.")

	// Set up callbacks
	peerConnection.OnICECandidate(func(candidate *webrtc.ICECandidate) {
		if candidate != nil {
			log.Printf("[Go/Pion] OnICECandidate: %s\n", candidate.ToJSON().Candidate)
			// Signaling of this candidate to the remote peer (JS client) is handled by C++ via WebSocket
		} else {
			log.Println("[Go/Pion] OnICECandidate: ICE Candidate gathering complete (nil candidate received).")
		}
	})
	peerConnection.OnICEConnectionStateChange(func(state webrtc.ICEConnectionState) {
		log.Printf("[Go/Pion] ICE Connection State: %s\n", state.String())
	})
	peerConnection.OnConnectionStateChange(func(state webrtc.PeerConnectionState) {
		log.Printf("[Go/Pion] PeerConnection State: %s\n", state.String())
	})

	log.Println("[Go/Pion] PeerConnection created.")
	pcMutex.Unlock()
	return 1
}

//export handleOffer
func handleOffer(offerSDP *C.char) {
	pcMutex.Lock()

	if peerConnection == nil {
		log.Println("[Go/Pion] handleOffer: no PeerConnection, creating one.")
		pcMutex.Unlock()
		if createPeerConnectionGo() == 0 {
			log.Println("[Go/Pion] handleOffer: Failed to create PeerConnection. Aborting offer handling.")
			return
		}
		pcMutex.Lock()
		if peerConnection == nil {
			log.Println("[Go/Pion] handleOffer: PeerConnection is STILL nil after creation attempt. Aborting.")
			pcMutex.Unlock()
			return
		}
		log.Println("[Go/Pion] handleOffer: PeerConnection successfully created and available.")
	}
	defer pcMutex.Unlock()

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
	log.Println("[Go/Pion] handleOffer: Remote description (offer) set successfully. If offer had a DataChannel, OnDataChannel should have triggered.")

	answer, err := peerConnection.CreateAnswer(nil)
	if err != nil {
		log.Printf("[Go/Pion] Error creating answer: %v\n", err)
		return
	}
	log.Println("[Go/Pion] handleOffer: Answer created successfully.")

	gatherComplete := webrtc.GatheringCompletePromise(peerConnection)
	log.Println("[Go/Pion] handleOffer: Setting Local Description (answer).")
	if err := peerConnection.SetLocalDescription(answer); err != nil {
		log.Printf("[Go/Pion] handleOffer: Error setting local description (answer): %v\n", err)
		return // pcMutex will be unlocked by defer
	}
	select {
	case <-gatherComplete:
		log.Println("[Go/Pion] handleOffer: ICE candidate gathering complete for the answer.")
	case <-time.After(10 * time.Second): // Increased timeout for potentially slow networks/STUN
		log.Println("[Go/Pion] handleOffer: ICE candidate gathering timed out for the answer.")
	}

	if ld := peerConnection.LocalDescription(); ld != nil {
		lastAnswerSDP = ld.SDP // Store the SDP for C++ to retrieve
		log.Printf("[Go/Pion] handleOffer: Answer SDP generated and stored (length: %d)\n", len(lastAnswerSDP))
	} else {
		log.Println("[Go/Pion] handleOffer: LocalDescription is nil after ICE gathering for answer. Cannot provide SDP.")
		lastAnswerSDP = "" // Ensure it's empty if no SDP.
	}
	log.Println("[Go/Pion] handleOffer: Processing complete.")
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
		// log.Println("[Go/Pion] sendVideoPacket: PeerConnection or videoTrack not initialized!")
		return -1
	}

	// Convert C data to Go byte slice
	buf := C.GoBytes(data, size)
	// log.Printf("[Go/Pion] Raw H.264 buffer (first 20 bytes): %x\n", buf[:min(20, len(buf))])

	const frameRate = 60.0
	const frameDuration90kHz = 90000.0 / frameRate
	ptsFloat := float64(pts) / 1000000.0
	baseTimeStamp := uint32(ptsFloat * 90000.0)

	if currentTimestamp == 0 {
		currentTimestamp = baseTimeStamp
	} else {
		expectedIncrement := uint32(frameDuration90kHz)
		if baseTimeStamp > currentTimestamp {
			currentTimestamp = baseTimeStamp
		} else {
			currentTimestamp += expectedIncrement
		}
	}

	const maxPacketSize = 1000

	// Parse NAL units from the buffer
	nalUnits := splitNALUnits(buf)
	if len(nalUnits) == 0 {
		// log.Println("[Go/Pion] sendVideoPacket: No NAL units found in buffer")
		return -1
	}

	// Process each NAL unit
	for i, nal := range nalUnits {
		// Log the first few bytes of the NAL unit for debugging
		if i == 0 {
			// log.Printf("[Go/Pion] First 4 bytes of frame: %x\n", nal[:min(4, len(nal))])
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
				// log.Printf("[Go/Pion] Error sending RTP packet: %v\n", err)
				return -1
			}

			// log.Printf("[Go/Pion] Sent H.264 RTP packet (size: %d, PTS: %d, Seq: %d, Marker: %v)\n",
			// 	len(nal), currentTimestamp, currentSequenceNumber-1, rtpPacket.Header.Marker)
		} else {
			// Fragment the NAL unit into FU-A packets
			err := sendFragmentedNALUnit(nal, maxPacketSize, i == len(nalUnits)-1)
			if err != nil {
				// log.Printf("[Go/Pion] Error sending fragmented NAL unit: %v\n", err)
				return -1
			}
		}
	}

	// log.Printf("[Go/Pion] Sent H.264 frame (total size: %d, PTS: %d)\n", size, currentTimestamp)
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
			// nalType := nal[0] & 0x1F
			// log.Printf("[Go/Pion] Parsed NAL Unit Type: %d, Size: %d\n", nalType, len(nal))
			filteredNALs = append(filteredNALs, nal)
		}
	}

	return filteredNALs
}

// sendFragmentedNALUnit sends a large NAL unit as FU-A packets
// In main.go :: sendFragmentedNALUnit()

func sendFragmentedNALUnit(nal []byte, maxPacketSize int, isLastNAL bool) error {
	if len(nal) < 2 {
		// log.Printf("[Go/Pion] NAL unit too small to fragment (size: %d)\n", len(nal))
		// Optionally send as single packet if possible, or log error
		// For now, let's just return, though this might drop small NALs
		// that somehow triggered fragmentation logic.
		return nil // Or handle appropriately
	}

	nalType := nal[0] & 0x1F            // Extract the NAL unit type (5 bits)
	fuIndicator := (nal[0] & 0xE0) | 28 // FU-A type (28), NRI bits from original NAL
	maxPayloadSize := maxPacketSize - 2 // Account for FU indicator and FU header

	// --- CHANGE: Fragment the NAL payload (nal[1:]) ---
	nalPayload := nal[1:]
	offset := 0
	firstFragment := true
	// --- END CHANGE ---

	// --- CHANGE: Loop condition uses nalPayload ---
	for offset < len(nalPayload) {
		// --- CHANGE: chunkSize calculation uses nalPayload ---
		chunkSize := len(nalPayload) - offset
		if chunkSize > maxPayloadSize {
			chunkSize = maxPayloadSize
		}

		// FU Header
		fuHeader := byte(0)
		if firstFragment {
			fuHeader |= 0x80 // Set start bit
		}
		// --- CHANGE: End bit check uses nalPayload ---
		if offset+chunkSize >= len(nalPayload) {
			fuHeader |= 0x40 // Set end bit
		}
		fuHeader |= nalType // Set original NAL type

		// Create the FU-A payload
		payload := make([]byte, 2+chunkSize)
		payload[0] = fuIndicator
		payload[1] = fuHeader
		// --- CHANGE: Copy chunk from nalPayload ---
		copy(payload[2:], nalPayload[offset:offset+chunkSize])
		// --- END CHANGE ---

		// Log NAL type being fragmented
		// log.Printf("[Go/Pion] Fragmenting NAL Unit Type: %d\n", nalType)

		// Send RTP packet
		rtpPacket := &rtp.Packet{
			Header: rtp.Header{
				Version:        2,
				PayloadType:    96, // Make sure this matches SDP
				SequenceNumber: currentSequenceNumber,
				Timestamp:      currentTimestamp,
				SSRC:           trackSSRC,
				// --- CHANGE: Marker bit check uses nalPayload ---
				Marker: (offset+chunkSize >= len(nalPayload)) && isLastNAL, // Marker on last packet of last NAL
				// --- END CHANGE ---
			},
			Payload: payload,
		}

		currentSequenceNumber++
		if err := videoTrack.WriteRTP(rtpPacket); err != nil {
			// log.Printf("[Go/Pion] Error writing FU-A RTP packet: %v\n", err)
			return err // Return error on write failure
		}

		// log.Printf("[Go/Pion] Sent H.264 FU-A packet (payload size: %d, offset: %d, PTS: %d, Seq: %d, Marker: %v, FU Header: %02x)\n",
		// 	len(payload), offset, currentTimestamp, currentSequenceNumber-1, rtpPacket.Header.Marker, fuHeader)

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
