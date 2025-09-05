#pragma once

#include <Windows.h>
#include <string>
#include <thread>
#include <atomic>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <audiopolicy.h>
#include <avrt.h>  // MMCSS (Multimedia Class Scheduler Service)
#include <dmo.h>   // DirectX Media Objects
#include <wmcodecdsp.h>  // Windows Media Codec DSP
#include <uuids.h> // DMO CLSIDs
#include <vector>
#include <memory>
#include <chrono>
#include <cmath>  // For math functions like sqrt, sinf
#include <queue>  // For std::queue
#include <mutex>  // For std::mutex, std::lock_guard, std::unique_lock
#include <condition_variable>  // For std::condition_variable
#include <wrl/client.h>
#include <nlohmann/json.hpp>  // For JSON configuration
#include <fstream>  // For WAV file writing
#include <cmath>    // For std::isfinite
#include "OpusEncoder.h"

// Forward declarations and helper structs
struct RawAudioFrame {
    std::vector<float> samples;
    int64_t timestampUs;
};

// Opus parameter update structure for dynamic reconfiguration
struct OpusParameterUpdate {
    int bitrate = 0;           // New bitrate in bps (0 = no change)
    int expectedLossPerc = -1; // New expected loss percentage (-1 = no change)
    int complexity = -1;       // New complexity (-1 = no change)
    int fecEnabled = -1;       // FEC enable/disable (-1 = no change, 0 = disable, 1 = enable)
};

// WAV file header structure for debugging output
struct WAVHeader {
    char riff[4] = {'R', 'I', 'F', 'F'};
    uint32_t fileSize = 0;
    char wave[4] = {'W', 'A', 'V', 'E'};
    char fmt[4] = {'f', 'm', 't', ' '};
    uint32_t fmtSize = 16;
    uint16_t audioFormat = 1;      // PCM
    uint16_t numChannels = 2;
    uint32_t sampleRate = 48000;
    uint32_t byteRate = 0;         // Calculated
    uint16_t blockAlign = 0;       // Calculated
    uint16_t bitsPerSample = 16;
    char data[4] = {'d', 'a', 't', 'a'};
    uint32_t dataSize = 0;

    void updateSizes(uint32_t totalSamples /* total interleaved samples */) {
        // totalSamples is total sample count across all channels (interleaved)
        dataSize = totalSamples * (bitsPerSample / 8);
        fileSize = 36 + dataSize;
        byteRate = sampleRate * numChannels * (bitsPerSample / 8);
        blockAlign = numChannels * (bitsPerSample / 8);
    }
};

class AudioCapturer
{
public:
    AudioCapturer();
    ~AudioCapturer();

    bool StartCapture(DWORD processId);
    void StopCapture();

    // Static method to configure audio settings from config.json
    static void SetAudioConfig(const nlohmann::json& config);

    // RTCP feedback for audio bitrate adaptation
    static void OnRtcpFeedback(double packetLoss, double rtt, double jitter);

    // Dynamic Opus parameter updates
    static void UpdateOpusParameters(int bitrate, int expectedLossPerc, int complexity, int fecEnabled = -1);

    // WAV file output for debugging
    bool StartWAVRecording(const std::string& filename = "output.wav");
    void StopWAVRecording();
    bool IsWAVRecording() const { return m_wavFile.is_open(); }

    // Shared reference clock for AV synchronization
    static void InitializeSharedReferenceClock();
    static int64_t GetSharedReferenceTimeUs();
    static void LogAVSyncStatus();

private:
    void CaptureThread(DWORD processId);
    bool ConvertPCMToFloat(const BYTE* pcmData, UINT32 numFrames, void* format, std::vector<float>& floatData);
    bool ConvertPCMToFloatInPlace(const BYTE* pcmData, UINT32 numFrames, void* format, float* outputBuffer, size_t outputBufferSize);
    void ProcessAudioFrame(const float* samples, size_t sampleCount, int64_t timestampUs);

    // Queue management methods
    void StartQueueProcessor();
    void StopQueueProcessor();
    void QueueProcessorThread();
    bool QueueAudioPacket(std::vector<uint8_t>& data, int64_t timestampUs, uint32_t rtpTimestamp);
    bool QueueAudioPacket(const uint8_t* buffer, size_t size, int64_t timestampUs, uint32_t rtpTimestamp);
    void ProcessQueuedPackets();

