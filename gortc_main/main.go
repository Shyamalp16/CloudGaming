package main

/*
#cgo CFLAGS: -I.
#include <stdlib.h>
#include <string.h>
*/
import "C"
import (
	"encoding/json"
	"fmt"
	"log"
	"math/rand"
	"strconv"
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
	videoFrameCounter     uint64 // New: for unique frame IDs
	dataChannel           *webrtc.DataChannel
	messageQueue          []string
	mouseQueue            []string
	queueMutex            sync.Mutex
	mouseChannel          *webrtc.DataChannel
	latencyChannel        *webrtc.DataChannel
	videoFeedbackChannel  *webrtc.DataChannel
	pingTimestamps        map[uint64]int64 // Stores host_send_time for video_frame_ping
	pingTimestampsMutex   sync.Mutex       // Mutex for pingTimestamps
	connectionState       webrtc.PeerConnectionState
)

func init() {
	rand.Seed(time.Now().UnixNano())
}

func enqueueMessage(msg string) {
	queueMutex.Lock()
	defer queueMutex.Unlock()
	log.Printf(
		"[Go/Pion] --> Enqueueing message: '%s'. Queue size BEFORE: %d",
		msg,
		len(messageQueue),
	)
	messageQueue = append(messageQueue, msg)
	log.Printf(
		"[Go/Pion] --> Enqueued message: '%s'. Queue size AFTER: %d",
		msg,
		len(messageQueue),
	)
}

func enqueueMouseEvent(msg string) {
	queueMutex.Lock()
	defer queueMutex.Unlock()
	log.Printf(
		"[Go/Pion] --> Enqueueing message: '%s'. Queue size BEFORE: %d",
		msg,
		len(mouseQueue),
	)
	mouseQueue = append(mouseQueue, msg)
	log.Printf(
		"[Go/Pion] --> Enqueued message: '%s'. Queue size AFTER: %d",
		msg,
		len(mouseQueue),
	)
}

//export getDataChannelMessage
func getDataChannelMessage() *C.char {
	queueMutex.Lock()
	defer queueMutex.Unlock()
	if len(messageQueue) == 0 {
		return nil
	}
	msg := messageQueue[0]
	messageQueue = messageQueue[1:]
	log.Printf(
		"[Go/Pion] <-- getDataChannelMessage: Dequeued: '%s'. Queue size AFTER: %d",
		msg,
		len(messageQueue),
	)
	return C.CString(msg)
}

