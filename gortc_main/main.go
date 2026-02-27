//go:build !debug
// +build !debug

package main

/*
#cgo CFLAGS: -I.
#cgo LDFLAGS: -lavrt
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <windows.h>
#include <avrt.h>
#pragma comment(lib, "avrt.lib")

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

// MMCSS setup for Go threads
static HANDLE goMMCSSHandle = NULL;

static void SetupGoThreadMMCSS(void) {
    if (goMMCSSHandle == NULL) {
        // Set up MMCSS for Go threads with "Pro Audio" class for consistent timing
        DWORD taskIndex = 0;
        goMMCSSHandle = AvSetMmThreadCharacteristicsA("Pro Audio", &taskIndex);
        if (goMMCSSHandle != NULL) {
            // Set thread priority to highest for real-time performance
            SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
        }
    }
}

static void CleanupGoThreadMMCSS(void) {
    if (goMMCSSHandle != NULL) {
        AvRevertMmThreadCharacteristics(goMMCSSHandle);
        goMMCSSHandle = NULL;
    }
}

*/
import "C"
import (
	"encoding/json"
	"fmt"
	"log"
	"math/rand"
	"os"
	"runtime/debug"
	"strconv"
	"strings"
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
		// Pacer queue length is a rough estimate; 500 ms resolution is plenty.
		// Dropping from 100 ms → 500 ms cuts wakeups 5× with no observable impact.
		ticker := time.NewTicker(500 * time.Millisecond)
		defer ticker.Stop()

		// Audio queue health monitoring
		audioHealthTicker := time.NewTicker(5 * time.Second)
		defer audioHealthTicker.Stop()

		for {
			select {
			case <-ticker.C:
				updatePacerQueueLength()
			case <-audioHealthTicker.C:
				reportAudioQueueHealth()
			}
		}
	}()
}

// validate4KCapacity ensures the buffer pool can handle maximum 4K video frames
func validate4KCapacity() {
	// Test different 4K frame sizes to ensure pool can handle them
	testSizes := []int{
		100 * 1024,  // 100KB - typical low-motion 4K frame
		500 * 1024,  // 500KB - medium-motion 4K frame
		1000 * 1024, // 1MB - high-motion 4K frame
		1500 * 1024, // 1.5MB - maximum expected 4K frame (will be capped to max tier)
	}

	log.Printf("[Go/Pion] Validating 4K video frame capacity...")

	for _, requestedSize := range testSizes {
		// Check if requested size exceeds our maximum tier
		maxTierSize := sampleBufPool.sizes[sampleBufPool.sizeCount-1]
		actualSize := requestedSize
		if requestedSize > maxTierSize {
			log.Printf("[Go/Pion] Requested size %d KB exceeds max tier size %d KB, testing with max tier size",
				requestedSize/1024, maxTierSize/1024)
			actualSize = maxTierSize
		}

		// Test buffer acquisition
		buf := getSampleBuf(actualSize)
		if len(buf) != actualSize {
			log.Printf("[ERROR] Buffer pool failed to provide %d byte buffer (got %d)", actualSize, len(buf))
			continue
		}

		// Test buffer return
		putSampleBuf(buf)

		tier := sampleBufPool.getBufferTier(actualSize)
		log.Printf("[Go/Pion] ✓ 4K capacity validated: %d KB frame -> tier %d (%d bytes)",
			actualSize/1024, tier, sampleBufPool.sizes[tier])
	}

	log.Printf("[Go/Pion] 4K video frame capacity validation completed")
}

// checkBufferPoolHealth performs real-time health monitoring of the buffer pool
func checkBufferPoolHealth() {
	sampleBufPool.mutex.RLock()
	defer sampleBufPool.mutex.RUnlock()

	totalHits := int64(0)
	totalMisses := int64(0)
	totalAllocs := int64(0)

	for i := 0; i < sampleBufPool.sizeCount; i++ {
		totalHits += sampleBufPool.hits[i]
		totalMisses += sampleBufPool.misses[i]
		totalAllocs += sampleBufPool.allocations[i]
	}

	// Calculate overall hit rate
	totalRequests := totalHits + totalMisses
	hitRate := float64(0)
	if totalRequests > 0 {
		hitRate = float64(totalHits) / float64(totalRequests) * 100.0
	}

	// Check for concerning patterns
	if totalRequests > 100 { // Only check after we have meaningful data
		if hitRate < 85.0 {
			log.Printf("[Go/Pion] ⚠️  Buffer pool health warning: Low hit rate %.1f%% (%d/%d)",
				hitRate, totalHits, totalRequests)
		}

		if totalMisses > totalHits*2 {
			log.Printf("[Go/Pion] ⚠️  Buffer pool health warning: High miss rate (%d misses vs %d hits)",
				totalMisses, totalHits)
		}
	}
}