    // Dedicated encoder thread methods
    void StartEncoderThread();
    void StopEncoderThread();
    void EncoderThread();
    bool QueueRawFrame(std::vector<float>& samples, int64_t timestampUs);
    bool QueueRawFrameRef(const std::vector<float>& samples, int64_t timestampUs);
    void ProcessRawFrames();
    void EncodeAndQueueFrame(RawAudioFrame frame);
    void QueueParameterUpdate(int bitrate, int expectedLossPerc, int complexity, int fecEnabled = -1);
    bool CheckForParameterUpdates();

    // Audio resampling methods
    void ResampleTo48k(const float* in, size_t inFrames, uint32_t inRate, uint32_t channels, std::vector<float>& out);
    void ResampleTo48kInPlace(std::vector<float>& buffer, size_t inFrames, uint32_t inRate, uint32_t channels);
    bool ResampleTo48kInPlaceConstrained(std::vector<float>& buffer, size_t inFrames, uint32_t inRate, uint32_t channels, size_t maxBufferSize);

    // DMO Resampler methods for high-quality audio resampling
    bool InitializeDMOResampler(uint32_t inputSampleRate, uint32_t inputChannels);
    bool ProcessResamplerDMO(const float* inputData, size_t inputSamples, std::vector<float>& outputData);
    bool ProcessResamplerDMOInPlace(std::vector<float>& buffer);
    void CleanupDMOResampler();

    // Test and validation methods
    void TestResamplerQuality(uint32_t testSampleRate, uint32_t testChannels);

    // Audio latency optimization methods
    bool ValidateOpusPacketization(int frameSizeMs);
    void ReportLatencyStats();

    // Error recovery and robustness methods
    bool ReinitializeAudioClient();
    bool ShouldRetryError(HRESULT hr, int retryCount);
    void LogErrorWithContext(HRESULT hr, const std::wstring& context, int retryCount = 0);

    // Enhanced error handling methods
    int CalculateRetryDelay(int retryCount);
    bool TryRecoverFromError(HRESULT hr, const std::wstring& operation);
    void SwitchToPollingMode();
    void SwitchToEventMode();
    void LogErrorStats();
    bool IsSystemUnderLoad();

    // Ring buffer management methods
    void InitializeRingBuffer();
    bool PushFrameToRingBuffer(const std::vector<float>& frame, int64_t timestamp);
    bool PopFrameFromRingBuffer(std::vector<float>& frame, int64_t& timestamp);
    bool IsRingBufferEmpty() const;
    bool IsRingBufferFull() const;
    size_t GetRingBufferCount() const;

    // Simple MediaBuffer implementation for DMO
    class CMediaBuffer : public IMediaBuffer {
    public:
        CMediaBuffer(size_t bufferSize) : m_bufferSize(bufferSize), m_dataLength(0) {
            m_pBuffer = new BYTE[bufferSize];
        }
        ~CMediaBuffer() {
            delete[] m_pBuffer;
        }

        // IUnknown methods
        STDMETHODIMP QueryInterface(REFIID riid, void** ppv) {
            if (riid == IID_IUnknown || riid == IID_IMediaBuffer) {
                *ppv = static_cast<IMediaBuffer*>(this);
                AddRef();
                return S_OK;
            }
            return E_NOINTERFACE;
        }
        STDMETHODIMP_(ULONG) AddRef() { return InterlockedIncrement(&m_refCount); }
        STDMETHODIMP_(ULONG) Release() {
            if (InterlockedDecrement(&m_refCount) == 0) {
                delete this;
                return 0;
            }
            return m_refCount;
        }

        // IMediaBuffer methods
        STDMETHODIMP SetLength(DWORD length) {
            if (length > m_bufferSize) return E_INVALIDARG;
            m_dataLength = length;
            return S_OK;
        }
        STDMETHODIMP GetMaxLength(DWORD* pLength) {
            *pLength = static_cast<DWORD>(m_bufferSize);
            return S_OK;
        }
        STDMETHODIMP GetBufferAndLength(BYTE** ppBuffer, DWORD* pLength) {
            if (ppBuffer) *ppBuffer = m_pBuffer;
            if (pLength) *pLength = static_cast<DWORD>(m_dataLength);
            return S_OK;
        }

    private:
        BYTE* m_pBuffer;
        size_t m_bufferSize;
        size_t m_dataLength;
        LONG m_refCount = 1;
    };
    