//export getMouseChannelMessage
func getMouseChannelMessage() *C.char {
	queueMutex.Lock()
	defer queueMutex.Unlock()
	if len(mouseQueue) == 0 {
		return nil
	}
	msg := mouseQueue[0]
	mouseQueue = mouseQueue[1:]
	log.Printf(
		"[Go/Pion] <-- getMouseChannelMessage: Dequeued: '%s'. Queue size AFTER: %d",
		msg,
		len(mouseQueue),
	)
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

	if peerConnection != nil {
		_ = peerConnection.Close()
		peerConnection = nil
		videoTrack = nil
		if dataChannel != nil {
			if err := dataChannel.Close(); err != nil {
				log.Printf(
					"[Go/Pion] Error closing existing DataChannel: %v\n",
					err,
				)
			}
		}
		dataChannel = nil
		mouseChannel = nil
		messageQueue = []string{}
		mouseQueue = []string{}
		lastAnswerSDP = ""
		pingTimestamps = make(map[uint64]int64) // Initialize/clear the map
		log.Println(
			"[Go/Pion] createPeerConnectionGo: Closed previous PeerConnection and reset state.",
		)
	} else {
		// This is the first run, initialize the map.
		pingTimestamps = make(map[uint64]int64)
		log.Println("[Go/Pion] createPeerConnectionGo: Initializing pingTimestamps for new PeerConnection.")
	}

	mediaEngine := &webrtc.MediaEngine{}
	codec := webrtc.RTPCodecParameters{
		RTPCodecCapability: webrtc.RTPCodecCapability{
			MimeType:    "video/h264",
			ClockRate:   90000,
			SDPFmtpLine: "level-asymmetry-allowed=1;packetization-mode=1;profile-level-id=42e01f",
		},
		PayloadType: 96,
	}
	if err := mediaEngine.RegisterCodec(codec, webrtc.RTPCodecTypeVideo); err != nil {
		log.Printf("[Go/Pion] Error registering H.264 codec: %v\n", err)
		// pcMutex.Unlock()
		return 0
	}

	if err := mediaEngine.RegisterDefaultCodecs(); err != nil {
		log.Printf("[Go/Pion] Error registering default codecs: %v\n", err)
		// pcMutex.Unlock()
		return 0
	}
	log.Println("[Go/Pion] createPeerConnectionGo: MediaEngine configured.")

	api := webrtc.NewAPI(webrtc.WithMediaEngine(mediaEngine))

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
	log.Println(
		"[Go/Pion] createPeerConnectionGo: New PeerConnection instance created successfully.",
	)

	peerConnection.OnDataChannel(func(dc *webrtc.DataChannel) {
		actualLabel := dc.Label()
		var idStr string
		if dc.ID() != nil {
			idStr = fmt.Sprintf("%d", *dc.ID())
		} else {
			idStr = "nil"
		}

		log.Printf(
			"[Go/Pion] OnDataChannel FIRED! Actual Label RECEIVED: '%s', ID: %s, Negotiated: %v, ReadyState: %s\n",
			actualLabel,
			idStr,
			dc.Negotiated(),
			dc.ReadyState().String(),
		)

		if actualLabel == "keyPressChannel" {
			log.Printf(
				"[Go/Pion] OnDataChannel: Label MATCHED ('%s'). Assigning to global dataChannel and attaching handlers.\n",
				actualLabel,
			)
			// --- DEADLOCK FIX: REMOVED pcMutex.Lock() HERE ---
			if dataChannel != nil && dataChannel != dc {
				log.Printf(
					"[Go/Pion] OnDataChannel: Closing previous global data channel '%s' before assigning new one.\n",
					dataChannel.Label(),
				)
				if errClose := dataChannel.Close(); errClose != nil {
					log.Printf(
						"[Go/Pion] OnDataChannel: Error closing previous global dataChannel: %v\n",
						errClose,
					)
				}
			}
			dataChannel = dc
			log.Printf(
				"[Go/Pion] OnDataChannel: Global 'dataChannel' variable assigned to new DC with label '%s'.",
				dataChannel.Label(),
			)
			// --- DEADLOCK FIX: REMOVED pcMutex.Unlock() HERE ---

			dc.OnOpen(func() {
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
				log.Printf(
					"[Go/Pion] Data channel '%s' (local, ID: %s) OnOpen event. Current Global DC: '%s' (ID: %s). Local DC ReadyState: %s\n",
					dc.Label(),
					idStr,
					gdcLabel,
					gdcID,
					dc.ReadyState().String(),
				)
			})

			dc.OnMessage(func(msg webrtc.DataChannelMessage) {
				log.Printf(
					"[Go/Pion] >>> DataChannel '%s' (ID: %s) OnMessage RECEIVED: %s\n",
					dc.Label(),
					idStr,
					string(msg.Data),
				)
				// Parse message to extract client_send_time
				var messageData map[string]interface{}
				if err := json.Unmarshal(msg.Data, &messageData); err == nil {
					if clientSendTime, ok := messageData["client_send_time"].(float64); ok {
						hostReceiveTime := float64(time.Now().UnixNano()) / float64(time.Millisecond) // Current time in milliseconds
						oneWayLatency := hostReceiveTime - clientSendTime
						log.Printf(
							"[Go/Pion] Keyboard event one-way latency: %.2f ms (Client: %.2f, Host: %.2f)\n",
							oneWayLatency, clientSendTime, hostReceiveTime,
						)
					}
				}
				enqueueMessage(string(msg.Data))
			})

			dc.OnClose(func() {
				log.Printf(
					"[Go/Pion] Data channel '%s' (ID: %s) OnClose event. ReadyState: %s\n",
					dc.Label(),
					idStr,
					dc.ReadyState().String(),
				)
				pcMutex.Lock()
				if dataChannel == dc {
					log.Printf(
						"[Go/Pion] OnClose: Global dataChannel ('%s', ID: %s) is being closed. Setting global to nil.\n",
						dc.Label(),
						idStr,
					)
					dataChannel = nil
				}
				pcMutex.Unlock()
			})

			dc.OnError(func(err error) {
				log.Printf(
					"[Go/Pion] Data channel '%s' (ID: %s) OnError event: %v\n",
					dc.Label(),
					idStr,
					err,
				)
			})
			log.Printf(
				"[Go/Pion] OnDataChannel: All handlers (OnOpen, OnMessage, OnClose, OnError) attached for DC '%s'.\n",
				actualLabel,
			)

		} else if actualLabel == "mouseChannel" {
			log.Printf(
				"[Go/Pion] OnDataChannel: Label MATCHED ('%s'). Assigning to global mouseChannel and attaching handlers.\n",
				actualLabel,
			)
			// --- DEADLOCK FIX: REMOVED pcMutex.Lock() HERE ---
			if mouseChannel != nil && mouseChannel != dc {
				log.Printf(
					"[Go/Pion] OnDataChannel: Closing previous global mouse channel '%s' (ID: %s) before assigning new one.\n",
					mouseChannel.Label(),
					fmt.Sprintf("%d", *mouseChannel.ID()),
				)
				if errClose := mouseChannel.Close(); errClose != nil {
					log.Printf(
						"[Go/Pion] OnDataChannel: Error closing previous global mouseChannel: %v\n",
						errClose,
					)
				}
			}
			mouseChannel = dc
			// --- DEADLOCK FIX: REMOVED pcMutex.Unlock() HERE ---

			dc.OnOpen(func() {
				pcMutex.Lock()
				gdcLabel := "nil (global)"
				gdcID := "nil"
				if mouseChannel != nil {
					gdcLabel = mouseChannel.Label()
					if mouseChannel.ID() != nil {
						gdcID = fmt.Sprintf("%d", *mouseChannel.ID())
					}
				}
				pcMutex.Unlock()
				log.Printf(
					"[Go/Pion] Mouse channel '%s' (local, ID: %s) OnOpen event. Current Global DC: '%s' (ID: %s). Local DC ReadyState: %s\n",
					dc.Label(),
					idStr,
					gdcLabel,
					gdcID,
					dc.ReadyState().String(),
				)
			})

			dc.OnMessage(func(msg webrtc.DataChannelMessage) {
				log.Printf(
					"[Go/Pion] >>> DataChannel '%s' (ID: %s) OnMessage RECEIVED: %s\n",
					dc.Label(),
					idStr,
					string(msg.Data),
				)
				var messageData map[string]interface{}
				if err := json.Unmarshal(msg.Data, &messageData); err == nil {
					if clientSendTime, ok := messageData["client_send_time"].(float64); ok {
						hostReceiveTime := float64(time.Now().UnixNano()) / float64(time.Millisecond)
						oneWayLatency := hostReceiveTime - clientSendTime
						log.Printf(
							"[Go/Pion] Mouse event one-way latency: %.2f ms (Client: %.2f, Host: %.2f)\n",
							oneWayLatency, clientSendTime, hostReceiveTime,
						)
					}
				}
				enqueueMouseEvent(string(msg.Data))
			})

			dc.OnClose(func() {
				log.Printf(
					"[Go/Pion] Mouse channel '%s' (ID: %s) OnClose event. ReadyState: %s\n",
					dc.Label(),
					idStr,
					dc.ReadyState().String(),
				)
				pcMutex.Lock()
				if mouseChannel == dc {
					log.Printf(
						"[Go/Pion] OnClose: Global mouseChannel ('%s', ID: %s) is being closed. Setting global to nil.\n",
						dc.Label(),
						idStr,
					)
					mouseChannel = nil
				}
				pcMutex.Unlock()
			})

			dc.OnError(func(err error) {
				log.Printf(
					"[Go/Pion] Data channel '%s' (ID: %s) OnError event: %v\n",
					dc.Label(),
					idStr,
					err,
				)
			})
			log.Printf(
				"[Go/Pion] OnDataChannel: All handlers attached for mouse DC '%s'.\n",
				actualLabel,
			)
		} else if actualLabel == "latencyChannel" {
			log.Printf(
				"[Go/Pion] OnDataChannel: Label MATCHED ('%s'). Assigning to global latencyChannel and attaching handlers.\n",
				actualLabel,
			)
			if latencyChannel != nil && latencyChannel != dc {
				log.Printf(
					"[Go/Pion] OnDataChannel: Closing previous global latency channel '%s' (ID: %s) before assigning new one.\n",
					latencyChannel.Label(),
					fmt.Sprintf("%d", *latencyChannel.ID()),
				)
				if errClose := latencyChannel.Close(); errClose != nil {
					log.Printf(
						"[Go/Pion] OnDataChannel: Error closing previous global latencyChannel: %v\n",
						errClose,
					)
				}
			}
			latencyChannel = dc

			dc.OnOpen(func() {
				log.Printf(
					"[Go/Pion] Latency channel '%s' (ID: %s) OnOpen event. ReadyState: %s\n",
					dc.Label(),
					idStr,
					dc.ReadyState().String(),
				)
			})

			dc.OnMessage(func(msg webrtc.DataChannelMessage) {
				log.Printf(
					"[Go/Pion] >>> DataChannel '%s' (ID: %s) OnMessage RECEIVED: %s\n",
					dc.Label(),
					idStr,
					string(msg.Data),
				)
				var messageData map[string]interface{}
				if err := json.Unmarshal(msg.Data, &messageData); err == nil {
					if msgType, ok := messageData["type"].(string); ok {
						if msgType == "ping" {
							if clientTimestamp, ok := messageData["timestamp"].(float64); ok {
								if sequenceNumber, ok := messageData["sequence_number"].(float64); ok {
									hostReceiveTime := float64(time.Now().UnixNano()) / float64(time.Millisecond)

									pongResponse := map[string]interface{}{
										"type":              "pong",
										"timestamp":         clientTimestamp,
										"sequence_number":   sequenceNumber,
										"host_receive_time": hostReceiveTime,
									}
									pongJSON, _ := json.Marshal(pongResponse)
									if err := dc.SendText(string(pongJSON)); err != nil {
										log.Printf("[Go/Pion] Error sending pong: %v\n", err)
									} else {
										log.Printf(
											"[Go/Pion] Sent pong for sequence %d (Client: %.2f, Host: %.2f)\n",
											int(sequenceNumber), clientTimestamp, hostReceiveTime,
										)
									}
								}
							}
						}
					}
				}
			})

			dc.OnClose(func() {
				log.Printf(
					"[Go/Pion] Latency channel '%s' (ID: %s) OnClose event. ReadyState: %s\n",
					dc.Label(),
					idStr,
					dc.ReadyState().String(),
				)
				pcMutex.Lock()
				if latencyChannel == dc {
					log.Printf(
						"[Go/Pion] OnClose: Global latencyChannel ('%s', ID: %s) is being closed. Setting global to nil.\n",
						dc.Label(),
						idStr,
					)
					latencyChannel = nil
				}
				pcMutex.Unlock()
			})

			dc.OnError(func(err error) {
				log.Printf(
					"[Go/Pion] Data channel '%s' (ID: %s) OnError event: %v\n",
					dc.Label(),
					idStr,
					err,
				)
			})
			log.Printf(
				"[Go/Pion] OnDataChannel: All handlers attached for latency DC '%s'.",
				actualLabel,
			)
		} else if actualLabel == "videoFeedbackChannel" {
			log.Printf(
				"[Go/Pion] OnDataChannel: Label MATCHED ('%s'). Assigning to global videoFeedbackChannel and attaching handlers.",
				actualLabel,
			)
			if videoFeedbackChannel != nil && videoFeedbackChannel != dc {
				log.Printf(
					"[Go/Pion] OnDataChannel: Closing previous global videoFeedbackChannel '%s' (ID: %s) before assigning new one.",
					videoFeedbackChannel.Label(),
					fmt.Sprintf("%d", *videoFeedbackChannel.ID()),
				)
				if errClose := videoFeedbackChannel.Close(); errClose != nil {
					log.Printf(
						"[Go/Pion] OnDataChannel: Error closing previous global videoFeedbackChannel: %v",
						errClose,
					)
				}
			}
			videoFeedbackChannel = dc

			dc.OnOpen(func() {
				log.Printf(
					"[Go/Pion] Video Feedback channel '%s' (ID: %s) OnOpen event. ReadyState: %s",
					dc.Label(),
					idStr,
					dc.ReadyState().String(),
				)
			})

			dc.OnMessage(func(msg webrtc.DataChannelMessage) {
				log.Printf(
					"[Go/Pion] >>> DataChannel '%s' (ID: %s) OnMessage RECEIVED: %s",
					dc.Label(),
					idStr,
					string(msg.Data),
				)
				// Host doesn't need to process pong for RTT, just log for now
				var messageData map[string]interface{}
				if err := json.Unmarshal(msg.Data, &messageData); err == nil {
					if msgType, ok := messageData["type"].(string); ok {
						if msgType == "video_frame_pong" {
							if frameIDFloat, ok := messageData["frame_id"].(float64); ok {
								frameID := uint64(frameIDFloat)
								if hostSendTimeString, ok := messageData["host_send_time"].(string); ok {
									hostSendTime, err := strconv.ParseInt(hostSendTimeString, 10, 64)
									if err != nil {
										log.Printf("[Go/Pion] Error parsing host_send_time from pong: %v", err)
										return
									}

									pingTimestampsMutex.Lock()
									originalHostSendTime, found := pingTimestamps[frameID]
									delete(pingTimestamps, frameID) // Clean up the map
									pingTimestampsMutex.Unlock()

									if found {
										// Optional: Verify the timestamp hasn't been tampered with
										if hostSendTime != originalHostSendTime {
											log.Printf("[Go/Pion] [PONG] Timestamp mismatch for frame %d. Original: %d, Received: %d", frameID, originalHostSendTime, hostSendTime)
											return
										}
										hostReceiveTime := time.Now().UnixNano()
										rttNano := hostReceiveTime - hostSendTime
										rttMilli := float64(rttNano) / float64(time.Millisecond)
										log.Printf("[Go/Pion] [PONG] RTT for frame %d: %.2f ms", frameID, rttMilli)

										// Send RTT update back to the client
										rttUpdateMsg := map[string]interface{}{
											"type": "rtt_update",
											"rtt":  rttMilli,
										}
										rttJSON, _ := json.Marshal(rttUpdateMsg)
										if err := dc.SendText(string(rttJSON)); err != nil {
											log.Printf("[Go/Pion] Error sending RTT update: %v", err)
										}
									} else {
										log.Printf("[Go/Pion] [PONG] Received pong for unknown or expired frame_id: %d", frameID)
									}
								} else {
									log.Printf("[Go/Pion] [PONG] Invalid 'host_send_time' in pong message: %v", messageData["host_send_time"])
								}
							} else {
								log.Printf("[Go/Pion] [PONG] Invalid 'frame_id' in pong message: %v", messageData["frame_id"])
							}
						}
					}
				}
			})

			dc.OnClose(func() {
				log.Printf(
					"[Go/Pion] Video Feedback channel '%s' (ID: %s) OnClose event. ReadyState: %s",
					dc.Label(),
					idStr,
					dc.ReadyState().String(),
				)
				pcMutex.Lock()
				if videoFeedbackChannel == dc {
					log.Printf(
						"[Go/Pion] OnClose: Global videoFeedbackChannel ('%s', ID: %s) is being closed. Setting global to nil.",
						dc.Label(),
						idStr,
					)
					videoFeedbackChannel = nil
				}
				pcMutex.Unlock()
			})

			dc.OnError(func(err error) {
				log.Printf(
					"[Go/Pion] Data channel '%s' (ID: %s) OnError event: %v",
					dc.Label(),
					idStr,
					err,
				)
			})
			log.Printf(
				"[Go/Pion] OnDataChannel: All handlers attached for video feedback DC '%s'.",
				actualLabel,
			)
		} else {
			log.Printf(
				"[Go/Pion] OnDataChannel: Label MISMATCH. Expected 'keyPressChannel', 'mouseChannel', 'latencyChannel' or 'videoFeedbackChannel' but received '%s' (ID: %s). Handlers NOT attached for this DC.",
				actualLabel,
				idStr,
			)
		}
	})
	log.Println(
		"[Go/Pion] createPeerConnectionGo: OnDataChannel handler has been set up on the PeerConnection.",
	)

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
			log.Printf(
				"[Go/Pion] createPeerConnectionGo: Error closing PeerConnection after track failure: %v\n",
				pcErr,
			)
		}
		peerConnection = nil
		pcMutex.Unlock()
		return 0
	}

	rtpSender, err := peerConnection.AddTrack(videoTrack)
	if err != nil {
		log.Printf("[Go/Pion] Error adding video track: %v\n", err)
		if pcErr := peerConnection.Close(); pcErr != nil {
			log.Printf(
				"[Go/Pion] createPeerConnectionGo: Error closing PeerConnection after AddTrack failure: %v\n",
				pcErr,
			)
		}
		peerConnection = nil
		pcMutex.Unlock()
		return 0
	}

	params := rtpSender.GetParameters()
	if len(params.Encodings) > 0 {
		trackSSRC = uint32(params.Encodings[0].SSRC)
		log.Printf(
			"[Go/Pion] Successfully captured SSRC for video track: %d\n",
			trackSSRC,
		)
	} else {
		log.Println(
			"[Go/Pion] CRITICAL: Could not get SSRC from RTPSender parameters.",
		)
	}

	peerConnection.OnICECandidate(func(candidate *webrtc.ICECandidate) {
		if candidate != nil {
			log.Printf(
				"[Go/Pion] OnICECandidate: %s\n",
				candidate.ToJSON().Candidate,
			)
		} else {
			log.Println(
				"[Go/Pion] OnICECandidate: ICE Candidate gathering complete (nil candidate received).",
			)
		}
	})
	peerConnection.OnICEConnectionStateChange(func(state webrtc.ICEConnectionState) {
		log.Printf("[Go/Pion] ICE Connection State: %s\n", state.String())
	})
	peerConnection.OnConnectionStateChange(func(state webrtc.PeerConnectionState) {
		pcMutex.Lock()
		connectionState = state
		pcMutex.Unlock()
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
			log.Println(
				"[Go/Pion] handleOffer: Failed to create PeerConnection. Aborting offer handling.",
			)
			return
		}
		pcMutex.Lock()
		if peerConnection == nil {
			log.Println(
				"[Go/Pion] handleOffer: PeerConnection is STILL nil after creation attempt. Aborting.",
			)
			pcMutex.Unlock()
			return
		}
		log.Println(
			"[Go/Pion] handleOffer: PeerConnection successfully created and available.",
		)
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
	log.Println(
		"[Go/Pion] handleOffer: Remote description (offer) set successfully. If offer had a DataChannel, OnDataChannel should have triggered.",
	)

	answer, err := peerConnection.CreateAnswer(nil)
	if err != nil {
		log.Printf("[Go/Pion] Error creating answer: %v\n", err)
		return
	}
	log.Println("[Go/Pion] handleOffer: Answer created successfully.")

	gatherComplete := webrtc.GatheringCompletePromise(peerConnection)
	log.Println("[Go/Pion] handleOffer: Setting Local Description (answer).")
	if err := peerConnection.SetLocalDescription(answer); err != nil {
		log.Printf(
			"[Go/Pion] handleOffer: Error setting local description (answer): %v\n",
			err,
		)
		return
	}
	select {
	case <-gatherComplete:
		log.Println(
			"[Go/Pion] handleOffer: ICE candidate gathering complete for the answer.",
		)
	case <-time.After(10 * time.Second):
		log.Println(
			"[Go/Pion] handleOffer: ICE candidate gathering timed out for the answer.",
		)
	}

	if ld := peerConnection.LocalDescription(); ld != nil {
		lastAnswerSDP = ld.SDP
		log.Printf(
			"[Go/Pion] handleOffer: Answer SDP generated and stored (length: %d)\n",
			len(lastAnswerSDP),
		)
	} else {
		log.Println(
			"[Go/Pion] handleOffer: LocalDescription is nil after ICE gathering for answer. Cannot provide SDP.",
		)
		lastAnswerSDP = ""
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
		return -1
	}

	buf := C.GoBytes(data, size)

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

	nalUnits := splitNALUnits(buf)
	if len(nalUnits) == 0 {
		return -1
	}

	for i, nal := range nalUnits {
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
				return -1
			}
		} else {
			err := sendFragmentedNALUnit(nal, maxPacketSize, i == len(nalUnits)-1)
			if err != nil {
				return -1
			}
		}
	}

	// After sending all RTP packets for the frame, send a video_frame_ping
	if videoFeedbackChannel != nil && videoFeedbackChannel.ReadyState() == webrtc.DataChannelStateOpen {
		videoFrameCounter++
		hostSendTime := time.Now().UnixNano()

		pingTimestampsMutex.Lock()
		pingTimestamps[videoFrameCounter] = hostSendTime
		pingTimestampsMutex.Unlock()

		pingMessage := map[string]interface{}{
			"type":           "video_frame_ping",
			"frame_id":       videoFrameCounter,
			"host_send_time": fmt.Sprintf("%d", hostSendTime), // Send as a string to avoid JS precision loss
		}

		pingJSON, err := json.Marshal(pingMessage)
		if err != nil {
			log.Printf("[Go/Pion] Error marshalling video_frame_ping: %v\n", err)
			return 0 // Keep as C.int return
		}

		if err := videoFeedbackChannel.SendText(string(pingJSON)); err != nil {
			log.Printf("[Go/Pion] Error sending video_frame_ping: %v\n", err)
		} else {
			log.Printf("[Go/Pion] [PING] Sent video_frame_ping for frame %d.", videoFrameCounter)
		}
	}

	return 0
}

