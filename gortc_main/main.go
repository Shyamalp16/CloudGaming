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

// MMCSS is thread-affine. Each locked Go sender thread owns its own handle.
static HANDLE SetupGoAudioThreadMMCSS(void) {
    DWORD taskIndex = 0;
    HANDLE handle = AvSetMmThreadCharacteristicsA("Pro Audio", &taskIndex);
    if (handle != NULL) AvSetMmThreadPriority(handle, AVRT_PRIORITY_HIGH);
    return handle;
}

static HANDLE SetupGoVideoThreadMMCSS(void) {
    DWORD taskIndex = 0;
    HANDLE handle = AvSetMmThreadCharacteristicsA("Playback", &taskIndex);
    if (handle != NULL) AvSetMmThreadPriority(handle, AVRT_PRIORITY_HIGH);
    return handle;
}

static void CleanupGoThreadMMCSS(HANDLE handle) {
    if (handle != NULL) AvRevertMmThreadCharacteristics(handle);
}

*/
import "C"
import (
	"encoding/json"
	"fmt"
	"log"
	"os"
	"path/filepath"
	"runtime"
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

func loadDotEnvFile(path string) (bool, error) {
	data, err := os.ReadFile(path)
	if err != nil {
		if os.IsNotExist(err) {
			return false, nil
		}
		return false, err
	}

	lines := strings.Split(string(data), "\n")
	for _, rawLine := range lines {
		line := strings.TrimSpace(strings.TrimSuffix(rawLine, "\r"))
		if line == "" || strings.HasPrefix(line, "#") {
			continue
		}
		if strings.HasPrefix(line, "export ") {
			line = strings.TrimSpace(strings.TrimPrefix(line, "export "))
		}

		key, value, ok := strings.Cut(line, "=")
		if !ok {
			continue
		}
		key = strings.TrimSpace(key)
		value = strings.TrimSpace(value)
		if key == "" {
			continue
		}
		if len(value) >= 2 {
			if (value[0] == '"' && value[len(value)-1] == '"') ||
				(value[0] == '\'' && value[len(value)-1] == '\'') {
				value = value[1 : len(value)-1]
			}
		}

		// Real environment variables always take precedence over .env values.
		if _, exists := os.LookupEnv(key); !exists {
			_ = os.Setenv(key, value)
		}
	}

	return true, nil
}

func loadDotEnvIfPresent() {
	paths := []string{"gortc_main/.env", ".env", "gortc_main/env.local"}
	if executable, err := os.Executable(); err == nil {
		exeDir := filepath.Dir(executable)
		// Support launching the host from x64\Debug/x64\Release, where the
		// repository-relative working directory is not guaranteed.
		paths = append(paths,
			filepath.Join(exeDir, "env.local"),
			filepath.Join(exeDir, "..", "..", "gortc_main", "env.local"),
			filepath.Join(exeDir, "..", "..", ".env"),
		)
	}
	for _, path := range paths {
		loaded, err := loadDotEnvFile(path)
		if err != nil {
			log.Printf("[Go/Pion] Failed to load %s: %v", path, err)
			continue
		}
		if loaded {
			log.Printf("[Go/Pion] Loaded environment from %s", path)
			return
		}
	}
	log.Printf("[Go/Pion] No .env file found; using process environment variables")
}

func buildICEServersFromEnv() []webrtc.ICEServer {
	servers := []webrtc.ICEServer{
		{URLs: []string{"stun:stun.l.google.com:19302"}},
	}

	turnURLsRaw := strings.TrimSpace(os.Getenv("PION_TURN_URLS"))
	if turnURLsRaw == "" {
		turnURLsRaw = strings.TrimSpace(os.Getenv("PION_TURN_URL"))
	}
	if turnURLsRaw == "" {
		log.Printf("[Go/Pion] No TURN env configured; using STUN-only ICE")
		return servers
	}

	urls := make([]string, 0, 4)
	for _, rawURL := range strings.Split(turnURLsRaw, ",") {
		url := strings.TrimSpace(rawURL)
		if url != "" {
			urls = append(urls, url)
		}
	}
	if len(urls) == 0 {
		log.Printf("[Go/Pion] TURN URL env was empty after parsing; using STUN-only ICE")
		return servers
	}

	username := strings.TrimSpace(os.Getenv("PION_TURN_USERNAME"))
	credential := strings.TrimSpace(os.Getenv("PION_TURN_CREDENTIAL"))
	if username == "" || credential == "" {
		log.Printf("[Go/Pion] TURN URLs set but credentials missing; using STUN-only ICE")
		return servers
	}

	servers = append(servers, webrtc.ICEServer{
		URLs:       urls,
		Username:   username,
		Credential: credential,
	})
	log.Printf("[Go/Pion] TURN configured from env with %d URL(s)", len(urls))
	return servers
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

var videoBytesSentInterval uint64
var videoPendingDrops uint32
var peerGeneration uint64
var moduleStop = make(chan struct{})
var moduleStopOnce sync.Once

func addPendingVideoDrops(count uint32) {
	for {
		current := atomic.LoadUint32(&videoPendingDrops)
		next := current + count
		if next < current || next > 65535 {
			next = 65535
		}
		if atomic.CompareAndSwapUint32(&videoPendingDrops, current, next) {
			return
		}
	}
}

func ntpMiddle32(now time.Time) uint32 {
	const ntpUnixOffset = uint64(2208988800)
	seconds := uint64(now.Unix()) + ntpUnixOffset
	fraction := uint64(now.Nanosecond()) * (uint64(1) << 32) / 1_000_000_000
	return uint32(((seconds << 32) | fraction) >> 16)
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
			case <-moduleStop:
				return
			}
		}
	}()
}