    std::thread m_captureThread;
    std::atomic<bool> m_stopCapture;
    
    // Opus encoder
    std::unique_ptr<OpusEncoderWrapper> m_opusEncoder;
    std::vector<float> m_frameBuffer;
    size_t m_samplesPerFrame;     // Total samples per frame (frameSize * channels)
    size_t m_frameSizeSamples;    // Samples per frame per channel (for RTP timestamps)

    // Audio configuration (loaded from config.json)
    struct AudioConfig {
        int bitrate = 64000;           // Target bitrate in bps
        int complexity = 5;            // Encoder complexity (0-10)
        int expectedLossPerc = 10;     // Expected packet loss percentage
        bool enableFec = true;         // Enable forward error correction
        bool enableDtx = false;        // Enable discontinuous transmission
        int application = 2049;        // OPUS_APPLICATION_AUDIO
        int frameSizeMs = 10;          // Frame size in milliseconds
        int channels = 2;              // Number of audio channels
        bool useThreadAffinity = false;     // Use thread affinity for encoder
        DWORD encoderThreadAffinityMask = 0; // CPU affinity mask (0 = no affinity)

        // WASAPI-specific configuration for low-latency audio capture
        struct WASAPIConfig {
            bool preferExclusiveMode = true;      // Prefer exclusive mode for smaller device periods
            bool enforceEventDriven = true;       // Enforce event-driven capture (no polling fallback)
            double devicePeriodMs = 2.5;          // Preferred device period in milliseconds (2.5-5ms)
            double fallbackPeriodMs = 5.0;        // Fallback device period if preferred not available
            bool force48kHzStereo = true;          // Force 48kHz stereo format to avoid resampling
            bool preferLinearResampling = true;   // Prefer linear interpolation over DMO
            bool useDmoOnlyForHighQuality = false; // Only use DMO when exact quality required
        } wasapi;

        // Audio latency optimization configuration
        struct LatencyConfig {
            bool enforceSingleFrameBuffering = true; // Strictly enforce one frame max buffering
            int maxFrameSizeMs = 10;                 // Maximum frame size for low-latency mode (5-10ms)
            int minFrameSizeMs = 5;                  // Minimum frame size for low-latency mode
            bool strictLatencyMode = true;           // Enable strict latency optimizations
            bool warnOnBuffering = true;             // Warn when buffering exceeds one frame
            int targetOneWayLatencyMs = 20;          // Target one-way audio latency (<20ms)
            bool ultraLowLatencyProfile = false;     // Enable ultra-low-latency Opus profile (5ms, moderate bitrate)
            bool disableFecInLowLatency = true;      // Disable FEC in low-latency mode unless needed
        } latency;
    };
    static AudioConfig s_audioConfig;

    // Active instance reference for parameter updates (assumes single instance)
    static inline AudioCapturer* s_activeInstance = nullptr;

    // Performance optimization: Conditional logging macros for hot paths
    // These macros compile to no-ops in release builds, eliminating overhead
#define AUDIO_LOG_DEBUG(msg) \
    do { \
        static bool enableDebugLogs = true; /* Set to false for production */ \
        if (enableDebugLogs) { \
            std::wcout << msg << std::endl; \
        } \
    } while (0)

#define AUDIO_LOG_ERROR(msg) \
    std::wcerr << msg << std::endl

#define AUDIO_LOG_INFO(msg) \
    std::wcout << msg << std::endl

// Thread priority optimization helpers
#define AUDIO_SET_THREAD_PRIORITY_HIGH() \
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST)