func splitNALUnits(buf []byte) [][]byte {
	var nalUnits [][]byte
	start := 0
	i := 0

	for i < len(buf) {
		if i+3 < len(buf) && buf[i] == 0 && buf[i+1] == 0 && buf[i+2] == 0 && buf[i+3] == 1 {
			if i > start {
				nalUnits = append(nalUnits, buf[start:i])
			}
			start = i + 4
			i += 4
		} else if i+2 < len(buf) && buf[i] == 0 && buf[i+1] == 0 && buf[i+2] == 1 {
			if i > start {
				nalUnits = append(nalUnits, buf[start:i])
			}
			start = i + 3
			i += 3
		} else {
			i++
		}
	}

	if start < len(buf) {
		nalUnits = append(nalUnits, buf[start:])
	}

	var filteredNALs [][]byte
	for _, nal := range nalUnits {
		if len(nal) > 0 {
			filteredNALs = append(filteredNALs, nal)
		}
	}

	return filteredNALs
}

func sendFragmentedNALUnit(nal []byte, maxPacketSize int, isLastNAL bool) error {
	if len(nal) < 2 {
		return nil
	}

	nalType := nal[0] & 0x1F
	fuIndicator := (nal[0] & 0xE0) | 28
	maxPayloadSize := maxPacketSize - 2

	nalPayload := nal[1:]
	offset := 0
	firstFragment := true

	for offset < len(nalPayload) {
		chunkSize := len(nalPayload) - offset
		if chunkSize > maxPayloadSize {
			chunkSize = maxPayloadSize
		}

		fuHeader := byte(0)
		if firstFragment {
			fuHeader |= 0x80
		}
		if offset+chunkSize >= len(nalPayload) {
			fuHeader |= 0x40
		}
		fuHeader |= nalType

		payload := make([]byte, 2+chunkSize)
		payload[0] = fuIndicator
		payload[1] = fuHeader
		copy(payload[2:], nalPayload[offset:offset+chunkSize])

		rtpPacket := &rtp.Packet{
			Header: rtp.Header{
				Version:        2,
				PayloadType:    96,
				SequenceNumber: currentSequenceNumber,
				Timestamp:      currentTimestamp,
				SSRC:           trackSSRC,
				Marker:         (offset+chunkSize >= len(nalPayload)) && isLastNAL,
			},
			Payload: payload,
		}

		currentSequenceNumber++
		if err := videoTrack.WriteRTP(rtpPacket); err != nil {
			return err
		}

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

//export getConnectionState
func getConnectionState() C.int {
	pcMutex.Lock()
	defer pcMutex.Unlock()
	return C.int(connectionState)
}

//export getPeerConnectionState
func getPeerConnectionState() C.int {
	pcMutex.Lock()
	defer pcMutex.Unlock()

	if peerConnection == nil {
		return C.int(0)
	}

	state := connectionState
	log.Printf("[Go/Pion] PeerConnection state: %s\n", state.String())

	switch state {
	case webrtc.PeerConnectionStateNew:
		return C.int(0)
	case webrtc.PeerConnectionStateConnecting:
		return C.int(1)
	case webrtc.PeerConnectionStateConnected:
		return C.int(3)
	case webrtc.PeerConnectionStateDisconnected:
		return C.int(4)
	case webrtc.PeerConnectionStateFailed:
		return C.int(5)
	case webrtc.PeerConnectionStateClosed:
		return C.int(6)
	default:
		return C.int(-1)
	}
}

//export initGo
func initGo() C.int {
	log.Println("[Go/Pion] initGo: Initializing Go WebRTC module.")
	return createPeerConnectionGo()
}

//export closeGo
func closeGo() {
	log.Println("[Go/Pion] closeGo: Closing Go WebRTC module.")
	closePeerConnection()
}

func main() {
	// This main function is required for building as a C shared library,
	// but its contents are not directly executed when loaded as a DLL.
	// Initialization and cleanup are handled by initGo and closeGo.
	log.Println("[Go/Pion] main() in DLL. Not directly executed.")
}
