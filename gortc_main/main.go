package main

/*
#cgo CFLAGS: -I.
#include <stdlib.h>
#include <string.h>

typedef void (*RTCPCallback)(double packetLoss, double rtt, double jitter);
typedef void (*OnPLICallback)();

// Helper function to call the C function pointer
static inline void callRTCPCallback(RTCPCallback f, double p, double r, double j) {
    if (f) {
        f(p, r, j);
    }
}

static inline void callPLICallback(OnPLICallback f) {
    if (f) { f(); }
}

// Provide no-op wake functions so cgo can resolve C.wakeKeyboardThread / C.wakeMouseThread
// at Go DLL build time. The C++ host exports real versions; if you later want to
// forward to them from inside this DLL, replace these with proper imports via LDFLAGS.
static inline void wakeKeyboardThread(void) { }
static inline void wakeMouseThread(void) { }
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

	"github.com/pion/interceptor"
	"github.com/pion/rtcp"
	"github.com/pion/rtp"
	"github.com/pion/webrtc/v3"
	"github.com/pion/webrtc/v3/pkg/media"
)

// normalizeToMs converts seconds/ms/us/ns epoch or relative values to milliseconds.
func normalizeToMs(v interface{}) (float64, bool) {
	var x float64
	switch t := v.(type) {
	case float64:
		x = t
	case string:
		f, err := strconv.ParseFloat(t, 64)
		if err != nil {
			return 0, false
		}
		x = f
	default:
		return 0, false
	}
	if x <= 0 {
		return 0, false
	}
	if x >= 1e17 { // ns epoch
		return x / 1e6, true
	}
	if x >= 1e15 { // us epoch
		return x / 1e3, true
	}
	if x >= 1e12 { // ms epoch
		return x, true
	}
	if x >= 1e9 { // s epoch
		return x * 1e3, true
	}
	if x >= 1e6 { // already ms-scale relative
		return x, true
	}
	return x * 1e3, true // treat as seconds otherwise
}

var rtcpCallback C.RTCPCallback
var pliCallback C.OnPLICallback

// Global variables
var (
	peerConnection        *webrtc.PeerConnection
	pcMutex               sync.Mutex
	videoTrack            *webrtc.TrackLocalStaticSample // switched to sample track for pacing
	audioTrack            *webrtc.TrackLocalStaticRTP
	trackSSRC             uint32
	audioSSRC             uint32
	lastAnswerSDP         string
	currentSequenceNumber uint16
	currentTimestamp      uint32
	currentAudioSeq       uint16
	currentAudioTS        uint32
	videoFrameCounter     uint64
	dataChannel           *webrtc.DataChannel
	messageQueue          []string
	mouseQueue            []string
	queueMutex            sync.Mutex
	mouseChannel          *webrtc.DataChannel
	latencyChannel        *webrtc.DataChannel
	videoFeedbackChannel  *webrtc.DataChannel
	pingTimestamps        map[uint64]int64
	pingTimestampsMutex   sync.Mutex
	connectionState       webrtc.PeerConnectionState
	videoPayloadType      uint8 = 96
	audioPayloadType      uint8 = 111
	// Buffer remote ICE candidates received before remote SDP is set
	pendingRemoteCandidates []webrtc.ICECandidateInit
	// Cache latest RTT (ms) from ping/pong to combine with RTCP loss/jitter
	lastRttMutex sync.Mutex
	lastRttMs    float64
	// Throttled logging for enqueues
	lastEnqueueLog    time.Time
	msgEnqueueCount   int
	mouseEnqueueCount int

	// send rate logging
	sendLastLog time.Time
	sendCount   int
)

// Reusable buffer pool for media samples to reduce per-frame allocations
var sampleBufPool sync.Pool

func getSampleBuf(n int) []byte {
	v := sampleBufPool.Get()
	if v == nil {
		return make([]byte, n)
	}
	b := v.([]byte)
	if cap(b) < n {
		b = make([]byte, n)
	}
	return b[:n]
}

func putSampleBuf(b []byte) {
	// Return full capacity slice to pool for reuse
	sampleBufPool.Put(b[:cap(b)])
}

func init() {
	rand.Seed(time.Now().UnixNano())
}

//export sendAudioPacket
func sendAudioPacket(data unsafe.Pointer, size C.int, pts C.longlong) C.int {
	pcMutex.Lock()
	defer pcMutex.Unlock()

	if peerConnection == nil || audioTrack == nil {
		return -1
	}
	if connectionState != webrtc.PeerConnectionStateConnected {
		return 0
	}

	// Reuse buffer from pool to avoid per-call allocation
	n := int(size)
	payload := getSampleBuf(n)
	C.memcpy(unsafe.Pointer(&payload[0]), data, C.size_t(n))

	// Opus uses 48kHz clock; derive timestamp from pts (us)
	ptsSeconds := float64(pts) / 1_000_000.0
	currentAudioTS = uint32(ptsSeconds * 48000.0)

	pkt := &rtp.Packet{
		Header: rtp.Header{
			Version:        2,
			PayloadType:    audioPayloadType,
			SequenceNumber: currentAudioSeq,
			Timestamp:      currentAudioTS,
			SSRC:           audioSSRC,
			Marker:         false,
		},
		Payload: payload,
	}
	currentAudioSeq++

	if err := audioTrack.WriteRTP(pkt); err != nil {
		return -1
	}
	// Return buffer to pool after write
	putSampleBuf(payload)
	return 0
}

//export sendVideoSample
func sendVideoSample(data unsafe.Pointer, size C.int, durationUs C.longlong) C.int {
	pcMutex.Lock()
	defer pcMutex.Unlock()

	if peerConnection == nil || videoTrack == nil {
		return -1
	}
	if connectionState != webrtc.PeerConnectionStateConnected {
		return 0
	}

	// Reuse buffer from pool to avoid per-call allocation
	n := int(size)
	buf := getSampleBuf(n)
	C.memcpy(unsafe.Pointer(&buf[0]), data, C.size_t(n))
	dur := time.Duration(int64(durationUs)) * time.Microsecond

	if err := videoTrack.WriteSample(media.Sample{Data: buf, Duration: dur}); err != nil {
		return -1
	}
	// Return buffer to pool after write
	putSampleBuf(buf)

	// Debug: log send rate once per second (disabled during streaming)
	sendCount++
	if time.Since(sendLastLog) >= time.Second {
		// log.Printf("[Pion] send samples/s: %d", sendCount)
		sendCount = 0
		sendLastLog = time.Now()
	}

	// Optional RTT ping to client (unchanged):
	if videoFeedbackChannel != nil && videoFeedbackChannel.ReadyState() == webrtc.DataChannelStateOpen {
		videoFrameCounter++
		hostSendTime := time.Now().UnixNano()
		pingTimestampsMutex.Lock()
		pingTimestamps[videoFrameCounter] = hostSendTime
		pingTimestampsMutex.Unlock()
		pingMessage := map[string]interface{}{
			"type":           "video_frame_ping",
			"frame_id":       videoFrameCounter,
			"host_send_time": fmt.Sprintf("%d", hostSendTime),
		}
		if pingJSON, err := json.Marshal(pingMessage); err == nil {
			_ = videoFeedbackChannel.SendText(string(pingJSON))
		}
	}
	return 0
}

func enqueueMessage(msg string) {
	queueMutex.Lock()
	defer queueMutex.Unlock()
	messageQueue = append(messageQueue, msg)
	msgEnqueueCount++
	if time.Since(lastEnqueueLog) >= time.Second {
		// log.Printf("[Go/Pion] queued key msgs=%d mouse=%d", msgEnqueueCount, mouseEnqueueCount)
		msgEnqueueCount = 0
		mouseEnqueueCount = 0
		lastEnqueueLog = time.Now()
	}
	// Wake the keyboard thread after enqueueing
	C.wakeKeyboardThread()
}

func enqueueMouseEvent(msg string) {
	queueMutex.Lock()
	defer queueMutex.Unlock()
	mouseQueue = append(mouseQueue, msg)
	mouseEnqueueCount++
	if time.Since(lastEnqueueLog) >= time.Second {
		// log.Printf("[Go/Pion] queued key msgs=%d mouse=%d", msgEnqueueCount, mouseEnqueueCount)
		msgEnqueueCount = 0
		mouseEnqueueCount = 0
		lastEnqueueLog = time.Now()
	}
	// Wake the mouse thread after enqueueing
	C.wakeMouseThread()
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

//export enqueueDataChannelMessage
func enqueueDataChannelMessage(msg *C.char) {
	goMsg := C.GoString(msg)
	enqueueMessage(goMsg)
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
	// log.Printf(
	// 	"[Go/Pion] <-- getMouseChannelMessage: Dequeued: '%s'. Queue size AFTER: %d",
	// 	msg,
	// 	len(mouseQueue),
	// )
	return C.CString(msg)
}

//export enqueueMouseChannelMessage
func enqueueMouseChannelMessage(msg *C.char) {
	goMsg := C.GoString(msg)
	enqueueMouseEvent(goMsg)
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
		pendingRemoteCandidates = nil
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
	i := &interceptor.Registry{}

	// Use the factory to add the interceptor
	i.Add(&rtcpReaderFactory{})

	if err := webrtc.RegisterDefaultInterceptors(mediaEngine, i); err != nil {
		log.Printf("[Go/Pion] Error registering default interceptors: %v\n", err)
		return 0
	}
	// codec := webrtc.RTPCodecParameters{
	// 	RTPCodecCapability: webrtc.RTPCodecCapability{
	// 		MimeType:    "video/h264",
	// 		ClockRate:   90000,
	// 		SDPFmtpLine: "level-asymmetry-allowed=1;packetization-mode=1;profile-level-id=42e033",
	// 	},
	// 	PayloadType: 96,
	// }

	codec := webrtc.RTPCodecParameters{
		RTPCodecCapability: webrtc.RTPCodecCapability{
			MimeType:    "video/h264",
			ClockRate:   90000,
			SDPFmtpLine: "level-asymmetry-allowed=1;packetization-mode=1;profile-level-id=640c33",
		},
		PayloadType: 96,
	}
	if err := mediaEngine.RegisterCodec(codec, webrtc.RTPCodecTypeVideo); err != nil {
		log.Printf("[Go/Pion] Error registering H.264 codec: %v\n", err)
		// pcMutex.Unlock()
		return 0
	}

	// Ensure Opus audio is available
	if err := mediaEngine.RegisterCodec(webrtc.RTPCodecParameters{
		RTPCodecCapability: webrtc.RTPCodecCapability{
			MimeType:  webrtc.MimeTypeOpus,
			ClockRate: 48000,
			Channels:  2,
		},
		PayloadType: 111,
	}, webrtc.RTPCodecTypeAudio); err != nil {
		log.Printf("[Go/Pion] Error registering Opus codec: %v\n", err)
		return 0
	}

	if err := mediaEngine.RegisterDefaultCodecs(); err != nil {
		log.Printf("[Go/Pion] Error registering default codecs: %v\n", err)
		// pcMutex.Unlock()
		return 0
	}
	log.Println("[Go/Pion] createPeerConnectionGo: MediaEngine configured.")

	api := webrtc.NewAPI(webrtc.WithMediaEngine(mediaEngine), webrtc.WithInterceptorRegistry(i))

	config := webrtc.Configuration{
		ICEServers: []webrtc.ICEServer{
			{
				URLs: []string{"stun:stun.l.google.com:19302"},
			},
			{
				URLs:       []string{"turn:openrelay.metered.ca:80"},
				Username:   "openrelayproject",
				Credential: "openrelayproject",
			},
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
				// Throttle noisy logs: only log size and acks
				var messageData map[string]interface{}
				if err := json.Unmarshal(msg.Data, &messageData); err == nil {
					if clientSendTime, ok := messageData["client_send_time"].(float64); ok {
						// Normalize to milliseconds: if value looks like seconds, convert
						clientMs := clientSendTime
						if clientMs < 1e11 { // seconds-range epoch
							clientMs = clientMs * 1000.0
						}
						hostReceiveTime := float64(time.Now().UnixNano()) / float64(time.Millisecond)
						oneWayLatency := hostReceiveTime - clientMs
						// No per-message log; only significant latency spikes
						if oneWayLatency > 100 {
							log.Printf("[Go/Pion] Keyboard latency high: %.1f ms", oneWayLatency)
						}
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
				// Throttle noisy logs: only log size and acks
				var messageData map[string]interface{}
				if err := json.Unmarshal(msg.Data, &messageData); err == nil {
					if clientSendTime, ok := messageData["client_send_time"].(float64); ok {
						// Normalize to milliseconds: if value looks like seconds, convert
						clientMs := clientSendTime
						if clientMs < 1e11 { // seconds-range epoch
							clientMs = clientMs * 1000.0
						}
						hostReceiveTime := float64(time.Now().UnixNano()) / float64(time.Millisecond)
						oneWayLatency := hostReceiveTime - clientMs
						if oneWayLatency > 100 {
							log.Printf("[Go/Pion] Mouse latency high: %.1f ms", oneWayLatency)
						}
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
				/* log.Printf(
					"[Go/Pion] >>> DataChannel '%s' (ID: %s) OnMessage RECEIVED: %s",
					dc.Label(),
					idStr,
					string(msg.Data),
				) */
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
										// log.Printf("[Go/Pion] [PONG] RTT for frame %d: %.2f ms", frameID, rttMilli)
										lastRttMutex.Lock()
										lastRttMs = rttMilli
										lastRttMutex.Unlock()

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

	// Create video track (Sample based) with unified profile-level-id (42e033)
	// videoTrack, err = webrtc.NewTrackLocalStaticSample(
	// 	webrtc.RTPCodecCapability{MimeType: webrtc.MimeTypeH264, ClockRate: 90000, SDPFmtpLine: "level-asymmetry-allowed=1;packetization-mode=1;profile-level-id=42e033"},
	// 	"video",
	// 	"game-stream",
	// )

	videoTrack, err = webrtc.NewTrackLocalStaticSample(
		webrtc.RTPCodecCapability{MimeType: webrtc.MimeTypeH264, ClockRate: 90000, SDPFmtpLine: "level-asymmetry-allowed=1;packetization-mode=1;profile-level-id=640c33"},
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
	if _, err := peerConnection.AddTrack(videoTrack); err != nil {
		log.Printf("[Go/Pion] Error adding video track: %v\n", err)
		if pcErr := peerConnection.Close(); pcErr != nil {
			log.Printf("[Go/Pion] createPeerConnectionGo: Error closing PeerConnection after AddTrack failure: %v\n", pcErr)
		}
		peerConnection = nil
		pcMutex.Unlock()
		return 0
	}

	// Cap sender bitrate to ~5 Mbps to avoid wireless bursts
	// Disabled: this Pion version doesn't expose MaxBitrate/SetParameters on RTPSender.
	// Rely on encoder-side bitrate (set to ~5 Mbps) and congestion control.

	// Enumerate codecs and select H264 payload type specifically
	// Removed: previous code queried RTPSender params, which is unnecessary for TrackLocalStaticSample pacing.

	// Create and add Opus audio track
	audio, err := webrtc.NewTrackLocalStaticRTP(
		webrtc.RTPCodecCapability{MimeType: webrtc.MimeTypeOpus, ClockRate: 48000, Channels: 2},
		"audio",
		"game-audio",
	)
	if err != nil {
		log.Printf("[Go/Pion] Error creating audio track: %v\n", err)
	} else {
		if aSender, err2 := peerConnection.AddTrack(audio); err2 != nil {
			log.Printf("[Go/Pion] Error adding audio track: %v\n", err2)
		} else {
			audioTrack = audio
			aParams := aSender.GetParameters()
			if len(aParams.Encodings) > 0 {
				audioSSRC = uint32(aParams.Encodings[0].SSRC)
			}
			for _, c := range aParams.Codecs {
				if c.MimeType == webrtc.MimeTypeOpus {
					audioPayloadType = uint8(c.PayloadType)
				}
			}
			log.Printf("[Go/Pion] Audio track added. PT=%d SSRC=%d\n", audioPayloadType, audioSSRC)
		}
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
	// Periodic RTT anomaly monitor (5s)
	go func() {
		t := time.NewTicker(5 * time.Second)
		defer t.Stop()
		var lastSample float64
		for range t.C {
			pcMutex.Lock()
			pc := peerConnection
			pcMutex.Unlock()
			if pc == nil {
				continue
			}
			lastRttMutex.Lock()
			rtt := lastRttMs
			lastRttMutex.Unlock()
			if lastSample > 0 && rtt > 0 {
				if rtt > 2*lastSample && rtt > 50 { // spike detection
					log.Printf("[Go/Pion] RTT anomaly: %.1f ms -> %.1f ms", lastSample, rtt)
				}
			}
			lastSample = rtt
		}
	}()
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
	// Drain any remote ICE candidates that arrived early
	if len(pendingRemoteCandidates) > 0 {
		log.Printf("[Go/Pion] handleOffer: Adding %d buffered remote ICE candidates", len(pendingRemoteCandidates))
		for _, c := range pendingRemoteCandidates {
			if err := peerConnection.AddICECandidate(c); err != nil {
				log.Printf("[Go/Pion] Error adding buffered ICE candidate: %v", err)
			}
		}
		pendingRemoteCandidates = nil
	}

	answer, err := peerConnection.CreateAnswer(nil)
	if err != nil {
		log.Printf("[Go/Pion] Error creating answer: %v\n", err)
		return
	}
	// Munge SDP to advertise H.264 Level 5.1 for higher FPS at 1080p
	// answer.SDP = strings.ReplaceAll(answer.SDP, "profile-level-id=42e01f", "profile-level-id=42e033")
	// log.Println("[Go/Pion] handleOffer: Answer created successfully (munged to 42e033).")

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

//export freeCString
func freeCString(p *C.char) {
	C.free(unsafe.Pointer(p))
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
	if peerConnection.RemoteDescription() == nil {
		pendingRemoteCandidates = append(pendingRemoteCandidates, candidate)
		log.Println("[Go/Pion] Buffered ICE candidate (remote description not set yet)")
		return
	}
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
	if connectionState != webrtc.PeerConnectionStateConnected {
		return 0
	}

	// Reuse buffer from pool to avoid per-call allocation
	n := int(size)
	buf := getSampleBuf(n)
	C.memcpy(unsafe.Pointer(&buf[0]), data, C.size_t(n))
	// For testing pacing effects: write with zero duration (no pacing)
	if err := videoTrack.WriteSample(media.Sample{Data: buf, Duration: 0}); err != nil {
		return -1
	}
	// Return buffer to pool after write
	putSampleBuf(buf)

	// Debug: log send rate once per second (shared counters, disabled during streaming)
	sendCount++
	if time.Since(sendLastLog) >= time.Second {
		// log.Printf("[Pion] send samples/s: %d", sendCount)
		sendCount = 0
		sendLastLog = time.Now()
	}
	return 0
}

func splitNALUnits(buf []byte) [][]byte {
	// First, try Annex B start-code scanning
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

	if len(filteredNALs) > 0 {
		return filteredNALs
	}

	// Fallback: parse AVCC length-prefixed format (4-byte big-endian NAL size)
	var avccNALs [][]byte
	offset := 0
	for offset+4 <= len(buf) {
		size := int(buf[offset])<<24 | int(buf[offset+1])<<16 | int(buf[offset+2])<<8 | int(buf[offset+3])
		offset += 4
		if size <= 0 || offset+size > len(buf) {
			break
		}
		avccNALs = append(avccNALs, buf[offset:offset+size])
		offset += size
	}
	return avccNALs
}

func sendFragmentedNALUnit(nal []byte, maxPacketSize int, isLastNAL bool) error {
	// Deprecated with TrackLocalStaticSample; pacing handled by WriteSample on full frames.
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
		// Best-effort close DataChannels first to avoid races with SCTP shutdown
		if dataChannel != nil {
			_ = dataChannel.Close()
		}
		if mouseChannel != nil {
			_ = mouseChannel.Close()
		}
		if videoFeedbackChannel != nil {
			_ = videoFeedbackChannel.Close()
		}
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
	// if state != webrtc.PeerConnectionStateConnected {
	// 	log.Printf("[Go/Pion] PeerConnection state: %s\n", state.String())
	// }

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

//export SetRTCPCallback
func SetRTCPCallback(callback C.RTCPCallback) {
	rtcpCallback = callback
}

//export SetPLICallback
func SetPLICallback(callback C.OnPLICallback) {
	pliCallback = callback
}

func main() {
	// This main function is required for building as a C shared library,
	// but its contents are not directly executed when loaded as a DLL.
	// Initialization and cleanup are handled by initGo and closeGo.
	log.Println("[Go/Pion] main() in DLL. Not directly executed.")
}

// rtcpReaderFactory implements the interceptor.Factory interface
type rtcpReaderFactory struct{}

// NewInterceptor creates a new rtcpReaderInterceptor
func (f *rtcpReaderFactory) NewInterceptor(id string) (interceptor.Interceptor, error) {
	return &rtcpReaderInterceptor{}, nil
}

// rtcpReaderInterceptor implements the interceptor.Interceptor interface
type rtcpReaderInterceptor struct {
	// No embedded interface here. Implement all methods directly.
}

// BindRTCPWriter implements the interceptor.Interceptor interface.
func (r *rtcpReaderInterceptor) BindRTCPWriter(writer interceptor.RTCPWriter) interceptor.RTCPWriter {
	return writer
}

// BindRTCPReader wraps the RTCPReader to intercept incoming RTCP packets
func (r *rtcpReaderInterceptor) BindRTCPReader(reader interceptor.RTCPReader) interceptor.RTCPReader {
	return interceptor.RTCPReaderFunc(func(in []byte, a interceptor.Attributes) (n int, attr interceptor.Attributes, err error) {
		pkts, err := rtcp.Unmarshal(in)
		if err != nil {
			return reader.Read(in, a)
		}

		if a == nil {
			a = make(interceptor.Attributes)
		}

		for _, pkt := range pkts {
			switch p := pkt.(type) {
			case *rtcp.ReceiverReport:
				for _, report := range p.Reports {
					if rtcpCallback != nil {
						packetLoss := float64(report.FractionLost) / 256.0
						jitterSeconds := float64(report.Jitter) / 90000.0
						lastRttMutex.Lock()
						rttMs := lastRttMs
						lastRttMutex.Unlock()
						C.callRTCPCallback(rtcpCallback, C.double(packetLoss), C.double(rttMs), C.double(jitterSeconds))
					}
				}
			case *rtcp.PictureLossIndication:
				// If C.OnPLI is available, this will call into C++ to force IDR.
				// Otherwise, this is a no-op.
				if pliCallback != nil {
					C.callPLICallback(pliCallback)
				}
			case *rtcp.FullIntraRequest:
				if pliCallback != nil {
					C.callPLICallback(pliCallback)
				}
			}
		}
		return reader.Read(in, a)
	})
}

// BindLocalStream implements the Interceptor interface.
func (r *rtcpReaderInterceptor) BindLocalStream(info *interceptor.StreamInfo, writer interceptor.RTPWriter) interceptor.RTPWriter {
	return writer
}

// UnbindLocalStream implements the Interceptor interface.
func (r *rtcpReaderInterceptor) UnbindLocalStream(info *interceptor.StreamInfo) {
	// No-op
}

// BindRemoteStream implements the Interceptor interface.
func (r *rtcpReaderInterceptor) BindRemoteStream(info *interceptor.StreamInfo, reader interceptor.RTPReader) interceptor.RTPReader {
	return reader
}

// UnbindRemoteStream implements the Interceptor interface.
func (r *rtcpReaderInterceptor) UnbindRemoteStream(info *interceptor.StreamInfo) {
	// No-op
}

// Close implements the Interceptor interface.
func (r *rtcpReaderInterceptor) Close() error {
	return nil
}