#define AUDIO_SET_THREAD_PRIORITY_TIME_CRITICAL() \
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL)

    // Per-instance audio frame accumulation (replaces static variables)
    std::vector<float> m_accumulatedSamples;
    size_t m_accumulatedCount = 0;

    // Audio packet structure for queue
    struct AudioPacket {
        std::vector<uint8_t> data;
        int64_t timestampUs;
        uint32_t rtpTimestamp;
    };

    // Fixed-size buffer for encoded audio (avoid heap churn)
    // Opus packets are typically <256 bytes at 64 kbps, so 512 bytes is sufficient
    static constexpr size_t ENCODED_BUFFER_SIZE = 512; // Optimized for Opus packet sizes
    std::array<uint8_t, ENCODED_BUFFER_SIZE> m_encodedBuffer;

    // Minimal queue for audio packets (effectively zero/one to let WebRTC handle congestion)
    static constexpr size_t MAX_QUEUE_SIZE = 1;
    std::queue<AudioPacket> m_audioQueue;
    std::mutex m_queueMutex;
    std::condition_variable m_queueCondition;
    std::thread m_queueProcessorThread;
    std::atomic<bool> m_stopQueueProcessor;

    // Dedicated encoder thread and raw frame queue (minimal buffering to let WebRTC handle congestion)
    static constexpr size_t MAX_RAW_FRAME_QUEUE_SIZE = 1; // Minimal buffering for encoder thread synchronization
    std::queue<RawAudioFrame> m_rawFrameQueue;
    std::mutex m_rawFrameMutex;
    std::condition_variable m_rawFrameCondition;
    std::thread m_encoderThread;
    std::atomic<bool> m_stopEncoder;

    // Opus parameter update queue for dynamic reconfiguration
    std::queue<OpusParameterUpdate> m_parameterUpdateQueue;
    std::mutex m_parameterMutex;

    // Thread affinity control (optional for heavy loads)
    bool m_useThreadAffinity = false;
    DWORD m_encoderThreadAffinityMask = 0; // 0 = no affinity

    // Audio bitrate adaptation state (static for all instances)
    static inline std::atomic<int> s_currentAudioBitrate = 64000; // Start at default bitrate
    static inline int s_minAudioBitrate = 8000;   // Minimum: 8 kbps (very low quality)
    static inline int s_maxAudioBitrate = 128000; // Maximum: 128 kbps (high quality)
    static inline std::chrono::steady_clock::time_point s_lastAudioChange;
    static inline int s_decreaseCooldownMs = 2000;   // 2 seconds between decreases
    static inline int s_increaseIntervalMs = 10000;  // 10 seconds between increases
    static inline int s_increaseStep = 8000;         // 8 kbps increase steps
    static inline int s_cleanSamplesRequired = 30;   // 30 good samples before increase
    static inline int s_cleanSamples = 0;
    static inline double s_highLossThreshold = 0.05; // 5% packet loss triggers decrease
    static inline double s_lowLossThreshold = 0.01;  // <1% packet loss allows increase

    // FEC control thresholds
    static inline double s_fecEnableThreshold = 0.03;  // 3% packet loss enables FEC
    static inline double s_fecDisableThreshold = 0.005; // 0.5% packet loss disables FEC
    static inline bool s_fecCurrentlyEnabled = false;   // Current FEC state

    // Minimal buffering performance monitoring (optional)
    static inline bool s_enableBufferMonitoring = false;  // Enable queue depth logging
    static inline int s_bufferMonitorInterval = 1000;     // Log every N operations

    // Shared reference clock for AV synchronization
    static inline std::chrono::steady_clock::time_point s_sharedReferenceTime{};
    static inline std::atomic<bool> s_sharedReferenceInitialized = false;

    // Timing for 20ms frames
    std::chrono::high_resolution_clock::time_point m_startTime;
    int64_t m_nextFrameTime;
    uint32_t m_rtpTimestamp;

    // Audio clock timing (single source of truth)
    int64_t m_initialAudioClockTime = 0; // Initial audio clock timestamp in microseconds
    
    // COM interfaces (smart pointers)
    Microsoft::WRL::ComPtr<IMMDeviceEnumerator> m_pEnumerator;
    Microsoft::WRL::ComPtr<IMMDevice> m_pDevice;
    Microsoft::WRL::ComPtr<IAudioClient> m_pAudioClient;
    Microsoft::WRL::ComPtr<IAudioCaptureClient> m_pCaptureClient;
    Microsoft::WRL::ComPtr<IAudioSessionControl2> m_pSessionControl2;
    Microsoft::WRL::ComPtr<IAudioClock> m_pAudioClock;
    UINT64 m_audioClockFreq = 0;

    // Event-driven capture
    HANDLE m_hCaptureEvent = nullptr;
    HANDLE m_hStopEvent = nullptr;    // Stop event for clean thread shutdown
    bool m_currentlyUsingEventMode = true; // Track current capture mode

    // MMCSS (Multimedia Class Scheduler Service) for thread prioritization
    HANDLE m_hMmcssTask = nullptr;
    DWORD m_mmcssTaskIndex = 0;
    HANDLE m_hEncoderMmcssTask = nullptr;  // MMCSS for encoder thread
    DWORD m_encoderMmcssTaskIndex = 0;

    // Windows Audio Resampler DMO for high-quality resampling
    Microsoft::WRL::ComPtr<IMediaObject> m_audioResamplerDMO;
    Microsoft::WRL::ComPtr<IMediaBuffer> m_inputBuffer;
    Microsoft::WRL::ComPtr<IMediaBuffer> m_outputBuffer;
    DMO_MEDIA_TYPE m_inputMediaType = {};
    DMO_MEDIA_TYPE m_outputMediaType = {};
    bool m_resamplerInitialized = false;
    uint32_t m_currentInputSampleRate = 0;
    uint32_t m_currentInputChannels = 0;

    // Error recovery and robustness state
    int m_consecutiveErrorCount = 0;
    int m_deviceReinitCount = 0;
    DWORD m_targetProcessId = 0;
    REFERENCE_TIME m_hnsRequestedDuration = 0;
    WAVEFORMATEX* m_pwfxOriginal = nullptr;

    // Enhanced error handling and monitoring
    struct ErrorStats {
        uint64_t totalErrors = 0;
        uint64_t deviceInvalidationErrors = 0;
        uint64_t bufferErrors = 0;
        uint64_t timeoutErrors = 0;
        uint64_t pointerErrors = 0;
        uint64_t otherErrors = 0;
        uint64_t successfulRecoveries = 0;
        uint64_t modeFallbacks = 0;  // Event ↔ Polling transitions
        uint64_t lastErrorTime = 0;
        HRESULT lastErrorCode = S_OK;
    } m_errorStats;

    // Retry and backoff configuration
    struct RetryConfig {
        int maxRetries = 5;           // Maximum retry attempts
        int baseDelayMs = 100;        // Base delay between retries
        int maxDelayMs = 2000;        // Maximum delay cap
        float backoffMultiplier = 1.5f; // Exponential backoff multiplier
        bool enableModeFallback = true; // Allow event ↔ polling fallback
    } m_retryConfig;

    // Persistent audio float buffer to avoid per-packet allocations
    std::vector<float> m_floatBuffer;

    // Ring buffer for encoder frames (zero-copy audio pipeline)
    static const size_t RING_BUFFER_SIZE = 16; // 16 frames of buffering
    std::vector<std::vector<float>> m_frameRingBuffer; // Preallocated frame buffers
    size_t m_ringBufferWriteIndex = 0;
    size_t m_ringBufferReadIndex = 0;
    size_t m_ringBufferCount = 0;

    // Direct frame processing (eliminates accumulation buffer)
    std::vector<float> m_currentFrameBuffer; // Working buffer for current frame
    size_t m_currentFrameSamples = 0; // Samples accumulated in current frame
    int64_t m_currentFrameTimestamp = 0; // Timestamp for current frame

    // WAV file output methods for debugging
    bool InitializeWAVFile(const std::string& filename);
    bool WriteWAVHeader();
    bool WriteWAVData(const float* samples, size_t sampleCount);
    bool FinalizeWAVFile();
    void WriteInt16ToFile(std::ofstream& file, int16_t value);
    void WriteInt32ToFile(std::ofstream& file, int32_t value);
    static void FinalizeWAVOnExit();
    static bool TryParseGuidFromWideString(const wchar_t* str, GUID& outGuid);

    // Simple resampler state for linear interpolation between callbacks
    std::vector<float> m_resampleRemainder; // per-channel remainder sample
    double m_resamplePhase = 0.0;
    uint32_t m_lastInputRate = 0;

    // Active audio format used for capture/processing (for WAV header)
    uint32_t m_activeSampleRate = 48000;
    uint16_t m_activeChannels = 2;
    uint16_t m_activeBitsPerSample = 16;
    uint16_t m_activeWavAudioFormat = 1; // 1=PCM, 3=IEEE_FLOAT
    bool m_wavWriteRawMode = false; // write float->PCM16 samples to WAV
    GUID m_targetSessionGuid = GUID_NULL; // Target render session GUID for per-process loopback

    // WAV recording state
    std::ofstream m_wavFile;
    std::string m_wavFilename;
    WAVHeader m_wavHeader;
    uint32_t m_wavTotalSamples = 0; // total interleaved samples written
    std::mutex m_wavMutex;
    bool m_wavRecordingEnabled = false;
    int m_wavConsecutiveErrors = 0;
    uint64_t m_wavSamplesSinceHeader = 0;
};