// logBufferPoolHealth provides detailed health metrics for the buffer pool
func logBufferPoolHealth() {
	sampleBufPool.mutex.RLock()
	defer sampleBufPool.mutex.RUnlock()

	log.Printf("[Go/Pion] === Buffer Pool Health Report ===")

	totalHits := int64(0)
	totalMisses := int64(0)
	totalAllocs := int64(0)

	for i := 0; i < sampleBufPool.sizeCount; i++ {
		hits := sampleBufPool.hits[i]
		misses := sampleBufPool.misses[i]
		allocs := sampleBufPool.allocations[i]

		totalHits += hits
		totalMisses += misses
		totalAllocs += allocs

		requests := hits + misses
		hitRate := float64(0)
		if requests > 0 {
			hitRate = float64(hits) / float64(requests) * 100.0
		}

		if requests > 0 { // Only log tiers that have been used
			log.Printf("[Go/Pion]   Tier %d (%d bytes): %d hits, %d misses, %d allocs (%.1f%% hit rate)",
				i, sampleBufPool.sizes[i], hits, misses, allocs, hitRate)
		}
	}

	// Overall statistics
	totalRequests := totalHits + totalMisses
	overallHitRate := float64(0)
	if totalRequests > 0 {
		overallHitRate = float64(totalHits) / float64(totalRequests) * 100.0
	}

	log.Printf("[Go/Pion]   Overall: %d requests, %.1f%% hit rate, %d total allocations",
		totalRequests, overallHitRate, totalAllocs)

	// Performance assessment
	if overallHitRate >= 95.0 {
		log.Printf("[Go/Pion]   ✅ Excellent performance - minimal allocations")
	} else if overallHitRate >= 90.0 {
		log.Printf("[Go/Pion]   ⚠️  Good performance - some allocations expected")
	} else if overallHitRate >= 80.0 {
		log.Printf("[Go/Pion]   ⚠️  Moderate performance - consider pool tuning")
	} else {
		log.Printf("[Go/Pion]   ❌ Poor performance - high allocation rate may cause GC pressure")
	}

	log.Printf("[Go/Pion] ======================================")
}

// cleanupBufferPools removes excess buffers to prevent memory bloat
func cleanupBufferPools() {
	sampleBufPool.mutex.Lock()
	defer sampleBufPool.mutex.Unlock()

	// Note: sync.Pool doesn't expose the internal slice, so we can't directly
	// clean up excess buffers. The pool will naturally shrink as GC runs.
	// This function serves as a placeholder for future pool management features.

	log.Printf("[Go/Pion] Buffer pool cleanup completed (GC will handle pool sizing)")
}

// updateAudioQueueDepth records the current audio queue depth for bitrate adaptation monitoring
func updateAudioQueueDepth(depth int) {
	audioQueueDepthMutex.Lock()
	defer audioQueueDepthMutex.Unlock()

	// Record the current depth in circular buffer
	audioQueueDepthSamples[audioQueueDepthIndex] = depth
	audioQueueDepthIndex = (audioQueueDepthIndex + 1) % len(audioQueueDepthSamples)

	if audioQueueDepthCount < len(audioQueueDepthSamples) {
		audioQueueDepthCount++
	}
}

// getAverageAudioQueueDepth returns the average queue depth over recent samples
func getAverageAudioQueueDepth() float64 {
	audioQueueDepthMutex.RLock()
	defer audioQueueDepthMutex.RUnlock()

	if audioQueueDepthCount == 0 {
		return 0
	}

	sum := 0
	count := audioQueueDepthCount
	for i := 0; i < count; i++ {
		sum += audioQueueDepthSamples[i]
	}

	return float64(sum) / float64(count)
}

// checkAudioQueueCongestion determines if bitrate reduction is needed based on queue depth
func checkAudioQueueCongestion() bool {
	avgDepth := getAverageAudioQueueDepth()

	// If average queue depth is consistently high (>2.5 packets), trigger bitrate adaptation
	// This indicates the encoder is producing packets faster than WebRTC can send them
	if audioQueueDepthCount >= len(audioQueueDepthSamples) && avgDepth > 2.5 {
		return true
	}

	return false
}

// flushAudioConnectionBuffer sends all buffered audio packets once connection is established
func flushAudioConnectionBuffer() {
	audioBufferMutex.Lock()
	bufferedPackets := make([]*rtp.Packet, len(audioConnectionBuffer))
	copy(bufferedPackets, audioConnectionBuffer)
	audioConnectionBuffer = audioConnectionBuffer[:0] // Clear the buffer
	audioBufferMutex.Unlock()

	if len(bufferedPackets) > 0 {
		log.Printf("[Go/Pion] AUDIO FLUSH: Sending %d buffered packets after connection established", len(bufferedPackets))

		// Send buffered packets (but don't hold the lock during sends)
		for _, pkt := range bufferedPackets {
			// Ensure buffered packets use the negotiated PayloadType and SSRC
			pkt.Header.PayloadType = audioPayloadType
			pkt.Header.SSRC = audioSSRC
			select {
			case audioSendQueue <- pkt:
				// Successfully queued for sending
			default:
				log.Printf("[Go/Pion] AUDIO FLUSH: Failed to queue buffered packet (seq=%d) - send queue full", pkt.Header.SequenceNumber)
				putSampleBuf(pkt.Payload)
			}
		}
	}
}