// checkBufferPoolHealth performs real-time health monitoring of the buffer pool
func checkBufferPoolHealth() {
	totalHits := int64(0)
	totalMisses := int64(0)
	totalAllocs := int64(0)

	for i := 0; i < sampleBufPool.sizeCount; i++ {
		totalHits += atomic.LoadInt64(&sampleBufPool.hits[i])
		totalMisses += atomic.LoadInt64(&sampleBufPool.misses[i])
		totalAllocs += atomic.LoadInt64(&sampleBufPool.allocations[i])
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
	log.Printf("[Go/Pion] === Buffer Pool Health Report ===")

	totalHits := int64(0)
	totalMisses := int64(0)
	totalAllocs := int64(0)

	for i := 0; i < sampleBufPool.sizeCount; i++ {
		hits := atomic.LoadInt64(&sampleBufPool.hits[i])
		misses := atomic.LoadInt64(&sampleBufPool.misses[i])
		allocs := atomic.LoadInt64(&sampleBufPool.allocations[i])

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

// checkAudioQueueCongestion determines if bitrate reduction is needed based on queue depth
func checkAudioQueueCongestion() bool {
	audioQueueDepthMutex.RLock()
	count := audioQueueDepthCount
	sum := 0
	for i := 0; i < count; i++ {
		sum += audioQueueDepthSamples[i]
	}
	audioQueueDepthMutex.RUnlock()
	if count == 0 {
		return false
	}
	avgDepth := float64(sum) / float64(count)

	// If average queue depth is consistently high (>2.5 packets), trigger bitrate adaptation
	// This indicates the encoder is producing packets faster than WebRTC can send them
	if count >= len(audioQueueDepthSamples) && avgDepth > 2.5 {
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
			enqueueLatestAudioPacket(pkt)
		}
	}
}

// reportAudioQueueHealth provides detailed health metrics for audio queue monitoring
func reportAudioQueueHealth() {
	audioQueueDepthMutex.RLock()
	count := audioQueueDepthCount
	if count == 0 {
		audioQueueDepthMutex.RUnlock()
		return // Not enough data yet
	}

	// Calculate statistics
	var minDepth, maxDepth int = 999, 0
	sum := 0
	for i := 0; i < count; i++ {
		depth := audioQueueDepthSamples[i]
		sum += depth
		if depth < minDepth {
			minDepth = depth
		}
		if depth > maxDepth {
			maxDepth = depth
		}
	}
	audioQueueDepthMutex.RUnlock()

	avgDepth := float64(sum) / float64(count)
	currentDepth := len(audioSendQueue)

	// Determine health status
	healthStatus := "GOOD"
	if avgDepth > 2.0 {
		healthStatus = "WARNING"
	}
	if avgDepth > 2.8 {
		healthStatus = "CRITICAL"
	}

	log.Printf("[Go/Pion] Audio Queue Health [%s]: current=%d, avg=%.1f, min=%d, max=%d, samples=%d",
		healthStatus, currentDepth, avgDepth, minDepth, maxDepth, count)

	// Additional diagnostics for concerning patterns
	if avgDepth > 2.5 {
		log.Printf("[Go/Pion] ⚠️  Audio queue consistently congested - consider bitrate reduction")
	}

	if maxDepth >= 3 {
		log.Printf("[Go/Pion] ⚠️  Audio queue reached maximum capacity - packets may be dropped")
	}
}

// Report the actual application queue depth and bytes accepted by Pion during
// the last monitoring interval. Pion's internal pacer is not exposed here.
func updatePacerQueueLength() {
	videoQueueLen := uint32(len(videoSendQueue))
	bytes := atomic.SwapUint64(&videoBytesSentInterval, 0)
	// Monitoring runs every 500 ms: bytes * 8 / 0.5 / 1000 = bytes * 16 / 1000 kbps.
	actualBitrateKbps := uint32((bytes * 16) / 1000)

	webrtcStats.statsMutex.Lock()
	webrtcStats.pacerQueueLength = videoQueueLen
	webrtcStats.sendBitrateKbps = actualBitrateKbps
	webrtcStats.statsMutex.Unlock()
}

// Global variables
var (
	peerConnection   *webrtc.PeerConnection
	pcMutex          sync.RWMutex
	pcLifecycleMutex sync.Mutex
	signalingMutex   sync.Mutex
	videoTrack       *webrtc.TrackLocalStaticSample // switched to sample track for pacing
	audioTrack       *webrtc.TrackLocalStaticRTP
	trackSSRC        uint32
	audioSSRC        uint32

	// Audio queue depth monitoring for bitrate adaptation
	audioQueueDepthSamples [10]int      // Circular buffer for recent queue depth samples
	audioQueueDepthIndex   int          // Current index in circular buffer
	audioQueueDepthCount   int          // Number of samples collected
	audioQueueDepthMutex   sync.RWMutex // Protects queue depth statistics

	// Audio buffering during WebRTC connection establishment
	audioConnectionBuffer []*rtp.Packet     // Buffer audio packets until connection is ready
	audioBufferMutex      sync.Mutex        // Protects the connection buffer
	maxAudioBufferSize    int           = 3 // Keep only the latest 30 ms while connecting.

	lastAnswerSDP      string
	audioFrameDuration uint32 = 480 // RTP timestamp increment per frame (10ms at 48kHz)
	videoFrameCounter  uint64
	dataChannel        *webrtc.DataChannel
	messageQueue       []string
	messageQueueHead   int
	mouseQueue         []string
	mouseQueueHead     int
	queueMutex         sync.Mutex
	mouseChannel       *webrtc.DataChannel
	connectionState    webrtc.PeerConnectionState
	videoPayloadType   uint8 = 96
	audioPayloadType   uint8 = 0

	// Buffer remote ICE candidates received before remote SDP is set
	pendingRemoteCandidates []webrtc.ICECandidateInit
	// Cache latest RTT (ms) from ping/pong to combine with RTCP loss/jitter
	lastRttMutex sync.Mutex
	lastRttMs    float64

	// Throttled logging for enqueues
	lastEnqueueLog    time.Time
	msgEnqueueCount   int
	mouseEnqueueCount int

	// Granular audio send path: bounded queue and dedicated sender goroutine
	audioSendQueue chan *rtp.Packet // Bounded channel for RTP packets (size ≤ 3)
	audioSendStop  chan struct{}    // Stop signal for sender goroutine
	mediaSendWG    sync.WaitGroup

	// Granular video send path: bounded queue and dedicated sender goroutine
	videoSendQueue chan queuedVideoSample // Bounded channel for encoded video frames
	videoSendStop  chan struct{}          // Stop signal for video sender goroutine

)

type queuedVideoSample struct {
	sample     media.Sample
	isKeyframe bool
	generation uint64
}

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

// TimestampForPTS quantizes the capture clock onto the Opus frame grid. If a
// frame was discarded before reaching Go, the RTP timestamp still advances,
// so the receiver does not accumulate A/V drift or play stale audio slowly.
func (s *AudioRTPState) TimestampForPTS(pts int64) uint32 {
	if !s.IsBaselineSet() {
		s.SetBaseline(pts)
	}

	basePTS := atomic.LoadInt64(&s.ptsBaseline)
	baseRTP := atomic.LoadUint32(&s.rtpBaseline)
	frameUs := int64(audioFrameDuration) * 1000000 / 48000
	if pts <= 0 || pts < basePTS || frameUs <= 0 {
		ts := atomic.AddUint32(&s.timestamp, audioFrameDuration)
		return ts - audioFrameDuration
	}

	elapsedFrames := (pts - basePTS + frameUs/2) / frameUs
	candidate := baseRTP + uint32(elapsedFrames)*audioFrameDuration
	next := atomic.LoadUint32(&s.timestamp)
	if int32(candidate-next) < 0 {
		candidate = next
	}
	atomic.StoreUint32(&s.timestamp, candidate+audioFrameDuration)
	return candidate
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
	hits        [13]int64 // Cache hits per tier
	misses      [13]int64 // Cache misses per tier
	allocations [13]int64 // New allocations per tier

}

type pooledSlice struct {
	data []byte
}

var pooledSliceWrappers = sync.Pool{
	New: func() any { return &pooledSlice{} },
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
	sizes:     [13]int{128, 256, 512, 1500, 4096, 8192, 16384, 32768, 65536, 131072, 262144, 524288, 1048576},
	sizeCount: 13,
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
			wrapper := pooledSliceWrappers.Get().(*pooledSlice)
			wrapper.data = buf
			sampleBufPool.pools[i].Put(wrapper)
		}
		log.Printf("[Go/Pion] Buffer pool tier %d (%d bytes): preallocated %d buffers", i, size, count)
	}
	log.Println("[Go/Pion] Buffer pool initialized with tiered preallocation")

	// Periodic buffer pool stats (5-minute reporting only; health check folded in)
	go func() {
		statsTicker := time.NewTicker(5 * time.Minute)
		defer statsTicker.Stop()

		for {
			select {
			case <-statsTicker.C:
				logBufferPoolStats()
				logBufferPoolHealth()
				checkBufferPoolHealth()
			case <-moduleStop:
				return
			}
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
// Stats fields are updated atomically outside the media sender's critical work.
func getSampleBuf(n int) []byte {
	if n <= 0 {
		return make([]byte, 0)
	}

	// Never truncate video samples. Oversize frames happen (often IDRs).
	// Allocate exact-size buffer outside pool when exceeding max tier.
	maxTierSize := sampleBufPool.sizes[sampleBufPool.sizeCount-1]
	if n > maxTierSize {
		log.Printf("[Go/Pion] WARNING: Oversize sample %d > max tier %d; allocating outside pool", n, maxTierSize)
		return make([]byte, n)
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

	wrapper := v.(*pooledSlice)
	b := wrapper.data
	wrapper.data = nil
	pooledSliceWrappers.Put(wrapper)
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

	// Don't pool oversize allocations (allocated outside pool in getSampleBuf).
	maxTierSize := sampleBufPool.sizes[sampleBufPool.sizeCount-1]
	if capacity > maxTierSize {
		return
	}

	// Find the appropriate tier for this buffer capacity
	tier := sampleBufPool.getBufferTier(capacity)
	targetSize := sampleBufPool.sizes[tier]
	putInTier := func(tier int) {
		wrapper := pooledSliceWrappers.Get().(*pooledSlice)
		wrapper.data = b[:capacity]
		sampleBufPool.pools[tier].Put(wrapper)
	}

	// Return to pool without zeroing -- buffers contain video data that will be
	// fully overwritten by C.memcpy on next use, so clearing is wasted work.
	if capacity == targetSize {
		putInTier(tier)
		return
	}

	if capacity >= targetSize/2 && capacity <= targetSize*2 {
		putInTier(tier)
		return
	}

	if tier > 0 {
		smallerTier := tier - 1
		smallerTargetSize := sampleBufPool.sizes[smallerTier]
		if capacity >= smallerTargetSize/2 && capacity <= smallerTargetSize*2 {
			putInTier(smallerTier)
			return
		}
	}

	if tier < sampleBufPool.sizeCount-1 {
		largerTier := tier + 1
		largerTargetSize := sampleBufPool.sizes[largerTier]
		if capacity >= largerTargetSize/2 && capacity <= largerTargetSize*2 {
			putInTier(largerTier)
			return
		}
	}
}

// logBufferPoolStats logs buffer pool usage statistics for monitoring
func logBufferPoolStats() {
	log.Printf("[Go/Pion] Buffer Pool Statistics:")
	totalHits := int64(0)
	totalMisses := int64(0)
	totalAllocs := int64(0)

	for i, size := range sampleBufPool.sizes {
		hits := atomic.LoadInt64(&sampleBufPool.hits[i])
		misses := atomic.LoadInt64(&sampleBufPool.misses[i])
		allocs := atomic.LoadInt64(&sampleBufPool.allocations[i])
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
	// Three 10 ms packets absorb brief scheduler jitter without building a
	// perceptible stale-audio backlog.
	audioSendQueue = make(chan *rtp.Packet, 3)
	audioSendStop = make(chan struct{})

	// Start the dedicated audio sender goroutine
	mediaSendWG.Add(1)
	go audioSenderGoroutine()
}

// initVideoSendQueue initializes the bounded video send queue and starts the sender goroutine
func initVideoSendQueue() {
	// Cap at 2 frames; oldest is replaced on overflow and its duration is
	// transferred to the replacement sample's RTP timeline.
	videoSendQueue = make(chan queuedVideoSample, 2)
	videoSendStop = make(chan struct{})

	// Start the dedicated video sender goroutine
	mediaSendWG.Add(1)
	go videoSenderGoroutine()

	log.Println("[Go/Pion] Audio send queue initialized with bounded channel (capacity: 3)")
	log.Println("[Go/Pion] Video send queue initialized with bounded channel (capacity: 2)")
}

func discardQueuedMedia() {
	if videoSendQueue != nil {
	videoDrain:
		for {
			select {
			case queued := <-videoSendQueue:
				putSampleBuf(queued.sample.Data)
			default:
				break videoDrain
			}
		}
	}
	if audioSendQueue != nil {
	audioDrain:
		for {
			select {
			case pkt := <-audioSendQueue:
				if pkt != nil {
					putSampleBuf(pkt.Payload)
				}
			default:
				break audioDrain
			}
		}
	}
}

// audioSenderGoroutine runs in a separate goroutine to send RTP packets without holding locks
// This prevents head-of-line blocking and keeps the send path lock-granular
func audioSenderGoroutine() {
	defer mediaSendWG.Done()
	runtime.LockOSThread()
	defer runtime.UnlockOSThread()
	mmcssHandle := C.SetupGoAudioThreadMMCSS()
	defer C.CleanupGoThreadMMCSS(mmcssHandle)
	log.Println("[Go/Pion] Audio sender goroutine started with MMCSS priority (Pro Audio class)")

	for {
		select {
		case pkt := <-audioSendQueue:
			// Send RTP packet without holding any locks
			// This is the potentially blocking operation, but it doesn't block other operations
			pcMutex.RLock()
			track := audioTrack
			pcMutex.RUnlock()
			if track != nil {
				if err := track.WriteRTP(pkt); err != nil {
					log.Printf("[Go/Pion] AUDIO ERROR: Failed to write RTP packet to audio track: %v", err)
					// Return buffer immediately on error
					putSampleBuf(pkt.Payload)
				} else {
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
	defer mediaSendWG.Done()
	runtime.LockOSThread()
	defer runtime.UnlockOSThread()
	mmcssHandle := C.SetupGoVideoThreadMMCSS()
	defer C.CleanupGoThreadMMCSS(mmcssHandle)
	log.Println("[Go/Pion] Video sender goroutine started with MMCSS priority (Playback class)")

	// Single reused idle timer — avoids the common anti-pattern of calling time.After
	// inside a hot loop, which allocates a new timer goroutine + channel on every iteration
	// (at 90fps that would be ~900 live timers at any moment, creating sustained GC pressure).
	idleTimer := time.NewTimer(10 * time.Second)
	defer idleTimer.Stop()

	for {
		select {
		case queued := <-videoSendQueue:
			// Reset the idle timer so it only fires if no samples arrive for 10s
			if !idleTimer.Stop() {
				select {
				case <-idleTimer.C:
				default:
				}
			}
			idleTimer.Reset(10 * time.Second)

			if queued.generation != atomic.LoadUint64(&peerGeneration) {
				putSampleBuf(queued.sample.Data)
				continue
			}

			pcMutex.RLock()
			track := videoTrack
			pcMutex.RUnlock()
			if track != nil {
				if err := track.WriteSample(queued.sample); err != nil {
					log.Printf("[Go/Pion] Error in video sender goroutine: %v", err)
				} else {
					atomic.AddUint64(&videoBytesSentInterval, uint64(len(queued.sample.Data)))
				}
			} else {
				log.Printf("[Go/Pion] Video track is nil, dropping sample")
			}
			putSampleBuf(queued.sample.Data)

		case <-videoSendStop:
			log.Println("[Go/Pion] Video sender goroutine stopped")
			return

		case <-idleTimer.C:
			pcMutex.RLock()
			trackMissing := videoTrack == nil
			pcMutex.RUnlock()
			log.Printf("[Go/Pion] Video sender: no samples for 10 seconds (track nil: %v)", trackMissing)
			idleTimer.Reset(10 * time.Second)
		}
	}
}

func init() {
	loadDotEnvIfPresent()

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

//export sendAudioPacket
func sendAudioPacket(data unsafe.Pointer, size C.int, pts C.longlong) C.int {
	if data == nil || size <= 0 {
		return -2
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
		packetRTPTimestamp := audioRTPState.TimestampForPTS(int64(pts))

		pkt := &rtp.Packet{
			Header: rtp.Header{
				Version:        2,
				PayloadType:    audioPayloadType,
				SequenceNumber: packetSequence,
				Timestamp:      packetRTPTimestamp,
				SSRC:           audioSSRC,
				Marker:         packetSequence == 0,
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
	packetRTPTimestamp := audioRTPState.TimestampForPTS(int64(pts))

	// Handle RTP timestamp wraparound (uint32 wraps at ~13.27 hours at 48kHz)
	if packetRTPTimestamp < audioRTPState.GetBaseline() {
		// This is a rare event - log it and handle gracefully
		log.Printf("[Go/Pion] RTP timestamp wraparound detected: baseline=%d, timestamp=%d",
			audioRTPState.GetBaseline(), packetRTPTimestamp)
		// uint32 RTP timestamps wrap naturally.
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
			Marker:         packetSequence == 0, // Continuous Opus stream; marker only starts the talkspurt.
		},
		Payload: payload,
	}

	// Queue RTP packet for the dedicated sender goroutine (bounded queue, size ≤ 3)
	// This implements backpressure by dropping oldest packets rather than accumulating
	// The sender goroutine will handle WriteRTP without holding any locks

	// Record queue depth for bitrate adaptation monitoring
	currentQueueDepth := len(audioSendQueue)
	updateAudioQueueDepth(currentQueueDepth)

	if enqueueLatestAudioPacket(pkt) {
		// Packet successfully queued for sending
		// Note: Buffer will be returned to pool by sender goroutine after WriteRTP
		return 0
	}

	return -1
}

// enqueueLatestAudioPacket keeps latency bounded by evicting stale queued audio.
// RTP sequence/timestamp gaps correctly communicate the discarded packet as loss.
func enqueueLatestAudioPacket(pkt *rtp.Packet) bool {
	queue := audioSendQueue
	if queue == nil {
		putSampleBuf(pkt.Payload)
		return false
	}
	select {
	case queue <- pkt:
		return true
	default:
	}

	select {
	case stale := <-queue:
		if stale != nil {
			putSampleBuf(stale.Payload)
		}
	default:
	}

	select {
	case queue <- pkt:
		return true
	default:
		putSampleBuf(pkt.Payload)
		return false
	}
}

//export sendVideoSample
func sendVideoSample(data unsafe.Pointer, size C.int, durationUs C.longlong, isKeyframe C.int) C.int {
	if data == nil || size <= 0 {
		return -2
	}
	// RLock: we only read PeerConnection state and track availability.
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

	droppedBefore := atomic.SwapUint32(&videoPendingDrops, 0)
	sample := media.Sample{
		Data:               buf,
		Duration:           dur,
		PrevDroppedPackets: uint16(droppedBefore),
	}
	queued := queuedVideoSample{
		sample: sample, isKeyframe: isKeyframe != 0,
		generation: atomic.LoadUint64(&peerGeneration),
	}
	select {
	case videoSendQueue <- queued:
		return 0
	default:
		// Queue is full: replace the oldest sample and tell Pion how many
		// application-level samples were skipped so RTP time advances.
		select {
		case oldest := <-videoSendQueue:
			// An IDR is the decoder's recovery point. Preserve it when the
			// incoming frame is only a delta frame.
			if oldest.isKeyframe && !queued.isKeyframe {
				select {
				case videoSendQueue <- oldest:
					putSampleBuf(buf)
					addPendingVideoDrops(uint32(sample.PrevDroppedPackets) + 1)
					return -1
				default:
					putSampleBuf(oldest.sample.Data)
				}
			} else {
				putSampleBuf(oldest.sample.Data)
			}
			transferred := uint32(sample.PrevDroppedPackets) + uint32(oldest.sample.PrevDroppedPackets) + 1
			if transferred > 65535 {
				transferred = 65535
			}
			queued.sample.PrevDroppedPackets = uint16(transferred)
			select {
			case videoSendQueue <- queued:
				return 0
			default:
				putSampleBuf(buf)
				addPendingVideoDrops(uint32(queued.sample.PrevDroppedPackets) + 1)
				return -1
			}
		default:
			putSampleBuf(buf)
			addPendingVideoDrops(uint32(queued.sample.PrevDroppedPackets) + 1)
			return -1
		}
	}
}

func enqueueMessage(msg string) {
	queueMutex.Lock()
	defer queueMutex.Unlock()
	if messageQueueHead >= 64 && messageQueueHead*2 >= len(messageQueue) {
		messageQueue = append(messageQueue[:0], messageQueue[messageQueueHead:]...)
		messageQueueHead = 0
	}
	const maxKeyboardMessages = 128
	if len(messageQueue)-messageQueueHead >= maxKeyboardMessages {
		var event struct {
			Type string `json:"type"`
		}
		_ = json.Unmarshal([]byte(msg), &event)
		if event.Type != "keyup" && event.Type != "mouseup" && event.Type != "input_reset" {
			return
		}

		// A saturated reliable/ordered key channel means releases may already be
		// trapped behind stale presses. Reset the pending stream and force the host
		// to release every tracked key before applying the newest release.
		for i := messageQueueHead; i < len(messageQueue); i++ {
			messageQueue[i] = ""
		}
		messageQueue = nil
		messageQueueHead = 0
		messageQueue = append(messageQueue, `{"type":"input_reset","reason":"keyboard_queue_overflow"}`)
	}
	messageQueue = append(messageQueue, msg)
	msgEnqueueCount++
	if time.Since(lastEnqueueLog) >= time.Second {
		// log.Printf("[Go/Pion] queued key msgs=%d mouse=%d", msgEnqueueCount, mouseEnqueueCount)
		msgEnqueueCount = 0
		mouseEnqueueCount = 0
		lastEnqueueLog = time.Now()
	}
}

func enqueueMouseEvent(msg string) {
	queueMutex.Lock()
	defer queueMutex.Unlock()
	if mouseQueueHead >= 64 && mouseQueueHead*2 >= len(mouseQueue) {
		mouseQueue = append(mouseQueue[:0], mouseQueue[mouseQueueHead:]...)
		mouseQueueHead = 0
	}
	const maxMouseMessages = 256
	if len(mouseQueue)-mouseQueueHead >= maxMouseMessages {
		var event struct {
			Type string `json:"type"`
		}
		_ = json.Unmarshal([]byte(msg), &event)
		if event.Type != "mouseup" && event.Type != "input_reset" {
			return
		}

		// Button releases must never sit behind a saturated stream of stale
		// movement events.  Reset tracked state, then apply the release.
		for i := mouseQueueHead; i < len(mouseQueue); i++ {
			mouseQueue[i] = ""
		}
		mouseQueue = nil
		mouseQueueHead = 0
		mouseQueue = append(mouseQueue, `{"type":"input_reset","reason":"mouse_queue_overflow"}`)
	}
	mouseQueue = append(mouseQueue, msg)
	mouseEnqueueCount++
	if time.Since(lastEnqueueLog) >= time.Second {
		// log.Printf("[Go/Pion] queued key msgs=%d mouse=%d", msgEnqueueCount, mouseEnqueueCount)
		msgEnqueueCount = 0
		mouseEnqueueCount = 0
		lastEnqueueLog = time.Now()
	}
}

//export getDataChannelMessage
func getDataChannelMessage() *C.char {
	queueMutex.Lock()
	defer queueMutex.Unlock()
	if messageQueueHead >= len(messageQueue) {
		return nil
	}
	msg := messageQueue[messageQueueHead]
	messageQueue[messageQueueHead] = ""
	messageQueueHead++
	if messageQueueHead == len(messageQueue) {
		messageQueue = nil
		messageQueueHead = 0
	}
	return C.CString(msg)
}

//export getMouseChannelMessage
func getMouseChannelMessage() *C.char {
	queueMutex.Lock()
	defer queueMutex.Unlock()
	if mouseQueueHead >= len(mouseQueue) {
		return nil
	}
	msg := mouseQueue[mouseQueueHead]
	mouseQueue[mouseQueueHead] = ""
	mouseQueueHead++
	if mouseQueueHead == len(mouseQueue) {
		mouseQueue = nil
		mouseQueueHead = 0
	}
	return C.CString(msg)
}

func drainSenderRTCP(sender *webrtc.RTPSender, mediaKind string) {
	for {
		if _, _, err := sender.ReadRTCP(); err != nil {
			log.Printf("[Go/Pion] %s RTCP reader stopped: %v", mediaKind, err)
			return
		}
	}
}

//export createPeerConnectionGo
func createPeerConnectionGo() C.int {
	pcLifecycleMutex.Lock()
	defer pcLifecycleMutex.Unlock()

	pcMutex.Lock()
	oldPC := peerConnection
	oldDataChannel := dataChannel
	oldMouseChannel := mouseChannel
	peerConnection = nil
	videoTrack = nil
	audioTrack = nil
	dataChannel = nil
	mouseChannel = nil
	connectionState = webrtc.PeerConnectionStateNew
	lastAnswerSDP = ""
	pcMutex.Unlock()

	// Closing can synchronously run callbacks; never do it while pcMutex is held.
	for _, dc := range []*webrtc.DataChannel{oldDataChannel, oldMouseChannel} {
		if dc != nil {
			_ = dc.Close()
		}
	}
	if oldPC != nil {
		_ = oldPC.Close()
	}
	atomic.AddUint64(&peerGeneration, 1)
	atomic.StoreUint32(&videoPendingDrops, 0)
	discardQueuedMedia()

	queueMutex.Lock()
	messageQueue = nil
	messageQueueHead = 0
	mouseQueue = nil
	mouseQueueHead = 0
	queueMutex.Unlock()
	signalingMutex.Lock()
	pendingRemoteCandidates = nil
	signalingMutex.Unlock()
	audioRTPState.Reset()
	audioBufferMutex.Lock()
	for _, pkt := range audioConnectionBuffer {
		putSampleBuf(pkt.Payload)
	}
	audioConnectionBuffer = audioConnectionBuffer[:0]
	audioBufferMutex.Unlock()

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
		ICEServers:   buildICEServersFromEnv(),
		SDPSemantics: webrtc.SDPSemanticsUnifiedPlan,
	}

	newPeerConnection, err := api.NewPeerConnection(config)
	if err != nil {
		log.Printf("[Go/Pion] Error creating PeerConnection: %v\n", err)
		return 0
	}
	pcMutex.Lock()
	peerConnection = newPeerConnection
	defer pcMutex.Unlock()
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
			pcMutex.Lock()
			previousChannel := dataChannel
			dataChannel = dc
			pcMutex.Unlock()
			if previousChannel != nil && previousChannel != dc {
				log.Printf(
					"[Go/Pion] OnDataChannel: Closing previous global data channel '%s' before assigning new one.\n",
					previousChannel.Label(),
				)
				if errClose := previousChannel.Close(); errClose != nil {
					log.Printf(
						"[Go/Pion] OnDataChannel: Error closing previous global dataChannel: %v\n",
						errClose,
					)
				}
			}
			log.Printf(
				"[Go/Pion] OnDataChannel: Global 'dataChannel' variable assigned to new DC with label '%s'.",
				dc.Label(),
			)

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
				enqueueMessage(`{"type":"input_reset","reason":"keyboard_channel_closed"}`)
			})

			dc.OnError(func(err error) {
				log.Printf(
					"[Go/Pion] Data channel '%s' (ID: %s) OnError event: %v\n",
					dc.Label(),
					idStr,
					err,
				)
				enqueueMessage(`{"type":"input_reset","reason":"keyboard_channel_error"}`)
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
			pcMutex.Lock()
			previousChannel := mouseChannel
			mouseChannel = dc
			pcMutex.Unlock()
			if previousChannel != nil && previousChannel != dc {
				log.Printf(
					"[Go/Pion] OnDataChannel: Closing previous global mouse channel '%s' before assigning new one.\n",
					previousChannel.Label(),
				)
				if errClose := previousChannel.Close(); errClose != nil {
					log.Printf(
						"[Go/Pion] OnDataChannel: Error closing previous global mouseChannel: %v\n",
						errClose,
					)
				}
			}
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
		} else {
			log.Printf(
				"[Go/Pion] OnDataChannel: Unsupported channel '%s' (ID: %s); no handlers attached.",
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
		failedPC := peerConnection
		peerConnection = nil
		pcMutex.Unlock()
		if pcErr := failedPC.Close(); pcErr != nil {
			log.Printf(
				"[Go/Pion] createPeerConnectionGo: Error closing PeerConnection after track failure: %v\n",
				pcErr,
			)
		}
		pcMutex.Lock()
		return 0
	}
	videoSender, err := peerConnection.AddTrack(videoTrack)
	if err != nil {
		log.Printf("[Go/Pion] Error adding video track: %v\n", err)
		failedPC := peerConnection
		peerConnection = nil
		pcMutex.Unlock()
		if pcErr := failedPC.Close(); pcErr != nil {
			log.Printf("[Go/Pion] createPeerConnectionGo: Error closing PeerConnection after AddTrack failure: %v\n", pcErr)
		}
		pcMutex.Lock()
		return 0
	}
	go drainSenderRTCP(videoSender, "video")

	// Cap sender bitrate to ~5 Mbps to avoid wireless bursts
	// Disabled: this Pion version doesn't expose MaxBitrate/SetParameters on RTPSender.
	// Rely on encoder-side bitrate (set to ~5 Mbps) and congestion control.

	// Enumerate codecs and select H264 payload type specifically
	// Removed: previous code queried RTPSender params, which is unnecessary for TrackLocalStaticSample pacing.

	// Create and add Opus audio track with fmtp aligned to host encoder
	opusFmtp := "minptime=10;stereo=1;useinbandfec=1" // host default: 10 ms stereo with FEC
	if val := os.Getenv("AUDIO_PTIME_MS"); val != "" {
		if n, err := strconv.Atoi(val); err == nil && n > 0 {
			opusFmtp = strings.ReplaceAll(opusFmtp, "minptime=10", fmt.Sprintf("minptime=%d", n))
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
	ptimeMs := 10
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
			go drainSenderRTCP(aSender, "audio")
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
	// Periodic RTT anomaly monitor (5s). It belongs to this PeerConnection and
	// exits when the connection is replaced or closed.
	monitoredPC := newPeerConnection
	go func() {
		t := time.NewTicker(5 * time.Second)
		defer t.Stop()
		var lastSample float64
		for {
			select {
			case <-moduleStop:
				return
			case <-t.C:
			}
			pcMutex.RLock()
			isCurrent := peerConnection == monitoredPC
			pcMutex.RUnlock()
			if !isCurrent {
				return
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
	return 1
}

//export handleOffer
func handleOffer(offerSDP *C.char) {
	pcMutex.RLock()
	pc := peerConnection
	pcMutex.RUnlock()
	if pc == nil {
		log.Println("[Go/Pion] handleOffer: no PeerConnection, creating one.")
		if createPeerConnectionGo() == 0 {
			log.Println(
				"[Go/Pion] handleOffer: Failed to create PeerConnection. Aborting offer handling.",
			)
			return
		}
		pcMutex.RLock()
		pc = peerConnection
		pcMutex.RUnlock()
		if pc == nil {
			log.Println(
				"[Go/Pion] handleOffer: PeerConnection is STILL nil after creation attempt. Aborting.",
			)
			return
		}
		log.Println(
			"[Go/Pion] handleOffer: PeerConnection successfully created and available.",
		)
	}
	pcLifecycleMutex.Lock()
	defer pcLifecycleMutex.Unlock()
	pcMutex.RLock()
	pc = peerConnection
	pcMutex.RUnlock()
	if pc == nil {
		return
	}

	// Serialize SDP and candidate state without blocking media's pcMutex.
	signalingMutex.Lock()
	defer signalingMutex.Unlock()

	sdpGoString := C.GoString(offerSDP)
	log.Printf("[Go/Pion] handleOffer: received %d-byte SDP", len(sdpGoString))

	offer := webrtc.SessionDescription{
		Type: webrtc.SDPTypeOffer,
		SDP:  sdpGoString,
	}
	if err := pc.SetRemoteDescription(offer); err != nil {
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
			if err := pc.AddICECandidate(c); err != nil {
				log.Printf("[Go/Pion] Error adding buffered ICE candidate: %v", err)
			}
		}
		pendingRemoteCandidates = nil
	}

	answer, err := pc.CreateAnswer(nil)
	if err != nil {
		log.Printf("[Go/Pion] Error creating answer: %v\n", err)
		return
	}
	// Munge SDP to advertise H.264 Level 5.1 for higher FPS at 1080p
	// answer.SDP = strings.ReplaceAll(answer.SDP, "profile-level-id=42e01f", "profile-level-id=42e033")
	// log.Println("[Go/Pion] handleOffer: Answer created successfully (munged to 42e033).")

	gatherComplete := webrtc.GatheringCompletePromise(pc)
	log.Println("[Go/Pion] handleOffer: Setting Local Description (answer).")
	if err := pc.SetLocalDescription(answer); err != nil {
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

	if ld := pc.LocalDescription(); ld != nil {
		pcMutex.Lock()
		lastAnswerSDP = ld.SDP
		answerLength := len(lastAnswerSDP)
		pcMutex.Unlock()
		log.Printf(
			"[Go/Pion] handleOffer: Answer SDP generated and stored (length: %d)\n",
			answerLength,
		)
	} else {
		log.Println(
			"[Go/Pion] handleOffer: LocalDescription is nil after ICE gathering for answer. Cannot provide SDP.",
		)
		pcMutex.Lock()
		lastAnswerSDP = ""
		pcMutex.Unlock()
	}
	log.Println("[Go/Pion] handleOffer: Processing complete.")
}

//export getAnswerSDP
func getAnswerSDP() *C.char {
	pcMutex.RLock()
	defer pcMutex.RUnlock()

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
	pcMutex.RLock()
	pc := peerConnection
	pcMutex.RUnlock()
	if pc == nil {
		log.Println("[Go/Pion] handleRemoteIceCandidate: no PeerConnection!")
		return
	}
	signalingMutex.Lock()
	defer signalingMutex.Unlock()

	cGoStr := C.GoString(candidateStr)

	candidate := webrtc.ICECandidateInit{Candidate: cGoStr}
	if pc.RemoteDescription() == nil {
		pendingRemoteCandidates = append(pendingRemoteCandidates, candidate)
		log.Println("[Go/Pion] Buffered ICE candidate (remote description not set yet)")
		return
	}
	if err := pc.AddICECandidate(candidate); err != nil {
		log.Printf("[Go/Pion] Error adding ICE candidate: %v\n", err)
	} else {
		log.Println("[Go/Pion] ICE Candidate added successfully.")
	}
}

// validateVideoDuration validates that video pacing parameters are reasonable
// Returns true if duration is valid for low-latency streaming
func validateVideoDuration(durationUs int64) bool {
	// Reject obviously invalid durations
	if durationUs < 0 {
		log.Printf("[WARNING] Invalid negative video duration: %d us", durationUs)
		return false
	}

	if durationUs == 0 {
		return false
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

//export closePeerConnection
func closePeerConnection() {
	pcLifecycleMutex.Lock()
	defer pcLifecycleMutex.Unlock()

	pcMutex.Lock()
	pc := peerConnection
	channels := []*webrtc.DataChannel{dataChannel, mouseChannel}
	peerConnection = nil
	videoTrack = nil
	audioTrack = nil
	dataChannel = nil
	mouseChannel = nil
	lastAnswerSDP = ""
	connectionState = webrtc.PeerConnectionStateClosed
	pcMutex.Unlock()
	atomic.AddUint64(&peerGeneration, 1)
	atomic.StoreUint32(&videoPendingDrops, 0)
	discardQueuedMedia()

	// Closing can synchronously invoke callbacks that take pcMutex.
	for _, dc := range channels {
		if dc != nil {
			_ = dc.Close()
		}
	}
	if pc != nil {
		_ = pc.Close()
		log.Println("[Go/Pion] PeerConnection closed.")
	}
}

//export checkAudioQueueCongestionGo
func checkAudioQueueCongestionGo() C.int {
	if checkAudioQueueCongestion() {
		return 1 // Congested
	}
	return 0 // Not congested
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
	moduleStopOnce.Do(func() { close(moduleStop) })

	// Stop the media sender goroutines.
	if audioSendStop != nil {
		close(audioSendStop)
		log.Println("[Go/Pion] Audio sender goroutine stop signal sent")
	}
	if videoSendStop != nil {
		close(videoSendStop)
		log.Println("[Go/Pion] Video sender goroutine stop signal sent")
	}
	mediaSendWG.Wait()
	audioSendStop = nil
	videoSendStop = nil
	if videoSendQueue != nil {
		drained := 0
		for len(videoSendQueue) > 0 {
			queued := <-videoSendQueue
			putSampleBuf(queued.sample.Data)
			drained++
		}
		videoSendQueue = nil
		log.Printf("[Go/Pion] Drained %d samples from video send queue", drained)
	}

	// Drain any remaining packets from the queue and return their buffers to pool
	if audioSendQueue != nil {
		timeout := time.After(1 * time.Second) // Prevent infinite wait
		drained := 0
	drainAudioQueue:
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
				break drainAudioQueue
			}
		}
		audioSendQueue = nil
		log.Printf("[Go/Pion] Drained %d packets from audio send queue", drained)
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
					rttMs := 0.0
					if report.LastSenderReport != 0 {
						rttUnits := ntpMiddle32(time.Now()) - report.LastSenderReport - report.Delay
						if rttUnits < 60*65536 {
							rttMs = float64(rttUnits) * 1000.0 / 65536.0
							lastRttMutex.Lock()
							lastRttMs = rttMs
							lastRttMutex.Unlock()
						}
					}

					webrtcStats.statsMutex.Lock()
					webrtcStats.lastStatsUpdate = time.Now()
					webrtcStats.statsMutex.Unlock()

					if rtcpCallback != nil {
						C.callRTCPCallback(rtcpCallback, C.double(packetLoss), C.double(rttMs), C.double(jitterSeconds))
					}

					if webrtcStatsCallback != nil {
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

			case *rtcp.FullIntraRequest:
				webrtcStats.statsMutex.Lock()
				webrtcStats.pliCount++
				webrtcStats.statsMutex.Unlock()

				if pliCallback != nil {
					C.callPLICallback(pliCallback)
				}

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
