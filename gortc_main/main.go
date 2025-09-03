//go:build !debug
// +build !debug

package main

/*
#cgo CFLAGS: -I.
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

// Enhanced WebRTC stats callback for comprehensive monitoring
typedef void (*WebRTCStatsCallback)(double packetLoss, double rtt, double jitter,
                                   unsigned int nackCount, unsigned int pliCount, unsigned int twccCount,
                                   unsigned int pacerQueueLength, unsigned int sendBitrateKbps);

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

// Helper function to call the enhanced WebRTC stats callback
static inline void callWebRTCStatsCallback(WebRTCStatsCallback f, double p, double r, double j,
                                          unsigned int nack, unsigned int pli, unsigned int twcc,
                                          unsigned int queueLen, unsigned int bitrate) {
    if (f) {
        f(p, r, j, nack, pli, twcc, queueLen, bitrate);
    }
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
	"os"
	"strconv"
	"sync"
	"sync/atomic"
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
var webrtcStatsCallback C.WebRTCStatsCallback

// Comprehensive WebRTC stats tracking
var webrtcStats struct {
	nackCount        uint32
	pliCount         uint32
	twccCount        uint32
	pacerQueueLength uint32
	sendBitrateKbps  uint32
	lastStatsUpdate  time.Time
	statsMutex       sync.RWMutex
}

// Periodic stats monitoring goroutine
func startStatsMonitoring() {
	go func() {
		ticker := time.NewTicker(100 * time.Millisecond) // Update every 100ms
		defer ticker.Stop()

		for {
			select {
			case <-ticker.C:
				updatePacerQueueLength()
			}
		}
	}()
}

// Estimate pacer queue length based on send queue depths and timing
func updatePacerQueueLength() {
	// Estimate based on video send queue depth
	videoQueueLen := uint32(len(videoSendQueue))

	// For audio queue, we can't directly check length due to channel semantics
	// In a real implementation, we'd maintain a separate counter or use Pion's stats
	audioQueueLen := uint32(0) // Simplified - assume minimal audio queuing

	// Estimate total pacer queue length
	// In a real implementation, this would come from Pion's internal pacer stats
	estimatedQueueLen := videoQueueLen + audioQueueLen

	// Estimate send bitrate (rough calculation based on queue growth)
	// This is a simplified estimation - real implementation would use actual bitrate
	// For now, use a more realistic estimation based on video frame rate
	estimatedBitrate := uint32(35000) // 35 Mbps baseline for 200fps video
	if estimatedQueueLen > 1 {
		// If queue is building up, network might be congested
		// Calculate reduction factor based on queue length
		reductionFactor := 1.0 - float64(estimatedQueueLen)/10.0
		if reductionFactor < 0.1 { // Don't go below 10% of original bitrate
			reductionFactor = 0.1
		}
		estimatedBitrate = uint32(float64(estimatedBitrate) * reductionFactor)
	}

	webrtcStats.statsMutex.Lock()
	webrtcStats.pacerQueueLength = estimatedQueueLen
	webrtcStats.sendBitrateKbps = estimatedBitrate
	webrtcStats.statsMutex.Unlock()
}

// Global variables
var (
	peerConnection        *webrtc.PeerConnection
	pcMutex               sync.Mutex
	audioMutex            sync.Mutex                     // Separate mutex for audio RTP state to reduce contention
	videoTrack            *webrtc.TrackLocalStaticSample // switched to sample track for pacing
	audioTrack            *webrtc.TrackLocalStaticRTP
	trackSSRC             uint32
	audioSSRC             uint32
	lastAnswerSDP         string
	currentSequenceNumber uint16
	currentTimestamp      uint32
	currentAudioSeq       uint16
	currentAudioTS        uint32
	// RTP timestamp baseline tracking for consistent frame deltas
	audioRTPBaseline     uint32       // Initial RTP timestamp established from first PTS
	audioPTSBaseline     int64        // Initial PTS value for reference
	audioBaselineSet     bool         // Whether baseline has been established
	audioFrameDuration   uint32 = 480 // RTP timestamp increment per frame (10ms at 48kHz)
	videoFrameCounter    uint64
	dataChannel          *webrtc.DataChannel
	messageQueue         []string
	mouseQueue           []string
	queueMutex           sync.Mutex
	mouseChannel         *webrtc.DataChannel
	latencyChannel       *webrtc.DataChannel
	videoFeedbackChannel *webrtc.DataChannel
	pingTimestamps       map[uint64]int64
	pingTimestampsMutex  sync.Mutex
	connectionState      webrtc.PeerConnectionState
	videoPayloadType     uint8 = 96
	audioPayloadType     uint8 = 111

	// Audio E2E latency measurement
	audioPingCounter    uint64
	audioPingTimestamps map[uint64]int64 // audioPingID -> hostSendTime
	audioPingMutex      sync.Mutex
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
	sendLastLog       time.Time
	sendCount         int
	zeroDurationCount int

	// Granular audio send path: bounded queue and dedicated sender goroutine
	audioSendQueue chan *rtp.Packet // Bounded channel for RTP packets (size ≤ 1)
	audioSendStop  chan struct{}    // Stop signal for sender goroutine

	// Granular video send path: bounded queue and dedicated sender goroutine
	videoSendQueue chan media.Sample // Bounded channel for video samples (size ≤ 2)
	videoSendStop  chan struct{}     // Stop signal for video sender goroutine

	// Buffer completion mechanism to prevent use-after-free
	audioBufferCompletion chan []byte // Channel to signal buffer completion
	videoBufferCompletion chan []byte // Channel to signal video buffer completion
)

// AudioRTPState encapsulates all audio RTP state with atomic operations
// This minimizes contention with control-plane operations and provides lock-free media writes
type AudioRTPState struct {
	sequence    uint32       // Atomic sequence number (uint32 for easier atomic ops)
	timestamp   uint32       // Atomic RTP timestamp
	rtpBaseline uint32       // RTP baseline for wraparound handling
	ptsBaseline int64        // PTS baseline for reference
	baselineSet int32        // Atomic flag (0=false, 1=true)
	mutex       sync.RWMutex // Minimal mutex for baseline setup (read-mostly)
}

// Global audio RTP state instance
var audioRTPState = &AudioRTPState{}

// GetNextSequence atomically increments and returns the next sequence number
func (s *AudioRTPState) GetNextSequence() uint16 {
	seq := atomic.AddUint32(&s.sequence, 1)
	return uint16(seq - 1) // Return the value before increment
}

// GetNextTimestamp atomically increments and returns the next timestamp
func (s *AudioRTPState) GetNextTimestamp() uint32 {
	ts := atomic.AddUint32(&s.timestamp, audioFrameDuration)
	return ts - audioFrameDuration // Return the value before increment
}

// IsBaselineSet atomically checks if baseline is set
func (s *AudioRTPState) IsBaselineSet() bool {
	return atomic.LoadInt32(&s.baselineSet) != 0
}

// SetBaseline atomically sets the RTP baseline from PTS
func (s *AudioRTPState) SetBaseline(pts int64) {
	s.mutex.Lock()
	defer s.mutex.Unlock()

	// Convert microseconds to RTP samples: PTS_us * 48_samples/ms / 1000_us/ms
	rtpBaseline := uint32((pts * 48) / 1000)

	atomic.StoreUint32(&s.rtpBaseline, rtpBaseline)
	atomic.StoreInt64(&s.ptsBaseline, pts)
	atomic.StoreUint32(&s.timestamp, rtpBaseline)
	atomic.StoreInt32(&s.baselineSet, 1)

	log.Printf("[Go/Pion] Audio RTP baseline established: PTS=%d us -> RTP=%d (48kHz clock, %d samples/frame)",
		pts, rtpBaseline, audioFrameDuration)
}

// GetBaseline atomically gets the current RTP baseline
func (s *AudioRTPState) GetBaseline() uint32 {
	return atomic.LoadUint32(&s.rtpBaseline)
}

// Reset atomically resets all RTP state
func (s *AudioRTPState) Reset() {
	s.mutex.Lock()
	defer s.mutex.Unlock()

	atomic.StoreUint32(&s.sequence, 0)
	atomic.StoreUint32(&s.timestamp, 0)
	atomic.StoreUint32(&s.rtpBaseline, 0)
	atomic.StoreInt64(&s.ptsBaseline, 0)
	atomic.StoreInt32(&s.baselineSet, 0)
}

// Optimized tiered buffer pool system for media samples
// This system reduces memory fragmentation and allocation churn by:
// 1. Preallocating buffers for common sizes (128, 256, 512, 1500 bytes)
// 2. Using separate pools for each size tier to minimize fragmentation
// 3. Only pooling buffers that match tier sizes exactly
// 4. Tracking hit rates and allocation patterns for optimization
//
// Size tiers are chosen based on typical media payload sizes:
// - 128 bytes: Small audio frames (Opus low bitrate)
// - 256 bytes: Medium audio frames (Opus medium bitrate)
// - 512 bytes: Large audio frames (Opus high bitrate)
// - 1500 bytes: Video frames and max network MTU
//
// Benefits:
// - Reduces GC pressure by reusing buffers
// - Eliminates memory fragmentation from variable-sized allocations
// - Provides predictable memory usage patterns
// - Maintains high cache hit rates for common sizes
type tieredBufferPool struct {
	pools       [8]sync.Pool // Pools for different size tiers
	sizes       [8]int       // Size classes: 128, 256, 512, 1500, 4096, 8192, 16384, 32768
	sizeCount   int
	hits        [8]int64     // Cache hits per tier
	misses      [8]int64     // Cache misses per tier
	allocations [8]int64     // New allocations per tier
	mutex       sync.RWMutex // Protects statistics
}

var sampleBufPool = &tieredBufferPool{
	sizes:     [8]int{128, 256, 512, 1500, 4096, 8192, 16384, 32768},
	sizeCount: 8,
}

// Initialize preallocated buffers for common sizes
func initBufferPool() {
	// Preallocate buffers for each size tier
	// Larger tiers (video) get fewer preallocated buffers to save memory
	// Smaller tiers (audio/metadata) get more for better cache performance
	preallocCounts := [8]int{10, 10, 8, 6, 4, 3, 2, 2} // Prealloc counts per tier

	for i, size := range sampleBufPool.sizes {
		count := preallocCounts[i]
		for j := 0; j < count; j++ {
			buf := make([]byte, size)
			sampleBufPool.pools[i].Put(buf)
		}
		log.Printf("[Go/Pion] Buffer pool tier %d (%d bytes): preallocated %d buffers", i, size, count)
	}
	log.Println("[Go/Pion] Buffer pool initialized with tiered preallocation")

	// Start periodic buffer pool statistics logging
	go func() {
		ticker := time.NewTicker(5 * time.Minute) // Log every 5 minutes
		defer ticker.Stop()

		for range ticker.C {
			logBufferPoolStats()
		}
	}()
}

// getBufferTier returns the appropriate tier index for a given size
func (tbp *tieredBufferPool) getBufferTier(size int) int {
	for i, tierSize := range tbp.sizes {
		if size <= tierSize {
			return i
		}
	}
	// If size is larger than all tiers, use the largest tier
	return tbp.sizeCount - 1
}

// getSampleBuf returns a buffer of at least the requested size
func getSampleBuf(n int) []byte {
	if n <= 0 {
		return make([]byte, 0)
	}

	// Find the appropriate size tier
	tier := sampleBufPool.getBufferTier(n)
	// targetSize := sampleBufPool.sizes[tier]

	// Try to get a buffer from the appropriate tier
	v := sampleBufPool.pools[tier].Get()
	if v == nil {
		// No buffer available in pool, allocate new one
		sampleBufPool.mutex.Lock()
		sampleBufPool.misses[tier]++
		sampleBufPool.allocations[tier]++
		sampleBufPool.mutex.Unlock()
		return make([]byte, n)
	}

	b := v.([]byte)
	if cap(b) < n {
		// Buffer too small, allocate new one
		sampleBufPool.mutex.Lock()
		sampleBufPool.misses[tier]++
		sampleBufPool.allocations[tier]++
		sampleBufPool.mutex.Unlock()
		return make([]byte, n)
	}

	// Cache hit - buffer available and suitable
	sampleBufPool.mutex.Lock()
	sampleBufPool.hits[tier]++
	sampleBufPool.mutex.Unlock()

	// Return slice of requested size
	return b[:n]
}

// putSampleBuf returns a buffer to the appropriate pool
func putSampleBuf(b []byte) {
	if b == nil || cap(b) == 0 {
		return
	}

	capacity := cap(b)

	// Find the appropriate tier for this buffer capacity
	tier := sampleBufPool.getBufferTier(capacity)
	targetSize := sampleBufPool.sizes[tier]

	// Only pool buffers that match our tier sizes exactly
	// This ensures consistent reuse and reduces fragmentation
	if capacity == targetSize {
		// Reset the buffer content and return to pool
		// Clear the slice to prevent data leaks between uses
		for i := range b {
			b[i] = 0
		}
		sampleBufPool.pools[tier].Put(b[:capacity])
	}
	// If buffer doesn't match a tier size, let it be garbage collected
}

// logBufferPoolStats logs buffer pool usage statistics for monitoring
func logBufferPoolStats() {
	sampleBufPool.mutex.RLock()
	defer sampleBufPool.mutex.RUnlock()

	log.Printf("[Go/Pion] Buffer Pool Statistics:")
	totalHits := int64(0)
	totalMisses := int64(0)
	totalAllocs := int64(0)

	for i, size := range sampleBufPool.sizes {
		hits := sampleBufPool.hits[i]
		misses := sampleBufPool.misses[i]
		allocs := sampleBufPool.allocations[i]
		totalHits += hits
		totalMisses += misses
		totalAllocs += allocs

		total := hits + misses
		hitRate := float64(0)
		if total > 0 {
			hitRate = float64(hits) / float64(total) * 100
		}

		log.Printf("[Go/Pion]   Tier %d (%d bytes): %d hits, %d misses, %d allocs (%.1f%% hit rate)",
			i, size, hits, misses, allocs, hitRate)
	}

	totalRequests := totalHits + totalMisses
	overallHitRate := float64(0)
	if totalRequests > 0 {
		overallHitRate = float64(totalHits) / float64(totalRequests) * 100
	}

	log.Printf("[Go/Pion]   Overall: %d requests, %d allocations, %.1f%% hit rate",
		totalRequests, totalAllocs, overallHitRate)
}

// initAudioSendQueue initializes the bounded audio send queue and starts the sender goroutine
func initAudioSendQueue() {
	// Create bounded channel with capacity 1 (as requested: size ≤ 1)
	audioSendQueue = make(chan *rtp.Packet, 1)
	audioSendStop = make(chan struct{})

	// Create buffer completion channel for safe buffer pool management
	audioBufferCompletion = make(chan []byte, 16) // Small buffer for completion signals

	// Start the dedicated audio sender goroutine
	go audioSenderGoroutine()

	// Start the buffer completion handler goroutine
	go audioBufferCompletionHandler()
}

// initVideoSendQueue initializes the bounded video send queue and starts the sender goroutine
func initVideoSendQueue() {
	// Create bounded channel with capacity 2 (for video: size ≤ 2)
	// Slightly larger than audio to handle frame reordering and prevent drops
	videoSendQueue = make(chan media.Sample, 2)
	videoSendStop = make(chan struct{})

	// Create buffer completion channel for safe buffer pool management
	// Increased buffer size to prevent blocking at high throughput
	videoBufferCompletion = make(chan []byte, 64) // Larger buffer for completion signals

	// Start the dedicated video sender goroutine
	go videoSenderGoroutine()

	// Start the buffer completion handler goroutine
	go videoBufferCompletionHandler()

	log.Println("[Go/Pion] Audio send queue initialized with bounded channel (capacity: 1)")
	log.Println("[Go/Pion] Video send queue initialized with bounded channel (capacity: 2)")
	log.Println("[Go/Pion] Buffer completion mechanism initialized for use-after-free prevention")
}

// audioBufferCompletionHandler safely manages buffer pool returns to prevent use-after-free
// This goroutine ensures buffers are only returned to the pool after WriteRTP has finished reading them
func audioBufferCompletionHandler() {
	log.Println("[Go/Pion] Audio buffer completion handler started")

	completionCount := 0
	startTime := time.Now()

	for {
		select {
		case buffer := <-audioBufferCompletion:
			// Safe to return buffer to pool now that WriteRTP has finished with it
			putSampleBuf(buffer)
			completionCount++

			// Periodic logging (every 1000 completions)
			if completionCount%1000 == 0 {
				elapsed := time.Since(startTime)
				rate := float64(completionCount) / elapsed.Seconds()
				log.Printf("[Go/Pion] Buffer completion: %d buffers processed (%.1f buffers/sec)",
					completionCount, rate)
			}

		case <-time.After(5 * time.Second):
			// Periodic health check
			if len(audioBufferCompletion) > 8 { // More than half capacity
				log.Printf("[Go/Pion] Buffer completion queue getting full: %d/%d",
					len(audioBufferCompletion), cap(audioBufferCompletion))
			}

		case <-audioSendStop:
			log.Printf("[Go/Pion] Audio buffer completion handler stopped after processing %d buffers", completionCount)
			return
		}
	}
}

// audioSenderGoroutine runs in a separate goroutine to send RTP packets without holding locks
// This prevents head-of-line blocking and keeps the send path lock-granular
func audioSenderGoroutine() {
	log.Println("[Go/Pion] Audio sender goroutine started")

	for {
		select {
		case pkt := <-audioSendQueue:
			// Send RTP packet without holding any locks
			// This is the potentially blocking operation, but it doesn't block other operations
			if audioTrack != nil {
				if err := audioTrack.WriteRTP(pkt); err != nil {
					log.Printf("[Go/Pion] Error in audio sender goroutine: %v", err)
				}
			}

			// Signal buffer completion to prevent use-after-free
			// This ensures the buffer is only returned to the pool after WriteRTP has finished reading it
			if len(pkt.Payload) > 0 {
				// Try to signal completion with fallback mechanisms
				select {
				case audioBufferCompletion <- pkt.Payload:
					// Buffer completion signaled successfully - normal path
				case <-time.After(200 * time.Millisecond):
					// Completion channel is full or handler is slow
					// Check if completion handler is still running and try a shorter timeout
					select {
					case audioBufferCompletion <- pkt.Payload:
						// Retry succeeded
						log.Printf("[Go/Pion] Buffer completion retry succeeded (seq=%d)",
							pkt.Header.SequenceNumber)
					case <-time.After(50 * time.Millisecond):
						// Still failing - force return to pool to prevent memory leaks
						log.Printf("[Go/Pion] Buffer completion failed after retry, forcing pool return (seq=%d)",
							pkt.Header.SequenceNumber)
						putSampleBuf(pkt.Payload)
					}
				}
			}

		case <-audioSendStop:
			log.Println("[Go/Pion] Audio sender goroutine stopped")
			return
		}
	}
}

// videoSenderGoroutine runs in a separate goroutine to send video samples without holding locks
// This prevents head-of-line blocking and keeps the video send path lock-granular
func videoSenderGoroutine() {
	log.Println("[Go/Pion] Video sender goroutine started")

	// Watchdog to detect if we're not consuming samples fast enough
	lastSampleTime := time.Now()
	sampleCount := 0

	// Start watchdog goroutine
	watchdogStop := make(chan struct{})
	go func() {
		ticker := time.NewTicker(1 * time.Second)
		defer ticker.Stop()

		for {
			select {
			case <-ticker.C:
				if sampleCount == 0 && time.Since(lastSampleTime) > 2*time.Second {
					log.Printf("[Go/Pion] Video sender watchdog: no samples processed in 2+ seconds")
				}
				sampleCount = 0 // Reset counter
			case <-watchdogStop:
				return
			}
		}
	}()
	defer close(watchdogStop)

	for {
		select {
		case sample := <-videoSendQueue:
			// Update watchdog counters
			lastSampleTime = time.Now()
			sampleCount++

			// Send video sample without holding any locks
			// This is the potentially blocking operation, but it doesn't block other operations
			if videoTrack != nil {
				if err := videoTrack.WriteSample(sample); err != nil {
					log.Printf("[Go/Pion] Error in video sender goroutine: %v", err)
				}
			} else {
				log.Printf("[Go/Pion] Video track is nil, dropping sample")
			}

			// Signal buffer completion to prevent use-after-free
			// This ensures the buffer is only returned to the pool after WriteSample has finished reading it
			if len(sample.Data) > 0 {
				// Try to signal completion with non-blocking approach
				select {
				case videoBufferCompletion <- sample.Data:
					// Buffer completion signaled successfully - normal path
				default:
					// Completion channel is full - use goroutine to avoid blocking
					go func(buf []byte) {
						select {
						case videoBufferCompletion <- buf:
							// Eventually succeeded
						case <-time.After(100 * time.Millisecond):
							// Timeout - force return to prevent leaks
							log.Printf("[Go/Pion] Video buffer completion timeout, forcing pool return (size=%d)",
								len(buf))
							putSampleBuf(buf)
						}
					}(sample.Data)
				}
			}

		case <-videoSendStop:
			log.Println("[Go/Pion] Video sender goroutine stopped")
			return

		case <-time.After(10 * time.Second):
			// Safety timeout to prevent goroutine from hanging indefinitely
			log.Printf("[Go/Pion] Video sender timeout - no activity for 10 seconds, checking health...")
			if videoTrack == nil {
				log.Printf("[Go/Pion] Video track is nil during timeout")
			}
			// Continue processing - this timeout just prevents indefinite blocking
		}
	}
}

// videoBufferCompletionHandler safely manages buffer pool returns to prevent use-after-free
// This goroutine ensures buffers are only returned to the pool after WriteSample has finished reading them
func videoBufferCompletionHandler() {
	log.Println("[Go/Pion] Video buffer completion handler started")

	bufferCount := 0
	lastLogTime := time.Now()

	for {
		select {
		case buf := <-videoBufferCompletion:
			// Return buffer to pool now that WriteSample has finished reading it
			putSampleBuf(buf)
			bufferCount++

			// Log buffer processing rate occasionally
			if time.Since(lastLogTime) >= 5*time.Second {
				log.Printf("[Go/Pion] Video buffer completion: %d buffers processed (%.1f buffers/sec)",
					bufferCount, float64(bufferCount)/5.0)
				bufferCount = 0
				lastLogTime = time.Now()
			}

		case <-videoSendStop:
			log.Println("[Go/Pion] Video buffer completion handler stopped")
			return
		}
	}
}

// testBufferPool performs a simple test of buffer pool functionality
// This can be called during development to verify the pool is working correctly
func testBufferPool() {
	log.Println("[Go/Pion] Testing buffer pool functionality...")

	// Test different buffer sizes
	testSizes := []int{50, 100, 150, 200, 300, 600, 1000, 1400}

	for _, size := range testSizes {
		// Get a buffer
		buf := getSampleBuf(size)

		// Verify buffer size
		if len(buf) < size {
			log.Printf("[Go/Pion] ERROR: Requested size %d, got %d", size, len(buf))
			continue
		}

		// Fill buffer with test data
		for i := range buf {
			buf[i] = byte(i % 256)
		}

		// Return buffer to pool
		putSampleBuf(buf)

		log.Printf("[Go/Pion] Tested buffer size %d bytes successfully", size)
	}

	// Log final statistics
	logBufferPoolStats()
	log.Println("[Go/Pion] Buffer pool test completed")
}

// testBufferCompletionMechanism tests the use-after-free prevention system
// This can be called during development to verify buffer safety
func testBufferCompletionMechanism() {
	log.Println("[Go/Pion] Testing buffer completion mechanism...")

	if audioBufferCompletion == nil {
		log.Println("[Go/Pion] Buffer completion channel not initialized")
		return
	}

	// Test buffer completion signaling
	testBuffer := getSampleBuf(512)
	// testSequence := 0

	// Simulate sending a packet and signaling completion
	select {
	case audioBufferCompletion <- testBuffer:
		log.Println("[Go/Pion] Buffer completion test: signal sent successfully")
	case <-time.After(100 * time.Millisecond):
		log.Println("[Go/Pion] Buffer completion test: timeout (this may indicate issues)")
		putSampleBuf(testBuffer) // Fallback
	}

	// Wait a moment for completion handler to process
	time.Sleep(50 * time.Millisecond)

	log.Println("[Go/Pion] Buffer completion mechanism test completed")
}

func init() {
	rand.Seed(time.Now().UnixNano())
	initBufferPool()
	initAudioSendQueue()
	initVideoSendQueue()
	startStatsMonitoring()
}

// validateAudioTimestampStability checks RTP timestamp progression for debugging
// This function validates that RTP timestamps follow the running increment pattern
func validateAudioTimestampStability() {
	if !audioRTPState.IsBaselineSet() {
		return
	}

	// Get current RTP state atomically
	currentSeq := atomic.LoadUint32(&audioRTPState.sequence)
	currentTS := atomic.LoadUint32(&audioRTPState.timestamp)
	baseline := audioRTPState.GetBaseline()

	// Calculate expected RTP timestamp based on sequence number and frame duration
	// This should match current timestamp if the running increment is working correctly
	expectedRTP := baseline + (currentSeq * audioFrameDuration)

	// Check for significant deviations from expected running increment
	rtpDiff := int64(currentTS) - int64(expectedRTP)
	if rtpDiff > 1000 || rtpDiff < -1000 { // Allow 1000 sample tolerance for timing variations
		log.Printf("[Go/Pion] RTP timestamp stability WARNING: seq=%d, expected=%d, actual=%d, diff=%d samples",
			currentSeq, expectedRTP, currentTS, rtpDiff)
	} else {
		// Only log occasionally to avoid spam
		if currentSeq%5000 == 0 {
			log.Printf("[Go/Pion] RTP timestamp stability OK: seq=%d, rtp=%d, baseline=%d, increment=%d samples",
				currentSeq, currentTS, baseline, audioFrameDuration)
		}
	}
}

//export sendAudioPacket
func sendAudioPacket(data unsafe.Pointer, size C.int, pts C.longlong) C.int {
	// Non-blocking audio RTP write implementation
	// Uses granular locking to reduce contention:
	// 1. Minimal global lock (pcMutex) for connection state checks only
	// 2. Dedicated audio mutex for RTP state (sequence, timestamp, baseline)
	// 3. No locks held during WriteRTP() - eliminates stalls from blocking I/O
	// This prevents audio writes from blocking control operations and vice versa

	// Check connection state and track availability with minimal lock time
	pcMutex.Lock()
	if peerConnection == nil || audioTrack == nil {
		pcMutex.Unlock()
		return -1
	}
	if connectionState != webrtc.PeerConnectionStateConnected {
		pcMutex.Unlock()
		return 0
	}

	// Get a copy of the track pointer and other shared state while holding the lock
	// track := audioTrack
	payloadType := audioPayloadType
	ssrc := audioSSRC
	pcMutex.Unlock() // Release global lock immediately

	// Reuse buffer from pool to avoid per-call allocation
	n := int(size)
	payload := getSampleBuf(n)
	C.memcpy(unsafe.Pointer(&payload[0]), data, C.size_t(n))

	// RTP Timestamp Management: Lock-free atomic operations
	// This eliminates contention with control-plane operations and provides
	// predictable packet timing for jitter buffers with stable inter-packet intervals

	// Check if baseline needs to be established (first packet)
	if !audioRTPState.IsBaselineSet() {
		// Initialize RTP baseline from first PTS to avoid wraparound issues
		audioRTPState.SetBaseline(int64(pts))
	}

	// Get next sequence and timestamp atomically (lock-free)
	packetSequence := audioRTPState.GetNextSequence()
	packetRTPTimestamp := audioRTPState.GetNextTimestamp()

	// Handle RTP timestamp wraparound (uint32 wraps at ~13.27 hours at 48kHz)
	if packetRTPTimestamp < audioRTPState.GetBaseline() {
		// This is a rare event - log it and handle gracefully
		log.Printf("[Go/Pion] RTP timestamp wraparound detected: baseline=%d, timestamp=%d",
			audioRTPState.GetBaseline(), packetRTPTimestamp)
		// Note: The AudioRTPState handles wraparound internally in GetNextTimestamp
	}

	// Check if we should insert an audio ping marker (every 100 packets)
	isAudioPing := (uint32(packetSequence) % 100) == 0
	var audioPingID uint64

	if isAudioPing {
		audioPingMutex.Lock()
		audioPingCounter++
		audioPingID = audioPingCounter
		audioPingTimestamps[audioPingID] = time.Now().UnixNano()
		audioPingMutex.Unlock()

		// Send audio ping via data channel
		if latencyChannel != nil && latencyChannel.ReadyState() == webrtc.DataChannelStateOpen {
			hostSendTime := time.Now().UnixNano()
			audioPingMutex.Lock()
			audioPingTimestamps[audioPingID] = hostSendTime
			audioPingMutex.Unlock()

			pingMessage := map[string]interface{}{
				"type":           "audio_ping",
				"ping_id":        audioPingID,
				"host_send_time": fmt.Sprintf("%d", hostSendTime),
				"sequence":       packetSequence,
				"timestamp":      packetRTPTimestamp,
			}
			if pingJSON, err := json.Marshal(pingMessage); err == nil {
				_ = latencyChannel.SendText(string(pingJSON))
			}
		}
	}

	// No mutex release needed - atomic operations are lock-free!

	// Create RTP packet with captured timestamp and sequence
	pkt := &rtp.Packet{
		Header: rtp.Header{
			Version:        2,
			PayloadType:    payloadType,
			SequenceNumber: packetSequence,
			Timestamp:      packetRTPTimestamp, // RTP timestamp for this specific packet
			SSRC:           ssrc,
			Marker:         true, // Mark each audio frame boundary for better jitter buffer behavior
		},
		Payload: payload,
	}

	// Queue RTP packet for the dedicated sender goroutine (bounded queue, size ≤ 1)
	// This implements backpressure by dropping oldest packets rather than accumulating
	// The sender goroutine will handle WriteRTP without holding any locks

	select {
	case audioSendQueue <- pkt:
		// Packet successfully queued for sending
		// RTP timestamp is maintained as: currentAudioTS = last_queued_packet_timestamp
		// Next packet will use: currentAudioTS + audioFrameDuration
		// This ensures stable inter-packet intervals and smooth sender timing

		// Optional performance logging (disabled during normal operation to avoid spam)
		// To enable performance monitoring, uncomment the following block:
		// if packetSequence%1000 == 0 {
		//     log.Printf("[Go/Pion] Audio RTP queued: seq=%d, ts=%d", packetSequence, packetRTPTimestamp)
		// }

		// Note: Buffer will be returned to pool by sender goroutine after WriteRTP
		return 0

	default:
		// Queue is full - implement backpressure by dropping the oldest packet
		// This prevents head-of-line blocking and keeps latency bounded
		select {
		case oldestPkt := <-audioSendQueue:
			// Successfully removed oldest packet, now queue the new one
			audioSendQueue <- pkt
			log.Printf("[Go/Pion] Audio backpressure: dropped oldest packet (seq=%d), queued new (seq=%d)",
				oldestPkt.Header.SequenceNumber, packetSequence)
			// Note: Buffer will be returned to pool by sender goroutine after WriteRTP
			return 0
		default:
			// This should not happen with bounded channel, but handle gracefully
			log.Printf("[Go/Pion] Audio queue unexpectedly full, dropping packet (seq=%d)", packetSequence)
			putSampleBuf(payload)
			return -1
		}
	}
}

//export sendVideoSample
func sendVideoSample(data unsafe.Pointer, size C.int, durationUs C.longlong) C.int {
	// Check connection state and track availability with minimal lock time
	pcMutex.Lock()
	if peerConnection == nil || videoTrack == nil {
		pcMutex.Unlock()
		return -1
	}
	if connectionState != webrtc.PeerConnectionStateConnected {
		pcMutex.Unlock()
		return 0
	}

	// Get a copy of the track pointer and other shared state while holding the lock
	// track := videoTrack
	feedbackChannel := videoFeedbackChannel
	pcMutex.Unlock() // Release global lock immediately

	// Validate duration for proper pacing
	durationValue := int64(durationUs)
	if !validateVideoDuration(durationValue) {
		return -3 // Distinct error code for invalid duration
	}

	// Reuse buffer from pool to avoid per-call allocation
	n := int(size)
	buf := getSampleBuf(n)
	C.memcpy(unsafe.Pointer(&buf[0]), data, C.size_t(n))
	dur := time.Duration(durationValue) * time.Microsecond

	// Create video sample for queuing
	sample := media.Sample{Data: buf, Duration: dur}

	// Queue sample for the dedicated sender goroutine (bounded queue, size ≤ 2)
	// This implements backpressure by dropping oldest samples rather than accumulating
	// The sender goroutine will handle WriteSample without holding any locks

	// Try to send sample with timeout to prevent blocking
	select {
	case videoSendQueue <- sample:
		// Sample successfully queued for sending
		// Buffer will be returned to pool by sender goroutine after WriteSample
		return 0
	case <-time.After(10 * time.Millisecond):
		// Queue is full or sender is slow - implement backpressure
		// Try to make room by dropping oldest sample
		select {
		case oldestSample := <-videoSendQueue:
			// Successfully removed oldest sample
			putSampleBuf(oldestSample.Data)

			// Now try to queue the new sample
			select {
			case videoSendQueue <- sample:
				// Success after making room
				log.Printf("[Go/Pion] Video backpressure: dropped oldest sample (size=%d), queued new (size=%d)",
					len(oldestSample.Data), n)
				return 0
			case <-time.After(5 * time.Millisecond):
				// Still can't queue - sender might be stuck
				putSampleBuf(buf)
				log.Printf("[Go/Pion] Video sender stuck, dropped sample (size=%d)", n)
				return -1
			}
		case <-time.After(5 * time.Millisecond):
			// Can't even read from queue - sender might be completely stuck
			putSampleBuf(buf)
			log.Printf("[Go/Pion] Video queue completely stuck, dropped sample (size=%d)", n)
			return -1
		}
	}

	// Debug: log send rate once per second (disabled during streaming)
	sendCount++
	if time.Since(sendLastLog) >= time.Second {
		// log.Printf("[Pion] send samples/s: %d", sendCount)
		sendCount = 0
		sendLastLog = time.Now()
	}

	// Optional RTT ping to client (unchanged):
	if feedbackChannel != nil && feedbackChannel.ReadyState() == webrtc.DataChannelStateOpen {
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
			_ = feedbackChannel.SendText(string(pingJSON))
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
		pingTimestamps = make(map[uint64]int64)      // Initialize/clear the map
		audioPingTimestamps = make(map[uint64]int64) // Initialize/clear audio ping map
		audioPingCounter = 0                         // Reset audio ping counter
		// Reset audio RTP state for new connection (atomic, lock-free)
		audioRTPState.Reset()
		log.Println("[Go/Pion] Audio RTP state reset for new connection")
		log.Println(
			"[Go/Pion] createPeerConnectionGo: Closed previous PeerConnection and reset state.",
		)
	} else {
		// This is the first run, initialize the map.
		pingTimestamps = make(map[uint64]int64)
		audioPingTimestamps = make(map[uint64]int64) // Initialize audio ping map
		audioPingCounter = 0                         // Initialize audio ping counter
		// Initialize audio RTP state for first connection (atomic, lock-free)
		audioRTPState.Reset()
		log.Println("[Go/Pion] Audio RTP state initialized for first connection")
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
					if clientMs, ok := normalizeToMs(messageData["client_send_time"]); ok {
						hostReceiveTime := float64(time.Now().UnixNano()) / float64(time.Millisecond)
						oneWayLatency := hostReceiveTime - clientMs
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
					if clientMs, ok := normalizeToMs(messageData["client_send_time"]); ok {
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
							} else if msgType == "audio_ping_pong" {
								if pingIDFloat, ok := messageData["ping_id"].(float64); ok {
									pingID := uint64(pingIDFloat)
									if hostSendTimeString, ok := messageData["host_send_time"].(string); ok {
										hostSendTime, err := strconv.ParseInt(hostSendTimeString, 10, 64)
										if err != nil {
											log.Printf("[Go/Pion] Error parsing host_send_time from audio pong: %v", err)
											return
										}

										audioPingMutex.Lock()
										originalHostSendTime, found := audioPingTimestamps[pingID]
										delete(audioPingTimestamps, pingID) // Clean up the map
										audioPingMutex.Unlock()

										if found {
											// Verify the timestamp hasn't been tampered with
											if hostSendTime != originalHostSendTime {
												log.Printf("[Go/Pion] [AUDIO PONG] Timestamp mismatch for ping %d. Original: %d, Received: %d", pingID, originalHostSendTime, hostSendTime)
												return
											}
											hostReceiveTime := time.Now().UnixNano()
											rttNano := hostReceiveTime - hostSendTime
											rttMilli := float64(rttNano) / float64(time.Millisecond)

											log.Printf("[Go/Pion] [AUDIO PONG] Ping %d E2E latency: %.2f ms", pingID, rttMilli)

											// Could cache audio latency for monitoring/combining with other metrics
											// For now, just log the measurement
										} else {
											log.Printf("[Go/Pion] [AUDIO PONG] Ping ID %d not found in timestamps map", pingID)
										}
									} else {
										log.Printf("[Go/Pion] [AUDIO PONG] Invalid 'host_send_time' in pong message: %v", messageData["host_send_time"])
									}
								} else {
									log.Printf("[Go/Pion] [AUDIO PONG] Invalid 'ping_id' in pong message: %v", messageData["ping_id"])
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
	// LATENCY CRITICAL: This function bypasses pacing by writing with Duration: 0
	// It should ONLY be used for testing/debugging, NEVER in production builds
	// Production code should use sendVideoSample with accurate durations for proper pacing

	// Enhanced gating: check if zero-duration packets are allowed
	if !isZeroDurationAllowed() {
		log.Printf("[ERROR] sendVideoPacket called but zero-duration packets are not allowed!")
		log.Printf("[ERROR] This bypasses pacing and increases latency. Use sendVideoSample with accurate durations.")
		log.Printf("[ERROR] To enable: set WEBRTC_ALLOW_ZERO_DURATION=1 or WEBRTC_DEBUG_MODE=1")
		return -2 // Distinct error code for zero-duration guard violation
	}

	log.Printf("[WARNING] sendVideoPacket called - this bypasses pacing (Duration: 0)")
	log.Printf("[WARNING] This may cause packetization delay, reordering, and jitter downstream")
	log.Printf("[WARNING] For low-latency streaming, use sendVideoSample with accurate frame durations")

	// MINIMAL LOCK SCOPE: Only hold pcMutex for state validation, not during I/O
	pcMutex.Lock()
	if peerConnection == nil || videoTrack == nil {
		pcMutex.Unlock()
		return -1
	}
	if connectionState != webrtc.PeerConnectionStateConnected {
		pcMutex.Unlock()
		return 0
	}

	// Get track reference and release lock IMMEDIATELY
	track := videoTrack
	pcMutex.Unlock() // CRITICAL: Release lock before I/O operation

	// Validate that zero duration is allowed (already checked at function entry)
	// This is redundant but ensures consistency
	if !validateVideoDuration(0) {
		log.Printf("[ERROR] Duration validation failed for zero-duration packet")
		return -3 // Should not happen if gating is working properly
	}

	// Reuse buffer from pool to avoid per-call allocation
	n := int(size)
	buf := getSampleBuf(n)
	C.memcpy(unsafe.Pointer(&buf[0]), data, C.size_t(n))

	// Write with zero duration (no pacing) - this is the test/debug functionality
	// WARNING: This bypasses WebRTC pacing and may cause jitter
	if err := track.WriteSample(media.Sample{Data: buf, Duration: 0}); err != nil {
		putSampleBuf(buf) // Return buffer on error
		log.Printf("[ERROR] sendVideoPacket WriteSample failed: %v", err)
		return -1
	}

	// Return buffer to pool after successful write
	putSampleBuf(buf)

	// Track zero-duration packet usage for monitoring
	zeroDurationCount++

	// Debug: log send rate once per second (shared counters, disabled during streaming)
	sendCount++
	if time.Since(sendLastLog) >= time.Second {
		// log.Printf("[Pion] send samples/s: %d (zero-duration: %d)", sendCount, zeroDurationCount)
		sendCount = 0
		zeroDurationCount = 0
		sendLastLog = time.Now()
	}
	return 0
}

// isProductionBuild returns true if this is a production build
// This can be controlled via build tags or environment variables
func isProductionBuild() bool {
	// Check for production build tag

	// Check environment variable for runtime control
	if debugMode := os.Getenv("WEBRTC_DEBUG_MODE"); debugMode == "1" || debugMode == "true" {
		return false // Debug mode enabled
	}

	// Check for debug build tag at compile time
	// This will be false if built with -tags debug
	if os.Getenv("GO_BUILD_TAGS") == "debug" {
		return false
	}

	// Default to production mode for safety - only allow zero-duration in debug builds
	return true
}

// isZeroDurationAllowed returns true if zero-duration video packets are allowed
// This provides finer control than the binary production/debug distinction
func isZeroDurationAllowed() bool {
	// Always allow in debug builds
	if !isProductionBuild() {
		return true
	}

	// In production, only allow if explicitly enabled for testing
	allowZeroDuration := os.Getenv("WEBRTC_ALLOW_ZERO_DURATION")
	return allowZeroDuration == "1" || allowZeroDuration == "true"
}

// validateVideoDuration validates that video pacing parameters are reasonable
// Returns true if duration is valid for low-latency streaming
func validateVideoDuration(durationUs int64) bool {
	// Reject obviously invalid durations
	if durationUs < 0 {
		log.Printf("[WARNING] Invalid negative video duration: %d us", durationUs)
		return false
	}

	// For zero duration (unpaced), require explicit allowance
	if durationUs == 0 {
		if !isZeroDurationAllowed() {
			log.Printf("[WARNING] Zero duration video packet rejected - pacing disabled in production")
			return false
		}
		log.Printf("[WARNING] Zero duration video packet allowed - pacing disabled")
		return true
	}

	// Validate reasonable duration bounds for video (0.1ms to 1 second)
	minDurationUs := int64(100)     // 0.1ms minimum
	maxDurationUs := int64(1000000) // 1 second maximum

	if durationUs < minDurationUs {
		log.Printf("[WARNING] Video duration too small: %d us (minimum: %d us)", durationUs, minDurationUs)
		return false
	}

	if durationUs > maxDurationUs {
		log.Printf("[WARNING] Video duration too large: %d us (maximum: %d us)", durationUs, maxDurationUs)
		return false
	}

	// Duration is within valid range
	return true
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

	// Stop the audio sender goroutine and buffer completion handler
	if audioSendStop != nil {
		close(audioSendStop)
		audioSendStop = nil
		log.Println("[Go/Pion] Audio sender goroutine stop signal sent")
	}

	// Drain any remaining packets from the queue and return their buffers to pool
	if audioSendQueue != nil {
		timeout := time.After(1 * time.Second) // Prevent infinite wait
		drained := 0
		for len(audioSendQueue) > 0 {
			select {
			case pkt := <-audioSendQueue:
				// Return buffer to pool for any undelivered packets
				if len(pkt.Payload) > 0 {
					putSampleBuf(pkt.Payload)
				}
				drained++
			case <-timeout:
				log.Printf("[Go/Pion] Timeout draining audio queue, %d packets remaining", len(audioSendQueue))
				// Continue with shutdown even if queue not fully drained
				break
			}
		}
		audioSendQueue = nil
		log.Printf("[Go/Pion] Drained %d packets from audio send queue", drained)
	}

	// Drain any pending buffer completion signals
	if audioBufferCompletion != nil {
		timeout := time.After(500 * time.Millisecond)
		completed := 0
		for len(audioBufferCompletion) > 0 {
			select {
			case buffer := <-audioBufferCompletion:
				putSampleBuf(buffer)
				completed++
			case <-timeout:
				log.Printf("[Go/Pion] Timeout draining buffer completion queue, %d buffers remaining",
					len(audioBufferCompletion))
				break
			}
		}
		audioBufferCompletion = nil
		log.Printf("[Go/Pion] Processed %d buffer completion signals", completed)
	}

	// Final buffer pool statistics
	logBufferPoolStats()

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

//export SetWebRTCStatsCallback
func SetWebRTCStatsCallback(callback C.WebRTCStatsCallback) {
	webrtcStatsCallback = callback
	log.Printf("[Go/Pion] Enhanced WebRTC stats callback registered")
}

// validateAudioTimestampConsistency checks RTP timestamp progression for debugging
// This function can be called periodically to verify timestamp consistency
func validateAudioTimestampConsistency() {
	if !audioRTPState.IsBaselineSet() {
		log.Println("[Go/Pion] Timestamp validation: No baseline established yet")
		return
	}

	// Get current RTP state atomically (no locks needed!)
	currentSeq := atomic.LoadUint32(&audioRTPState.sequence)
	currentTS := atomic.LoadUint32(&audioRTPState.timestamp)
	baseline := audioRTPState.GetBaseline()

	// Calculate expected RTP timestamp based on sequence number
	expectedRTP := baseline + (currentSeq * audioFrameDuration)

	// Calculate expected PTS progression
	expectedPTSDelta := int64(currentSeq) * (int64(audioFrameDuration) * 1000000 / 48000) // Convert to microseconds

	// Check for discrepancies
	rtpDiff := int64(currentTS) - int64(expectedRTP)
	ptsDiff := (int64(currentSeq) * 10000) - expectedPTSDelta // 10ms per frame in microseconds

	if rtpDiff != 0 || ptsDiff != 0 {
		log.Printf("[Go/Pion] Timestamp validation WARNING: seq=%d, RTP diff=%d, PTS diff=%d us",
			currentSeq, rtpDiff, ptsDiff)
	} else {
		log.Printf("[Go/Pion] Timestamp validation OK: seq=%d, RTP=%d, baseline=%d",
			currentSeq, currentTS, baseline)
	}
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

		// Track comprehensive WebRTC stats
		for _, pkt := range pkts {
			switch p := pkt.(type) {
			case *rtcp.ReceiverReport:
				for _, report := range p.Reports {
					// Update stats tracking
					webrtcStats.statsMutex.Lock()
					// Note: In a real implementation, we'd get these from Pion's internal stats
					// For now, we'll use the RTCP report data as a proxy
					packetLoss := float64(report.FractionLost) / 256.0
					jitterSeconds := float64(report.Jitter) / 90000.0
					webrtcStats.lastStatsUpdate = time.Now()
					webrtcStats.statsMutex.Unlock()

					// Call legacy RTCP callback if registered
					if rtcpCallback != nil {
						lastRttMutex.Lock()
						rttMs := lastRttMs
						lastRttMutex.Unlock()
						C.callRTCPCallback(rtcpCallback, C.double(packetLoss), C.double(rttMs), C.double(jitterSeconds))
					}

					// Call enhanced stats callback if registered
					if webrtcStatsCallback != nil {
						lastRttMutex.Lock()
						rttMs := lastRttMs
						lastRttMutex.Unlock()

						webrtcStats.statsMutex.RLock()
						nackCount := webrtcStats.nackCount
						pliCount := webrtcStats.pliCount
						twccCount := webrtcStats.twccCount
						queueLen := webrtcStats.pacerQueueLength
						bitrate := webrtcStats.sendBitrateKbps
						webrtcStats.statsMutex.RUnlock()

						C.callWebRTCStatsCallback(webrtcStatsCallback,
							C.double(packetLoss), C.double(rttMs), C.double(jitterSeconds),
							C.uint(nackCount), C.uint(pliCount), C.uint(twccCount),
							C.uint(queueLen), C.uint(bitrate))
					}
				}
			case *rtcp.PictureLossIndication:
				// Track PLI count for stats
				webrtcStats.statsMutex.Lock()
				webrtcStats.pliCount++
				webrtcStats.statsMutex.Unlock()

				// Call legacy PLI callback
				if pliCallback != nil {
					C.callPLICallback(pliCallback)
				}

				log.Printf("[Go/Pion] PLI received - total: %d", webrtcStats.pliCount)

			case *rtcp.FullIntraRequest:
				// Track FIR count (similar to PLI)
				webrtcStats.statsMutex.Lock()
				webrtcStats.pliCount++
				webrtcStats.statsMutex.Unlock()

				if pliCallback != nil {
					C.callPLICallback(pliCallback)
				}

				log.Printf("[Go/Pion] FIR received - total: %d", webrtcStats.pliCount)

			case *rtcp.TransportLayerNack:
				// Track NACK count
				webrtcStats.statsMutex.Lock()
				webrtcStats.nackCount += uint32(len(p.Nacks))
				webrtcStats.statsMutex.Unlock()

				log.Printf("[Go/Pion] NACK received (%d packets) - total: %d",
					len(p.Nacks), webrtcStats.nackCount)

			default:
				// Note: TransportLayerCc (TWCC) feedback is handled internally by pion/webrtc
				// and may not be exposed as a separate RTCP packet in the current version.
				// TWCC feedback is still being processed by the underlying WebRTC stack.
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