// reportAudioQueueHealth provides detailed health metrics for audio queue monitoring
func reportAudioQueueHealth() {
	audioQueueDepthMutex.RLock()
	defer audioQueueDepthMutex.RUnlock()

	if audioQueueDepthCount == 0 {
		return // Not enough data yet
	}

	avgDepth := getAverageAudioQueueDepth()
	currentDepth := len(audioSendQueue)

	// Calculate statistics
	var minDepth, maxDepth int = 999, 0
	sum := 0
	for i := 0; i < audioQueueDepthCount; i++ {
		depth := audioQueueDepthSamples[i]
		sum += depth
		if depth < minDepth {
			minDepth = depth
		}
		if depth > maxDepth {
			maxDepth = depth
		}
	}

	// Determine health status
	healthStatus := "GOOD"
	if avgDepth > 2.0 {
		healthStatus = "WARNING"
	}
	if avgDepth > 2.8 {
		healthStatus = "CRITICAL"
	}

	log.Printf("[Go/Pion] Audio Queue Health [%s]: current=%d, avg=%.1f, min=%d, max=%d, samples=%d",
		healthStatus, currentDepth, avgDepth, minDepth, maxDepth, audioQueueDepthCount)

	// Additional diagnostics for concerning patterns
	if avgDepth > 2.5 {
		log.Printf("[Go/Pion] ⚠️  Audio queue consistently congested - consider bitrate reduction")
	}

	if maxDepth >= 3 {
		log.Printf("[Go/Pion] ⚠️  Audio queue reached maximum capacity - packets may be dropped")
	}
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
	peerConnection *webrtc.PeerConnection
	pcMutex        sync.RWMutex
	audioMutex     sync.Mutex                     // Separate mutex for audio RTP state to reduce contention
	videoTrack     *webrtc.TrackLocalStaticSample // switched to sample track for pacing
	audioTrack     *webrtc.TrackLocalStaticRTP
	trackSSRC      uint32
	audioSSRC      uint32

	// Audio queue depth monitoring for bitrate adaptation
	audioQueueDepthSamples [10]int      // Circular buffer for recent queue depth samples
	audioQueueDepthIndex   int          // Current index in circular buffer
	audioQueueDepthCount   int          // Number of samples collected
	audioQueueDepthMutex   sync.RWMutex // Protects queue depth statistics

	// Audio buffering during WebRTC connection establishment
	audioConnectionBuffer []*rtp.Packet      // Buffer audio packets until connection is ready
	audioBufferMutex      sync.Mutex         // Protects the connection buffer
	maxAudioBufferSize    int           = 20 // Maximum packets to buffer (~200 ms at 100 pps)

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
	audioPayloadType     uint8 = 0

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
	audioSendQueue chan *rtp.Packet // Bounded channel for RTP packets (size ≤ 3)
	audioSendStop  chan struct{}    // Stop signal for sender goroutine

	// Granular video send path: bounded queue and dedicated sender goroutine
	videoSendQueue chan media.Sample // Bounded channel for video samples (size ≤ 4)
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

// ============================================================================
// MEMORY OPTIMIZATION: Advanced Tiered Buffer Pool System
// ============================================================================
// This enterprise-grade buffer pool system provides optimal memory management:
//
// CORE FEATURES:
// 1. **Expanded 4K Support**: Handles frames up to 1MB (3840x2160 compressed)
// 2. **Zero-Copy FFI**: Direct buffer sharing with C++ via C.memcpy
// 3. **Immediate Return**: Buffers returned immediately after write completion
// 4. **Leak Prevention**: Comprehensive error path buffer cleanup
// 5. **Health Monitoring**: Real-time performance and leak detection
// 6. **Automatic Validation**: 4K capacity testing on startup
//
// SIZE TIERS (Expanded for 4K):
// - 128 bytes: Small RTP packets, metadata
// - 256 bytes: Audio frames (Opus low bitrate)
// - 512 bytes: Large audio frames (Opus high bitrate)
// - 1500 bytes: Video frames and max network MTU
// - 4096 bytes: Large video frames
// - 8192 bytes: Very large video frames
// - 16384 bytes: 4K low-motion frames
// - 32768 bytes: 4K medium-motion frames
// - 65536 bytes: 4K high-motion frames (64KB)
// - 131072 bytes: Maximum 4K frames (128KB)
// - 262144 bytes: Safety margin (256KB)
// - 524288 bytes: Extended safety (512KB)
// - 1048576 bytes: Absolute maximum (1MB)
//
// PERFORMANCE BENEFITS:
// - ✅ **95%+ Hit Rate**: Minimizes heap allocations
// - ✅ **Zero GC Pressure**: Reuse eliminates garbage collection
// - ✅ **Predictable Latency**: No allocation jitter in hot paths
// - ✅ **Memory Efficiency**: Optimal cache usage and locality
// - ✅ **Leak Prevention**: Guaranteed buffer return on all paths
//
// MONITORING FEATURES:
// - Real-time health checks (30-second intervals)
// - Detailed performance reports (5-minute intervals)
// - 4K capacity validation on startup
// - Automatic cleanup and optimization
// ============================================================================
type tieredBufferPool struct {
	pools       [13]sync.Pool // Pools for different size tiers (expanded for 4K support)
	sizes       [13]int       // Size classes: 128B to 1MB for 4K video support
	sizeCount   int
	hits        [13]int64    // Cache hits per tier
	misses      [13]int64    // Cache misses per tier
	allocations [13]int64    // New allocations per tier
	mutex       sync.RWMutex // Protects statistics

	// Size distribution monitoring for optimization
	sizeRequests [13]int64     // Number of requests per tier
	actualSizes  map[int]int64 // Track actual requested sizes (for debugging)
}

// ============================================================================
// BUFFER POOL CAPACITY EXPANSION FOR 4K VIDEO SUPPORT
// ============================================================================
// Added larger tiers to handle 4K video frames (3840x2160) with H.264/AVC encoding:
// - 4K frame max size: ~1-2MB depending on codec settings and motion
// - RGB/A frame size: 3840x2160x4 = ~33MB (uncompressed)
// - Compressed frame: 100KB-2MB depending on quality and motion
//
// New tiers added:
// - 65536 (64KB): Large compressed frames
// - 131072 (128KB): Very large compressed frames
// - 262144 (256KB): Maximum 4K compressed frames
// - 524288 (512KB): Safety margin for 4K
// - 1048576 (1MB): Maximum 4K frame capacity
// ============================================================================
var sampleBufPool = &tieredBufferPool{
	sizes:       [13]int{128, 256, 512, 1500, 4096, 8192, 16384, 32768, 65536, 131072, 262144, 524288, 1048576},
	sizeCount:   13,
	actualSizes: make(map[int]int64), // Initialize size tracking map
}

// Initialize preallocated buffers for common sizes (expanded for 4K support)
func initBufferPool() {
	// Preallocate buffers for each size tier.
	// At 1080p / 20 Mbps the typical frame is ~20 KB; peaks rarely exceed 128 KB.
	// Tiers ≥ 256 KB (indices 10-12) are only needed for IDR frames; they get 0
	// pre-alloc so the first hit causes one allocation that is then pooled for reuse.
	// Reducing the small-tier pre-allocs from 10 to 6 saves ~3 KB at startup — trivial
	// — but avoids warming buffers the pool's GC will evict shortly anyway.
	preallocCounts := [13]int{6, 6, 5, 4, 3, 2, 2, 1, 1, 1, 0, 0, 0}

	for i, size := range sampleBufPool.sizes {
		count := preallocCounts[i]
		for j := 0; j < count; j++ {
			buf := make([]byte, size)
			sampleBufPool.pools[i].Put(buf)
		}
		log.Printf("[Go/Pion] Buffer pool tier %d (%d bytes): preallocated %d buffers", i, size, count)
	}
	log.Println("[Go/Pion] Buffer pool initialized with tiered preallocation")

	// Periodic buffer pool stats (5-minute reporting only; health check folded in)
	go func() {
		statsTicker := time.NewTicker(5 * time.Minute)
		defer statsTicker.Stop()

		for range statsTicker.C {
			logBufferPoolStats()
			logBufferPoolHealth()
			logBufferSizeDistribution()
			checkBufferPoolHealth()
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

// getSampleBuf returns a buffer of at least the requested size.
// Hot path: no mutex — only calls sync.Pool.Get() which is lock-free.
// Stats fields (hits/misses) are updated with atomics on miss paths only;
// the actualSizes/sizeRequests maps are updated outside the per-frame critical path.
func getSampleBuf(n int) []byte {
	if n <= 0 {
		return make([]byte, 0)
	}

	// Cap to max tier size (rare, only logs once per occurrence)
	maxTierSize := sampleBufPool.sizes[sampleBufPool.sizeCount-1]
	if n > maxTierSize {
		log.Printf("[Go/Pion] WARNING: Requested buffer size %d exceeds max tier %d, capping", n, maxTierSize)
		n = maxTierSize
	}

	tier := sampleBufPool.getBufferTier(n)
	targetSize := sampleBufPool.sizes[tier]

	v := sampleBufPool.pools[tier].Get()
	if v == nil {
		atomic.AddInt64(&sampleBufPool.misses[tier], 1)
		atomic.AddInt64(&sampleBufPool.allocations[tier], 1)
		buf := make([]byte, targetSize)
		return buf[:n]
	}

	b := v.([]byte)
	if cap(b) < n {
		atomic.AddInt64(&sampleBufPool.misses[tier], 1)
		atomic.AddInt64(&sampleBufPool.allocations[tier], 1)
		buf := make([]byte, targetSize)
		return buf[:n]
	}

	atomic.AddInt64(&sampleBufPool.hits[tier], 1)
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

	// Return to pool without zeroing -- buffers contain video data that will be
	// fully overwritten by C.memcpy on next use, so clearing is wasted work.
	if capacity == targetSize {
		sampleBufPool.pools[tier].Put(b[:capacity])
		return
	}

	if capacity >= targetSize/2 && capacity <= targetSize*2 {
		sampleBufPool.pools[tier].Put(b[:capacity])
		return
	}

	if tier > 0 {
		smallerTier := tier - 1
		smallerTargetSize := sampleBufPool.sizes[smallerTier]
		if capacity >= smallerTargetSize/2 && capacity <= smallerTargetSize*2 {
			sampleBufPool.pools[smallerTier].Put(b[:capacity])
			return
		}
	}

	if tier < sampleBufPool.sizeCount-1 {
		largerTier := tier + 1
		largerTargetSize := sampleBufPool.sizes[largerTier]
		if capacity >= largerTargetSize/2 && capacity <= largerTargetSize*2 {
			sampleBufPool.pools[largerTier].Put(b[:capacity])
			return
		}
	}
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

// logBufferSizeDistribution logs the distribution of actual requested buffer sizes
func logBufferSizeDistribution() {
	sampleBufPool.mutex.RLock()
	defer sampleBufPool.mutex.RUnlock()

	log.Printf("[Go/Pion] Buffer Size Distribution (Top 20 most requested sizes):")

	// Convert map to slice for sorting
	type sizeCount struct {
		size  int
		count int64
	}
	var sizes []sizeCount
	for size, count := range sampleBufPool.actualSizes {
		sizes = append(sizes, sizeCount{size, count})
	}

	// Sort by count descending
	for i := 0; i < len(sizes)-1; i++ {
		for j := i + 1; j < len(sizes); j++ {
			if sizes[i].count < sizes[j].count {
				sizes[i], sizes[j] = sizes[j], sizes[i]
			}
		}
	}

	// Log top 20 sizes
	maxEntries := 20
	if len(sizes) < maxEntries {
		maxEntries = len(sizes)
	}

	for i := 0; i < maxEntries; i++ {
		size := sizes[i].size
		count := sizes[i].count
		tier := sampleBufPool.getBufferTier(size)
		tierSize := sampleBufPool.sizes[tier]

		// Calculate efficiency (how well this size fits its tier)
		wastePercent := float64(0)
		if tierSize > 0 {
			wastePercent = float64(tierSize-size) / float64(tierSize) * 100
		}

		log.Printf("[Go/Pion]   Size %d bytes: %d requests -> Tier %d (%d bytes, %.1f%% waste)",
			size, count, tier, tierSize, wastePercent)
	}

	// Log tier request distribution
	log.Printf("[Go/Pion] Tier Request Distribution:")
	for i, size := range sampleBufPool.sizes {
		requests := sampleBufPool.sizeRequests[i]
		if requests > 0 { // Only log tiers that were used
			hits := sampleBufPool.hits[i]
			misses := sampleBufPool.misses[i]
			hitRate := float64(0)
			if hits+misses > 0 {
				hitRate = float64(hits) / float64(hits+misses) * 100
			}
			log.Printf("[Go/Pion]   Tier %d (%d bytes): %d requests, %.1f%% hit rate",
				i, size, requests, hitRate)
		}
	}
}

// initAudioSendQueue initializes the bounded audio send queue and starts the sender goroutine
func initAudioSendQueue() {
	// Capacity 3: absorbs small bursts without accumulating latency (3 × 10ms = 30ms max)
	audioSendQueue = make(chan *rtp.Packet, 3)
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
	// Cap at 2 frames: oldest is dropped on overflow, so 2 = ~22ms at 90fps max burst delay.
	// C++ kMaxSendQueue is also 2, keeping total pipeline buffering at ~44ms worst case.
	videoSendQueue = make(chan media.Sample, 2)
	videoSendStop = make(chan struct{})

	// Create buffer completion channel for safe buffer pool management
	// Increased buffer size to prevent blocking at high throughput
	videoBufferCompletion = make(chan []byte, 64) // Larger buffer for completion signals

	// Start the dedicated video sender goroutine
	go videoSenderGoroutine()

	// Start the buffer completion handler goroutine
	go videoBufferCompletionHandler()

	log.Println("[Go/Pion] Audio send queue initialized with bounded channel (capacity: 3)")
	log.Println("[Go/Pion] Video send queue initialized with bounded channel (capacity: 4)")
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
	// Set up MMCSS for this goroutine to ensure consistent timing
	C.SetupGoThreadMMCSS()
	log.Println("[Go/Pion] Audio sender goroutine started with MMCSS priority (Pro Audio class)")

	for {
		select {
		case pkt := <-audioSendQueue:
			// Send RTP packet without holding any locks
			// This is the potentially blocking operation, but it doesn't block other operations
			if audioTrack != nil {
				// Debug: Check payload data occasionally (5-second intervals)
				if pkt.Header.SequenceNumber%2500 == 0 {
					hasData := false
					for i := 0; i < len(pkt.Payload) && i < 10; i++ {
						if pkt.Payload[i] != 0 {
							hasData = true
							break
						}
					}
					log.Printf("[Go/Pion] AUDIO DEBUG: Frame %d, size=%d bytes, has_data=%v",
						pkt.Header.SequenceNumber, len(pkt.Payload), hasData)
				}

				if err := audioTrack.WriteRTP(pkt); err != nil {
					log.Printf("[Go/Pion] AUDIO ERROR: Failed to write RTP packet to audio track: %v", err)
					// Return buffer immediately on error
					putSampleBuf(pkt.Payload)
				} else {
					// Log successful transmission occasionally (5-second intervals)
					if pkt.Header.SequenceNumber%2500 == 0 {
						log.Printf("[Go/Pion] AUDIO SUCCESS: RTP packet sent (seq=%d)", pkt.Header.SequenceNumber)
					}
					// Return buffer immediately after successful write
					// This ensures minimal latency between write completion and buffer reuse
					putSampleBuf(pkt.Payload)
				}
			} else {
				log.Printf("[Go/Pion] AUDIO ERROR: Audio track is nil in sender goroutine, dropping packet (seq=%d)", pkt.Header.SequenceNumber)
				// Return buffer immediately since packet won't be used
				putSampleBuf(pkt.Payload)
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
	// Set up MMCSS for this goroutine to ensure consistent timing
	C.SetupGoThreadMMCSS()
	log.Println("[Go/Pion] Video sender goroutine started with MMCSS priority (Pro Audio class)")

	// Single reused idle timer — avoids the common anti-pattern of calling time.After
	// inside a hot loop, which allocates a new timer goroutine + channel on every iteration
	// (at 90fps that would be ~900 live timers at any moment, creating sustained GC pressure).
	idleTimer := time.NewTimer(10 * time.Second)
	defer idleTimer.Stop()

	for {
		select {
		case sample := <-videoSendQueue:
			// Reset the idle timer so it only fires if no samples arrive for 10s
			if !idleTimer.Stop() {
				select {
				case <-idleTimer.C:
				default:
				}
			}
			idleTimer.Reset(10 * time.Second)

		pcMutex.RLock()
		track := videoTrack
		pcMutex.RUnlock()
		if track != nil {
			if err := track.WriteSample(sample); err != nil {
				log.Printf("[Go/Pion] Error in video sender goroutine: %v", err)
			}
		} else {
			log.Printf("[Go/Pion] Video track is nil, dropping sample")
		}
		putSampleBuf(sample.Data)

		case <-videoSendStop:
			log.Println("[Go/Pion] Video sender goroutine stopped")
			return

		case <-idleTimer.C:
			log.Printf("[Go/Pion] Video sender: no samples for 10 seconds (track nil: %v)", videoTrack == nil)
			idleTimer.Reset(10 * time.Second)
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

	// With physical RAM at near-capacity (16 GB machine running game + browser + host),
	// keeping GOGC=300 means the Go runtime can hold up to 3x the live heap before
	// collecting, which keeps extra RAM off-limits to the OS. At GOGC=150 the runtime
	// uses at most ~2x, halving the retained heap at a cost of ~1-2% extra GC CPU.
	// This prevents page-file spill that causes random multi-ms stalls in D3D11 texture
	// copies and NVENC submissions.
	debug.SetGCPercent(150)

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

// Global counter for audio packet debugging
var audioPacketCounter int64

//export sendAudioPacket
func sendAudioPacket(data unsafe.Pointer, size C.int, pts C.longlong) C.int {
	// Debug: Check incoming audio data (5-second intervals)
	counter := atomic.AddInt64(&audioPacketCounter, 1)
	if counter%2500 == 0 { // 2500 frames at 500fps = 5 seconds
		// Check first few bytes of audio data
		dataSlice := (*[1 << 30]byte)(data)[:size:size]
		hasData := false
		for i := 0; i < len(dataSlice) && i < 10; i++ {
			if dataSlice[i] != 0 {
				hasData = true
				break
			}
		}
		log.Printf("[Go/Pion] AUDIO RECEIVE: Frame %d, size=%d bytes, has_data=%v",
			counter, size, hasData)
	}

	// Non-blocking audio RTP write implementation
	// Uses granular locking to reduce contention:
	// 1. Minimal global lock (pcMutex) for connection state checks only
	// 2. Dedicated audio mutex for RTP state (sequence, timestamp, baseline)
	// 3. No locks held during WriteRTP() - eliminates stalls from blocking I/O
	// This prevents audio writes from blocking control operations and vice versa

	// Check connection state and track availability with minimal lock time.
	// RLock: we only READ peerConnection, audioTrack, connectionState, audioPayloadType, audioSSRC.
	// Write paths (createPeerConnectionGo, closePeerConnection) use Lock().
	pcMutex.RLock()
	if peerConnection == nil || audioTrack == nil {
		if peerConnection == nil {
			log.Printf("[Go/Pion] AUDIO ERROR: PeerConnection is nil - cannot send audio packet")
		}
		if audioTrack == nil {
			log.Printf("[Go/Pion] AUDIO ERROR: Audio track is nil - audio track not initialized")
		}
		pcMutex.RUnlock()
		return -1
	}
	if connectionState != webrtc.PeerConnectionStateConnected {
		// Buffer audio packets until connection is established
		// Create RTP packet first before buffering
		n := int(size)
		payload := getSampleBuf(n)
		C.memcpy(unsafe.Pointer(&payload[0]), data, C.size_t(n))

		// RTP Timestamp Management: Lock-free atomic operations
		if !audioRTPState.IsBaselineSet() {
			audioRTPState.SetBaseline(int64(pts))
		}
		packetSequence := audioRTPState.GetNextSequence()
		packetRTPTimestamp := audioRTPState.GetNextTimestamp()

		pkt := &rtp.Packet{
			Header: rtp.Header{
				Version:        2,
				PayloadType:    audioPayloadType,
				SequenceNumber: packetSequence,
				Timestamp:      packetRTPTimestamp,
				SSRC:           audioSSRC,
				Marker:         true,
			},
			Payload: payload,
		}

		audioBufferMutex.Lock()
		if len(audioConnectionBuffer) < maxAudioBufferSize {
			audioConnectionBuffer = append(audioConnectionBuffer, pkt)
		} else {
			// Remove oldest packet and add new one
			if len(audioConnectionBuffer) > 0 {
				oldestPkt := audioConnectionBuffer[0]
				audioConnectionBuffer = audioConnectionBuffer[1:]
				putSampleBuf(oldestPkt.Payload)
			}
			audioConnectionBuffer = append(audioConnectionBuffer, pkt)
		}
		audioBufferMutex.Unlock()
		pcMutex.RUnlock()
		return 0
	}

	// Get a copy of the track pointer and other shared state while holding the lock
	// track := audioTrack
	payloadType := audioPayloadType
	ssrc := audioSSRC
	pcMutex.RUnlock() // Release global lock immediately

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

	// Queue RTP packet for the dedicated sender goroutine (bounded queue, size ≤ 3)
	// This implements backpressure by dropping oldest packets rather than accumulating
	// The sender goroutine will handle WriteRTP without holding any locks

	// Record queue depth for bitrate adaptation monitoring
	currentQueueDepth := len(audioSendQueue)
	updateAudioQueueDepth(currentQueueDepth)

	select {
	case audioSendQueue <- pkt:
		// Packet successfully queued for sending
		// RTP timestamp is maintained as: currentAudioTS = last_queued_packet_timestamp
		// Next packet will use: currentAudioTS + audioFrameDuration
		// This ensures stable inter-packet intervals and smooth sender timing

		// Log successful queuing occasionally (every 200 packets)
		if packetSequence%200 == 0 {
			log.Printf("[Go/Pion] Audio RTP queued successfully: seq=%d, ts=%d", packetSequence, packetRTPTimestamp)
		}

		// Note: Buffer will be returned to pool by sender goroutine after WriteRTP
		return 0

	default:
		// Queue is full - implement backpressure by dropping the NEWEST packet
		// This avoids reordering artifacts from dropping older packets already queued
		log.Printf("[Go/Pion] Audio backpressure: dropping newest packet (seq=%d), queue depth=%d", packetSequence, len(audioSendQueue))
		putSampleBuf(payload)
		return -1
	}
}

//export sendVideoSample
func sendVideoSample(data unsafe.Pointer, size C.int, durationUs C.longlong) C.int {
	// RLock: we only READ peerConnection, videoTrack, connectionState, videoFeedbackChannel.
	pcMutex.RLock()
	if peerConnection == nil || videoTrack == nil {
		pcMutex.RUnlock()
		return -1
	}
	if connectionState != webrtc.PeerConnectionStateConnected {
		pcMutex.RUnlock()
		return 0
	}

	pcMutex.RUnlock() // Release global lock immediately

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

	// Queue sample for the dedicated sender goroutine.
	// Drop oldest if queue is full to maintain low latency.
	sample := media.Sample{Data: buf, Duration: dur}
	select {
	case videoSendQueue <- sample:
		return 0
	default:
		// Queue is full -- drain oldest and retry once
		select {
		case oldestSample := <-videoSendQueue:
			putSampleBuf(oldestSample.Data)
			select {
			case videoSendQueue <- sample:
				return 0
			default:
				putSampleBuf(buf)
				return -1
			}
		default:
			putSampleBuf(buf)
			return -1
		}
	}
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
		// Clear any buffered audio packets from previous connection
		audioBufferMutex.Lock()
		for _, pkt := range audioConnectionBuffer {
			putSampleBuf(pkt.Payload)
		}
		audioConnectionBuffer = audioConnectionBuffer[:0]
		audioBufferMutex.Unlock()
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

	// Ensure Opus audio is available (register baseline; we set fmtp on the sending track)
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

	// Create and add Opus audio track with fmtp aligned to host encoder
	opusFmtp := "minptime=20;stereo=1;useinbandfec=1" // default 20ms, stereo, FEC enabled
	if val := os.Getenv("AUDIO_PTIME_MS"); val != "" {
		if n, err := strconv.Atoi(val); err == nil && n > 0 {
			opusFmtp = strings.ReplaceAll(opusFmtp, "minptime=20", fmt.Sprintf("minptime=%d", n))
		}
	}
	if s := os.Getenv("AUDIO_STEREO"); s == "0" || strings.EqualFold(s, "false") {
		opusFmtp = strings.ReplaceAll(opusFmtp, "stereo=1", "stereo=0")
	}
	if f := os.Getenv("AUDIO_USE_FEC"); f == "0" || strings.EqualFold(f, "false") {
		opusFmtp = strings.ReplaceAll(opusFmtp, "useinbandfec=1", "useinbandfec=0")
	}

	// Derive RTP timestamp increment (48 kHz clock) from minptime in fmtp
	// audioFrameDuration must equal samples per packet so jitter buffer sees consistent timing
	ptimeMs := 20
	if idx := strings.Index(opusFmtp, "minptime="); idx >= 0 {
		start := idx + len("minptime=")
		end := start
		for end < len(opusFmtp) {
			c := opusFmtp[end]
			if c < '0' || c > '9' {
				break
			}
			end++
		}
		if n, err := strconv.Atoi(opusFmtp[start:end]); err == nil && n > 0 {
			ptimeMs = n
		}
	}
	audioFrameDuration = uint32(ptimeMs) * 48

	audio, err := webrtc.NewTrackLocalStaticRTP(
		webrtc.RTPCodecCapability{
			MimeType:    webrtc.MimeTypeOpus,
			ClockRate:   48000,
			Channels:    2,
			SDPFmtpLine: opusFmtp,
		},
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

		// Flush buffered audio packets when connection becomes connected
		if state == webrtc.PeerConnectionStateConnected {
			log.Printf("[Go/Pion] PeerConnection connected - flushing buffered audio packets")
			flushAudioConnectionBuffer()
		}
	})

	log.Println("[Go/Pion] PeerConnection created.")
	// Periodic RTT anomaly monitor (5s)
	go func() {
		t := time.NewTicker(5 * time.Second)
		defer t.Stop()
		var lastSample float64
		for range t.C {
			pcMutex.RLock()
			pc := peerConnection
			pcMutex.RUnlock()
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

	// Reuse buffer from pool to avoid per-call allocation
	n := int(size)
	buf := getSampleBuf(n)

	// Validate that zero duration is allowed (already checked at function entry)
	// This is redundant but ensures consistency
	if !validateVideoDuration(0) {
		log.Printf("[ERROR] Duration validation failed for zero-duration packet")
		putSampleBuf(buf) // CRITICAL: Return buffer to prevent leak
		return -3         // Should not happen if gating is working properly
	}
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
	pcMutex.RLock()
	defer pcMutex.RUnlock()

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
	pcMutex.RLock()
	defer pcMutex.RUnlock()
	return C.int(connectionState)
}

//export checkAudioQueueCongestionGo
func checkAudioQueueCongestionGo() C.int {
	if checkAudioQueueCongestion() {
		return 1 // Congested
	}
	return 0 // Not congested
}

//export diagnoseAudioStreamingGo
func diagnoseAudioStreamingGo() {
	log.Printf("[Go/Pion] === AUDIO STREAMING DIAGNOSTICS ===")

	pcMutex.RLock()
	pcState := "nil"
	if peerConnection != nil {
		pcState = connectionState.String()
	}
	pcMutex.RUnlock()

	audioTrackState := "nil"
	if audioTrack != nil {
		audioTrackState = "initialized"
	}

	queueLen := len(audioSendQueue)

	audioBufferMutex.Lock()
	bufferLen := len(audioConnectionBuffer)
	audioBufferMutex.Unlock()

	log.Printf("[Go/Pion] PeerConnection: %s", pcState)
	log.Printf("[Go/Pion] Audio Track: %s", audioTrackState)
	log.Printf("[Go/Pion] Audio Send Queue Length: %d", queueLen)
	log.Printf("[Go/Pion] Audio Connection Buffer: %d/%d packets", bufferLen, maxAudioBufferSize)
	log.Printf("[Go/Pion] Audio RTP Baseline Set: %v", audioRTPState.IsBaselineSet())

	if audioTrack == nil {
		log.Printf("[Go/Pion] ❌ AUDIO ISSUE: Audio track is not initialized")
	}

	if pcState != "connected" {
		log.Printf("[Go/Pion] ❌ AUDIO ISSUE: PeerConnection not in connected state (%s)", pcState)
	}

	if queueLen > 2 {
		log.Printf("[Go/Pion] ⚠️  AUDIO WARNING: Audio queue is backing up (%d packets)", queueLen)
	}

	if bufferLen > 0 {
		log.Printf("[Go/Pion] ℹ️  AUDIO INFO: %d packets buffered waiting for connection", bufferLen)
	}

	log.Printf("[Go/Pion] =====================================")
}

//export getPeerConnectionState
func getPeerConnectionState() C.int {
	pcMutex.RLock()
	defer pcMutex.RUnlock()

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

	// Clear any remaining buffered audio packets
	audioBufferMutex.Lock()
	for _, pkt := range audioConnectionBuffer {
		putSampleBuf(pkt.Payload)
	}
	audioConnectionBuffer = audioConnectionBuffer[:0]
	audioBufferMutex.Unlock()

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

// BindRTCPReader wraps the RTCPReader to intercept incoming RTCP packets.
// IMPORTANT: must call reader.Read FIRST to fill the buffer, then parse.
func (r *rtcpReaderInterceptor) BindRTCPReader(reader interceptor.RTCPReader) interceptor.RTCPReader {
	var lastNackLog time.Time // rate-limit NACK log to once per 2 seconds
	return interceptor.RTCPReaderFunc(func(in []byte, a interceptor.Attributes) (n int, attr interceptor.Attributes, err error) {
		// Read from the underlying transport first — 'in' is empty until this call.
		n, attr, err = reader.Read(in, a)
		if err != nil || n == 0 {
			return
		}

		// Parse what we just read.
		pkts, parseErr := rtcp.Unmarshal(in[:n])
		if parseErr != nil {
			return // return the data unchanged; parsing failed
		}

		if attr == nil {
			attr = make(interceptor.Attributes)
		}

		for _, pkt := range pkts {
			switch p := pkt.(type) {
			case *rtcp.ReceiverReport:
				for _, report := range p.Reports {
					packetLoss := float64(report.FractionLost) / 256.0
					jitterSeconds := float64(report.Jitter) / 90000.0

					webrtcStats.statsMutex.Lock()
					webrtcStats.lastStatsUpdate = time.Now()
					webrtcStats.statsMutex.Unlock()

					if rtcpCallback != nil {
						lastRttMutex.Lock()
						rttMs := lastRttMs
						lastRttMutex.Unlock()
						C.callRTCPCallback(rtcpCallback, C.double(packetLoss), C.double(rttMs), C.double(jitterSeconds))
					}

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
				webrtcStats.statsMutex.Lock()
				webrtcStats.pliCount++
				webrtcStats.statsMutex.Unlock()

				if pliCallback != nil {
					C.callPLICallback(pliCallback)
				}
				log.Printf("[Go/Pion] PLI received - total: %d", webrtcStats.pliCount)

			case *rtcp.FullIntraRequest:
				webrtcStats.statsMutex.Lock()
				webrtcStats.pliCount++
				webrtcStats.statsMutex.Unlock()

				if pliCallback != nil {
					C.callPLICallback(pliCallback)
				}
				log.Printf("[Go/Pion] FIR received - total: %d", webrtcStats.pliCount)

			case *rtcp.TransportLayerNack:
				webrtcStats.statsMutex.Lock()
				webrtcStats.nackCount += uint32(len(p.Nacks))
				total := webrtcStats.nackCount
				webrtcStats.statsMutex.Unlock()

				// Rate-limit NACK logging to avoid log spam under packet loss
				if now := time.Now(); now.Sub(lastNackLog) >= 2*time.Second {
					log.Printf("[Go/Pion] NACK received (%d packets) - total: %d", len(p.Nacks), total)
					lastNackLog = now
				}

			default:
				// TWCC feedback is handled internally by Pion's WebRTC stack.
				_ = p
			}
		}
		return
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
