#include "AudioCapturer.h"
#include "ThreadPriorityManager.h"
#include <iostream>
#include <fstream>
#include <sstream>
#include <comdef.h>
#include <vector>
#include <algorithm>
#include "pion_webrtc.h"
#include <mmreg.h>
#include <ks.h>
#include <ksmedia.h>
#include <opus/opus.h>
#include <cmath>
#include <chrono>
#include <cmath>
#include <audioclientactivationparams.h>
#include <propvarutil.h>
#include <activation.h>
#pragma comment(lib, "Propsys.lib")
#include <versionhelpers.h>  // For Windows version checking
#include <tlhelp32.h>         // For process enumeration
#include <filesystem>         // For std::filesystem::path
// Define PKEY_Device_FriendlyName if not available from headers
#ifndef PKEY_Device_FriendlyName
const PROPERTYKEY PKEY_Device_FriendlyName = { { 0xa45c254e, 0xdf1c, 0x4efd, { 0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0 } }, 14 };
#endif
// Completion handler for ActivateAudioInterfaceAsync
class ActivateAudioCompletionHandler : public IActivateAudioInterfaceCompletionHandler {
public:
    explicit ActivateAudioCompletionHandler(HANDLE hEvent) : m_ref(1), m_hEvent(hEvent), m_hr(E_FAIL) {}

    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IActivateAudioInterfaceCompletionHandler)) {
            *ppv = static_cast<IActivateAudioInterfaceCompletionHandler*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return InterlockedIncrement(&m_ref); }
    STDMETHODIMP_(ULONG) Release() override {
        ULONG ulRef = InterlockedDecrement(&m_ref);
        if (ulRef == 0) delete this;
        return ulRef;
    }

    STDMETHODIMP ActivateCompleted(IActivateAudioInterfaceAsyncOperation* operation) override {
        if (!operation) { m_hr = E_POINTER; SetEvent(m_hEvent); return S_OK; }
        HRESULT hrOp = E_FAIL; IUnknown* punk = nullptr;
        operation->GetActivateResult(&hrOp, &punk);
        m_hr = hrOp;
        if (SUCCEEDED(hrOp) && punk) {
            punk->QueryInterface(__uuidof(IAudioClient), reinterpret_cast<void**>(m_audioClient.ReleaseAndGetAddressOf()));
            punk->Release();
        }
        SetEvent(m_hEvent);
        return S_OK;
    }

    HRESULT Result() const { return m_hr; }
    Microsoft::WRL::ComPtr<IAudioClient> GetClient() const { return m_audioClient; }

private:
    ~ActivateAudioCompletionHandler() = default;
    LONG m_ref;
    HANDLE m_hEvent;
    HRESULT m_hr;
    Microsoft::WRL::ComPtr<IAudioClient> m_audioClient;
};
#include <rpc.h>
bool AudioCapturer::TryParseGuidFromWideString(const wchar_t* str, GUID& outGuid)
{
    if (!str) return false;
    RPC_WSTR wstr = (RPC_WSTR)str;
    RPC_STATUS status = UuidFromStringW(wstr, &outGuid);
    return status == RPC_S_OK;
}

#define REFTIMES_PER_SEC  10000000
#define REFTIMES_PER_MILLISEC  10000

// ============================================================================
// MEMORY OPTIMIZATION: Audio Processing Buffers
// ============================================================================
// This optimization eliminates heap allocations in audio processing by:
// 1. Using thread-local pre-allocated buffers for PCM conversion
// 2. Reusing buffers for resampling operations (avoid std::vector creation)
// 3. Pre-reserving capacity for worst-case scenarios (48kHz, 8 channels)
// 4. Thread-local storage prevents cross-thread contention
//
// Benefits:
// - Eliminates heap allocations in hot audio processing path
// - Reduces GC pressure from temporary vector allocations
// - Provides predictable memory usage for real-time audio
// - Improves cache performance through buffer reuse
// ============================================================================
static const size_t MAX_AUDIO_FRAME_SAMPLES = 48000; // 1 second at 48kHz
static const size_t MAX_AUDIO_CHANNELS = 8; // Support up to 7.1 audio

// Thread-local buffers for audio processing to avoid heap allocations
static thread_local std::vector<float> g_audioConversionBuffer;
static thread_local std::vector<float> g_audioResampleBuffer;
static thread_local std::vector<float> g_audioTempBuffer;

// Initialize audio buffers with pre-reserved capacity
static void EnsureAudioBuffersCapacity() {
    if (g_audioConversionBuffer.capacity() < MAX_AUDIO_FRAME_SAMPLES * MAX_AUDIO_CHANNELS) {
        g_audioConversionBuffer.reserve(MAX_AUDIO_FRAME_SAMPLES * MAX_AUDIO_CHANNELS);
    }
    if (g_audioResampleBuffer.capacity() < MAX_AUDIO_FRAME_SAMPLES * MAX_AUDIO_CHANNELS) {
        g_audioResampleBuffer.reserve(MAX_AUDIO_FRAME_SAMPLES * MAX_AUDIO_CHANNELS);
    }
    if (g_audioTempBuffer.capacity() < MAX_AUDIO_FRAME_SAMPLES * MAX_AUDIO_CHANNELS) {
        g_audioTempBuffer.reserve(MAX_AUDIO_FRAME_SAMPLES * MAX_AUDIO_CHANNELS);
    }
}

AudioCapturer::AudioCapturer() :
    m_stopCapture(true),
    m_stopQueueProcessor(true),
    m_stopEncoder(true),
    m_wavRecordingEnabled(false),
    m_wavTotalSamples(0),
    m_wavSamplesSinceHeader(0),
    m_pEnumerator(nullptr),
    m_pDevice(nullptr),
    m_pAudioClient(nullptr),
    m_pCaptureClient(nullptr),
    m_nextFrameTime(0),
    m_rtpTimestamp(0),
    m_frameSizeSamples(480), // 10ms at 48kHz (will be updated based on config)
    m_samplesPerFrame(960), // 10ms at 48kHz stereo (will be updated based on config)
    m_accumulatedCount(0),   // Initialize accumulation counter
    m_hCaptureEvent(nullptr),
    m_hStopEvent(nullptr),
    m_hMmcssTask(nullptr),
    m_mmcssTaskIndex(0),
    m_consecutiveErrorCount(0),
    m_deviceReinitCount(0),
    m_targetProcessId(0),
    m_hnsRequestedDuration(0),
    m_pwfxOriginal(nullptr)
{
    // Initialize Opus encoder with optimized settings for low-latency gaming
    m_opusEncoder = std::make_unique<OpusEncoderWrapper>();

    // Initialize zero-copy audio pipeline with ring buffer
    InitializeRingBuffer();

    // Pre-allocate working buffers with fixed sizes to avoid dynamic resizing
    m_floatBuffer.resize(m_samplesPerFrame); // Fixed size for PCM to float conversion
    m_currentFrameBuffer.resize(m_samplesPerFrame); // Fixed size for current frame accumulation
    std::fill(m_floatBuffer.begin(), m_floatBuffer.end(), 0.0f); // Initialize to silence
    std::fill(m_currentFrameBuffer.begin(), m_currentFrameBuffer.end(), 0.0f); // Initialize to silence

    // Fixed-size buffer is already allocated, no initialization needed
    // Size: 512 bytes (optimized for Opus packets <256 bytes at 64 kbps)

    // Initialize error handling structures
    m_errorStats = {}; // Zero-initialize all error statistics
    AUDIO_LOG_INFO(L"[AudioCapturer] Enhanced error handling initialized with "
                  << m_retryConfig.maxRetries << L" max retries, "
                  << m_retryConfig.baseDelayMs << L"ms base delay, "
                  << m_retryConfig.backoffMultiplier << L"x backoff multiplier");

    // Register process shutdown hook to finalize WAV if needed
    static bool s_registeredAtExit = false;
    if (!s_registeredAtExit) {
        s_registeredAtExit = true;
        std::atexit(&AudioCapturer::FinalizeWAVOnExit);
    }
}

AudioCapturer::~AudioCapturer()
{
    StopCapture();
    // Defensive finalization in case StopCapture didn't run or process is terminating
    {
        std::lock_guard<std::mutex> lock(m_wavMutex);
        if (m_wavRecordingEnabled || m_wavFile.is_open()) {
            FinalizeWAVFile();
        }
    }
}

// Helper function to find process ID by executable name
DWORD FindProcessIdByName(const std::wstring& processName)
{
    DWORD processId = 0;
    HANDLE hSnapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (hSnapshot != INVALID_HANDLE_VALUE)
    {
        PROCESSENTRY32 pe32;
        pe32.dwSize = sizeof(PROCESSENTRY32);

        if (Process32First(hSnapshot, &pe32))
        {
            do
            {
                if (_wcsicmp(pe32.szExeFile, processName.c_str()) == 0)
                {
                    processId = pe32.th32ProcessID;
                    break;
                }
            } while (Process32Next(hSnapshot, &pe32));
        }
        CloseHandle(hSnapshot);
    }
    return processId;
}

bool AudioCapturer::StartCapture(DWORD processId, const std::string& processName)
{
    // Set up MMCSS for audio capture thread to ensure consistent timing
    static bool audioCaptureThreadConfigured = false;
    static ThreadPriorityManager::MMCSSHandle audioMMCSS; // Make static to persist
    if (!audioCaptureThreadConfigured) {
        audioCaptureThreadConfigured = true;
        // Configure audio capture thread with MMCSS "Audio" class for real-time audio
        ThreadPriorityManager::ThreadPriorityConfig audioConfig;
        audioConfig.mmcssClass = ThreadPriorityManager::MMCSSClass::Audio;
        audioConfig.taskName = "AudioCapture";
        audioConfig.enableMMCSS = true;
        audioConfig.enableTimeCritical = false; // Use high priority but not time critical
        audioConfig.threadPriority = THREAD_PRIORITY_HIGHEST;

        if (audioMMCSS.elevate(audioConfig)) {
            std::cout << "[Audio] MMCSS priority configured for audio capture thread (Audio class)" << std::endl;
        } else {
            std::cout << "[Audio] MMCSS failed, falling back to thread priority only" << std::endl;
            // Fall back to just setting thread priority without MMCSS
            HANDLE threadHandle = GetCurrentThread();
            if (threadHandle != nullptr && SetThreadPriority(threadHandle, audioConfig.threadPriority)) {
                std::cout << "[Audio] Thread priority set to HIGH (fallback)" << std::endl;
            } else {
                std::cout << "[Audio] Warning: Failed to set thread priority fallback" << std::endl;
            }
        }
    }

    m_stopCapture = false;
    
    // Store initialization parameters for potential reinitialization
    m_targetProcessId = processId;
    m_targetProcessName = processName; // Store for diagnostics
    m_hnsRequestedDuration = 20000000; // 2 seconds default (can be made configurable)

    // Set this as the active instance for parameter updates
    s_activeInstance = this;

    // Initialize shared reference clock for AV synchronization
    InitializeSharedReferenceClock();
    
    // ============================================================================
    // OPUS ENCODER CONFIGURATION - Optimized for low-latency gaming
    // ============================================================================
    // Frame Size: Configurable (default 10ms) - Lower latency than 20ms frames
    // RTP timestamps increment by frameSize per frame for correct timing
    // Total samples per frame = frameSize * channels
    // ============================================================================

    OpusEncoderWrapper::Settings settings;

    // Calculate frame size in samples from milliseconds
    int frameSizeSamples = (s_audioConfig.frameSizeMs * 48000) / 1000; // 48kHz * ms / 1000

    settings.sampleRate = 48000;                    // 48 kHz (fixed for Opus compatibility)
    settings.channels = s_audioConfig.channels;      // Configurable: 1=mono, 2=stereo
    settings.frameSize = frameSizeSamples;           // Configurable frame size
    settings.bitrate = s_audioConfig.bitrate;        // Configurable bitrate (64-96kbps recommended)
    settings.complexity = s_audioConfig.complexity;  // Configurable complexity (5-6 recommended)
    settings.useVbr = true;                         // Variable bitrate (always recommended)
    settings.constrainedVbr = true;                 // Constrain VBR peaks (recommended)
    settings.enableFec = s_audioConfig.enableFec;    // Configurable FEC
    settings.expectedLossPerc = s_audioConfig.expectedLossPerc; // Configurable loss expectation
    settings.enableDtx = false;                     // Keep DTX off for continuous game audio
    settings.application = 2049;                    // Use OPUS_APPLICATION_AUDIO for full-band game audio

    std::wcout << L"[AudioOpus] Using OPUS_APPLICATION_AUDIO (music/content) for game audio" << std::endl;

    // Apply ultra-low-latency profile optimizations
    if (s_audioConfig.latency.ultraLowLatencyProfile) {
        // Force 5ms frames for minimum algorithmic delay
        if (s_audioConfig.frameSizeMs > 5) {
            std::wcout << L"[AudioOpus] Ultra-low-latency profile: Forcing 5ms frames (was " << s_audioConfig.frameSizeMs << L"ms)" << std::endl;
            s_audioConfig.frameSizeMs = 5;
            settings.frameSize = (5 * 48000) / 1000; // Recalculate frame size
        }

        // Moderate bitrate for low-latency (balance quality vs delay)
        if (settings.bitrate > 48000) {
            std::wcout << L"[AudioOpus] Ultra-low-latency profile: Reducing bitrate to 48kbps for lower delay" << std::endl;
            settings.bitrate = 48000; // 48kbps for moderate quality with lower delay
        }

        // Disable FEC in low-latency mode unless explicitly needed
        if (s_audioConfig.latency.disableFecInLowLatency && s_audioConfig.expectedLossPerc < 5) {
            std::wcout << L"[AudioOpus] Ultra-low-latency profile: Disabling FEC (low packet loss: " << s_audioConfig.expectedLossPerc << L"%)" << std::endl;
            settings.enableFec = false;
        }

        // Lower complexity for faster encoding
        if (settings.complexity > 4) {
            std::wcout << L"[AudioOpus] Ultra-low-latency profile: Reducing complexity to 4 for faster encoding" << std::endl;
            settings.complexity = 4;
        }

        std::wcout << L"[AudioOpus] Ultra-low-latency profile activated: 5ms frames, "
                  << settings.bitrate << L" bps, complexity " << settings.complexity
                  << L", FEC " << (settings.enableFec ? L"enabled" : L"disabled") << std::endl;
    }
    
    if (!m_opusEncoder->initialize(settings)) {
        std::wcerr << L"[AudioCapturer] Failed to initialize Opus encoder" << std::endl;
        return false;
    }

    // Validate Opus packetization for low-latency mode
    if (s_audioConfig.latency.strictLatencyMode) {
        this->ValidateOpusPacketization(s_audioConfig.frameSizeMs);
    }

    // Initialize timing
    m_startTime = std::chrono::high_resolution_clock::now();
    m_nextFrameTime = 0;
    m_rtpTimestamp = 0;
    // settings.frameSize is samples per channel (e.g., 960 at 20ms, 48kHz)
    m_frameSizeSamples = settings.frameSize;                  // per-channel samples
    m_samplesPerFrame = settings.frameSize * settings.channels; // total interleaved samples
    m_frameBuffer.resize(m_samplesPerFrame);

    // Re-initialize ring buffer now that final frame sizes are known
    InitializeRingBuffer();
    
    AUDIO_LOG_INFO(L"[AudioCapturer] Initialized Opus encoder: " << settings.sampleRate << L"Hz, "
                  << settings.channels << L" channels, " << settings.frameSize << L" total samples/frame ("
                  << (settings.frameSize / settings.channels) << L" per channel), " << settings.bitrate << L" bps");

    // Log quality comparison with WAV settings
    AUDIO_LOG_INFO(L"[AudioQuality] WAV settings: 48kHz, 16-bit PCM, uncompressed");
    AUDIO_LOG_INFO(L"[AudioQuality] Streaming settings: 48kHz, Opus " << settings.bitrate << L" bps, frame "
                  << (settings.frameSize * 1000 / settings.sampleRate) << L"ms");
    
    // Start the dedicated encoder thread for offloading Opus encoding from capture thread
    StartEncoderThread();

    // Start the queue processor thread for async audio packet processing
    StartQueueProcessor();

    std::wcout << L"[AudioCapturer] DEBUG: Starting capture thread for PID=" << processId << std::endl;
    m_captureThread = std::thread(&AudioCapturer::CaptureThread, this, processId);

    // Run audio streaming diagnostics after a short delay to allow WebRTC connection to establish
    std::wcout << L"[AudioCapturer] Scheduling audio streaming diagnostics..." << std::endl;
    std::thread([this]() {
        std::this_thread::sleep_for(std::chrono::seconds(2)); // Wait 2 seconds for connection
        if (!m_stopCapture) {
            std::wcout << L"[AudioCapturer] Running audio streaming diagnostics..." << std::endl;
            diagnoseAudioStreamingGo();
        }
    }).detach();

    // Start periodic audio streaming health check
    m_audioHealthCheckThread = std::thread([this]() {
        std::wcout << L"[AudioCapturer] Audio health check thread started" << std::endl;
        while (!m_stopCapture) {
            std::this_thread::sleep_for(std::chrono::minutes(1)); // Check every minute
            if (!m_stopCapture) {
                std::wcout << L"[AudioCapturer] Periodic audio streaming health check:" << std::endl;
                diagnoseAudioStreamingGo();
            }
        }
        std::wcout << L"[AudioCapturer] Audio health check thread stopped" << std::endl;
    });

    return true;
}

void AudioCapturer::StopCapture()
{
    m_stopCapture = true;

    // Stop audio health check thread
    if (m_audioHealthCheckThread.joinable()) {
        try {
            m_audioHealthCheckThread.join();
        } catch (const std::system_error& e) {
            std::wcerr << L"[AudioCapturer] Error joining audio health check thread: " << e.what() << std::endl;
        }
    }

    // Signal the stop event to wake up the capture thread immediately
    if (m_hStopEvent) {
        SetEvent(m_hStopEvent);
    }

    if (m_captureThread.joinable()) { m_captureThread.join(); }

    // Stop encoder thread first (process remaining frames)
    StopEncoderThread();

    // Stop queue processor thread
    StopQueueProcessor();

    // Clear active instance reference
    if (s_activeInstance == this) {
        s_activeInstance = nullptr;
    }

    // Clean up DMO resampler
    CleanupDMOResampler();

    // Clear accumulation buffers to ensure clean state for next capture session
    // This prevents any leftover data from affecting subsequent captures
    m_accumulatedSamples.clear();
    m_accumulatedCount = 0;

    // Clear encoded buffer
    // Fixed-size array doesn't need clearing - data is overwritten by encoder

    // Reset timing state
    m_initialAudioClockTime = 0;
    m_nextFrameTime = 0;
    m_rtpTimestamp = 0;

    // Stop WAV recording if active
    StopWAVRecording();
}

// ============================================================================
// WAV FILE OUTPUT IMPLEMENTATION - For debugging purposes
// ============================================================================

// Maximum consecutive errors before disabling WAV recording
static const int MAX_WAV_CONSECUTIVE_ERRORS = 5;

bool AudioCapturer::StartWAVRecording(const std::string& filename)
{
    std::lock_guard<std::mutex> lock(m_wavMutex);

    if (m_wavRecordingEnabled) {
        std::wcerr << L"[AudioCapturer] WAV recording already active" << std::endl;
        return false;
    }

    std::wcout << L"[AudioCapturer] Initializing WAV recording to: " << filename.c_str() << std::endl;

    if (!InitializeWAVFile(filename)) {
        std::wcerr << L"[AudioCapturer] Failed to initialize WAV file: " << filename.c_str() << std::endl;
        return false;
    }

    m_wavRecordingEnabled = true;
    std::wcout << L"[AudioCapturer] WAV recording started successfully to: " << filename.c_str() << std::endl;
    return true;
}

void AudioCapturer::StopWAVRecording()
{
    std::lock_guard<std::mutex> lock(m_wavMutex);

    if (!m_wavRecordingEnabled) {
        return;
    }

    m_wavRecordingEnabled = false;

    if (FinalizeWAVFile()) {
        std::wcout << L"[AudioCapturer] WAV recording stopped. File: " << m_wavFilename.c_str()
                   << L" (" << m_wavTotalSamples << L" samples)" << std::endl;
    } else {
        std::wcerr << L"[AudioCapturer] Failed to finalize WAV file: " << m_wavFilename.c_str() << std::endl;
    }
}

bool AudioCapturer::InitializeWAVFile(const std::string& filename)
{
    try {
        // Close any existing file first
        if (m_wavFile.is_open()) {
            m_wavFile.close();
        }

        m_wavFilename = filename;
        m_wavTotalSamples = 0;
        m_wavSamplesSinceHeader = 0;

        // Additional safety check - ensure we have valid config
        if (s_audioConfig.channels == 0) {
            std::wcerr << L"[WAV] WARNING: Audio config not properly initialized, using defaults" << std::endl;
        }

        m_wavFile.open(filename, std::ios::binary | std::ios::out | std::ios::trunc);

        if (!m_wavFile.is_open()) {
            std::wcerr << L"[AudioCapturer] Failed to open WAV file: " << filename.c_str() << std::endl;
            return false;
        }

        // Initialize WAV header format. Force PCM16 to match WriteWAVData conversion path
        // Use safe defaults if audio device hasn't been initialized yet
        uint16_t channels = (m_activeChannels > 0) ? m_activeChannels :
                           (s_audioConfig.channels > 0) ? s_audioConfig.channels : 2;
        uint32_t sampleRate = (m_activeSampleRate > 0) ? m_activeSampleRate : 48000;

        m_wavHeader.numChannels = channels;
        m_wavHeader.sampleRate = sampleRate;
        m_wavHeader.bitsPerSample = 16;
        m_wavHeader.audioFormat = 1; // PCM

        std::wcout << L"[WAV] Creating WAV file: " << filename.c_str() << std::endl;
        std::wcout << L"[WAV] Config - Channels: " << m_wavHeader.numChannels << L", SampleRate: " << m_wavHeader.sampleRate << L", BitsPerSample: " << m_wavHeader.bitsPerSample << std::endl;
        std::wcout << L"[WAV] Header - Channels: " << m_wavHeader.numChannels << L", SampleRate: " << m_wavHeader.sampleRate << L", BitsPerSample: " << m_wavHeader.bitsPerSample << std::endl;

        m_wavHeader.updateSizes(0); // Initialize with 0 total interleaved samples
        std::wcout << L"[WAV] Initial header - fileSize: " << m_wavHeader.fileSize << L", dataSize: " << m_wavHeader.dataSize << L", byteRate: " << m_wavHeader.byteRate << std::endl;

        // Write initial header (will be overwritten with final sizes)
        if (!WriteWAVHeader()) {
            std::wcerr << L"[WAV] Failed to write initial WAV header to: " << filename.c_str() << std::endl;
            m_wavFile.close();
            return false;
        }

        std::wcout << L"[WAV] Successfully wrote initial header, file is ready for data" << std::endl;
        return true;
    }
    catch (const std::exception& e) {
        std::wcerr << L"[AudioCapturer] Exception initializing WAV file: " << e.what() << std::endl;
        if (m_wavFile.is_open()) {
            m_wavFile.close();
        }
        return false;
    }
}

bool AudioCapturer::WriteWAVHeader()
{
    if (!m_wavFile.is_open()) {
        return false;
    }

    try {
        std::wcout << L"[WAV] Writing header - RIFF, WAVE, fmt, data" << std::endl;

        // Write RIFF header
        m_wavFile.write(m_wavHeader.riff, 4);
        if (m_wavFile.fail()) {
            std::wcerr << L"[WAV] Failed to write RIFF header" << std::endl;
            return false;
        }

        // Write file size (little-endian)
        WriteInt32ToFile(m_wavFile, m_wavHeader.fileSize);

        // Write WAVE header
        m_wavFile.write(m_wavHeader.wave, 4);
        m_wavFile.write(m_wavHeader.fmt, 4);

        // Write format chunk
        WriteInt32ToFile(m_wavFile, m_wavHeader.fmtSize);
        WriteInt16ToFile(m_wavFile, m_wavHeader.audioFormat);
        WriteInt16ToFile(m_wavFile, m_wavHeader.numChannels);
        WriteInt32ToFile(m_wavFile, m_wavHeader.sampleRate);
        WriteInt32ToFile(m_wavFile, m_wavHeader.byteRate);
        WriteInt16ToFile(m_wavFile, m_wavHeader.blockAlign);
        WriteInt16ToFile(m_wavFile, m_wavHeader.bitsPerSample);

        // Write data chunk header
        m_wavFile.write(m_wavHeader.data, 4);
        WriteInt32ToFile(m_wavFile, m_wavHeader.dataSize);

        std::wcout << L"[WAV] Header written successfully" << std::endl;
        return m_wavFile.good();
    }
    catch (const std::exception& e) {
        std::wcerr << L"[WAV] Exception writing header: " << e.what() << std::endl;
        return false;
    }
}

bool AudioCapturer::WriteWAVData(const float* samples, size_t sampleCount)
{
    if (!m_wavFile.is_open() || !m_wavRecordingEnabled) {
        return false;
    }

    try {
        if (m_wavWriteRawMode) {
            // In raw mode, we expect to be called with the latest captured PCM block.
            // However this function currently receives float*. For raw writes we will skip if samples is null
            // and rely on the raw path in CaptureThread to write device bytes directly.
            return true;
        }

        if (!samples || sampleCount == 0) {
            return false;
        }

        // Convert float samples (interleaved) to 16-bit PCM and write
        bool hasNonZeroSamples = false;
        float maxAmplitude = 0.0f;
        float minAmplitude = 0.0f;

        // Debug: Check for audio signal quality (reduced frequency)
        static int wavFrameCount = 0;
        if (++wavFrameCount % 500 == 0) { // Log every 500 frames
            // Check signal levels for WAV recording
            bool hasSignal = false;
            int nonZeroCount = 0;
            float maxAmp = 0.0f, minAmp = 0.0f;
            for (size_t i = 0; i < sampleCount && i < 1000; ++i) {
                if (std::abs(samples[i]) > maxAmp) maxAmp = std::abs(samples[i]);
                if (samples[i] < minAmp) minAmp = samples[i];
                if (samples[i] != 0.0f) {
                    hasSignal = true;
                    nonZeroCount++;
                }
            }
            AUDIO_LOG_INFO(L"[WAV] Frame " << wavFrameCount << L" - Max: " << maxAmp
                          << L", Min: " << minAmp << L", Signal: " << (hasSignal ? L"YES" : L"NO"));
        }

        for (size_t i = 0; i < sampleCount; ++i) {
            float sampleValue = samples[i];
            if (std::abs(sampleValue) > maxAmplitude) maxAmplitude = std::abs(sampleValue);
            if (sampleValue < minAmplitude) minAmplitude = sampleValue;
            if (!std::isfinite(sampleValue)) sampleValue = 0.0f;
            if (sampleValue != 0.0f) hasNonZeroSamples = true;
            if (sampleValue > 1.0f) sampleValue = 1.0f;
            if (sampleValue < -1.0f) sampleValue = -1.0f;
            int16_t pcmSample = static_cast<int16_t>(sampleValue * 32767.0f);
            WriteInt16ToFile(m_wavFile, pcmSample);
        }

        m_wavFile.flush();
        m_wavTotalSamples += static_cast<uint32_t>(sampleCount);
        m_wavSamplesSinceHeader += sampleCount;

        // Periodic header rewrite
        const uint32_t headerRewriteIntervalSamples = m_wavHeader.sampleRate * m_wavHeader.numChannels; // ~1s
        if (m_wavSamplesSinceHeader >= headerRewriteIntervalSamples) {
            m_wavSamplesSinceHeader = 0;
            std::streampos cur = m_wavFile.tellp();
            if (cur != std::streampos(-1)) {
                m_wavHeader.updateSizes(m_wavTotalSamples);
                m_wavFile.seekp(0, std::ios::beg);
                if (!m_wavFile.fail()) {
                    if (WriteWAVHeader()) m_wavFile.flush();
                } else {
                    m_wavFile.clear();
                }
                m_wavFile.seekp(cur);
            }
        }

        return m_wavFile.good();
    }
    catch (const std::exception& e) {
        m_wavConsecutiveErrors++;
        try { std::wcerr << L"[AudioCapturer] WAV write error (" << m_wavConsecutiveErrors << L"): " << e.what() << std::endl; } catch (...) {}
        if (m_wavConsecutiveErrors >= MAX_WAV_CONSECUTIVE_ERRORS) {
            std::wcerr << L"[AudioCapturer] Too many consecutive WAV write errors, disabling recording" << std::endl;
            m_wavRecordingEnabled = false;
            try { if (m_wavFile.is_open()) { m_wavFile.close(); } } catch (...) {}
        }
        return false;
    }
}

bool AudioCapturer::FinalizeWAVFile()
{
    if (!m_wavFile.is_open()) {
        std::wcout << L"[WAV] File already closed, nothing to finalize" << std::endl;
        return true; // Already closed, consider it successful
    }

    try {
        std::wcout << L"[WAV] Finalizing WAV file with " << m_wavTotalSamples << L" total samples" << std::endl;

        // Update header with final sample count
        m_wavHeader.updateSizes(m_wavTotalSamples);
        std::wcout << L"[WAV] Updated header - fileSize: " << m_wavHeader.fileSize << L", dataSize: " << m_wavHeader.dataSize << std::endl;

        // Flush any pending writes
        m_wavFile.flush();
        if (m_wavFile.fail()) {
            std::wcerr << L"[WAV] Flush failed before seeking" << std::endl;
        }

        // Seek back to beginning and rewrite header
        m_wavFile.seekp(0, std::ios::beg);
        if (m_wavFile.fail()) {
            std::wcerr << L"[WAV] Failed to seek to beginning of WAV file (failbit set)" << std::endl;
            m_wavFile.clear(); // Clear error flags
            m_wavFile.seekp(0, std::ios::beg);
            if (m_wavFile.fail()) {
                std::wcerr << L"[WAV] Still failed to seek even after clearing flags" << std::endl;
                m_wavFile.close();
                return false;
            }
        }

        std::wcout << L"[WAV] Successfully seeked to file beginning" << std::endl;

        bool headerWritten = WriteWAVHeader();
        if (!headerWritten) {
            std::wcerr << L"[WAV] Failed to write final WAV header" << std::endl;
        } else {
            std::wcout << L"[WAV] Successfully wrote final WAV header" << std::endl;
        }

        // Flush the header write
        m_wavFile.flush();
        if (m_wavFile.fail()) {
            std::wcerr << L"[WAV] Flush failed after writing header" << std::endl;
        }

        // Close file
        m_wavFile.close();
        std::wcout << L"[WAV] File closed successfully" << std::endl;

        // Reset state
        m_wavTotalSamples = 0;
        m_wavFilename.clear();

        return headerWritten;
    }
    catch (const std::exception& e) {
        std::wcerr << L"[WAV] Exception finalizing WAV file: " << e.what() << std::endl;
        try {
            m_wavFile.close();
        } catch (...) {
            // Ignore close errors
        }
        return false;
    }
}

void AudioCapturer::WriteInt16ToFile(std::ofstream& file, int16_t value)
{
    file.put(static_cast<char>(value & 0xFF));
    file.put(static_cast<char>((value >> 8) & 0xFF));
}

void AudioCapturer::WriteInt32ToFile(std::ofstream& file, int32_t value)
{
    file.put(static_cast<char>(value & 0xFF));
    file.put(static_cast<char>((value >> 8) & 0xFF));
    file.put(static_cast<char>((value >> 16) & 0xFF));
    file.put(static_cast<char>((value >> 24) & 0xFF));
}

// ============================================================================
// QUEUE PROCESSOR IMPLEMENTATION - Async audio packet processing
// ============================================================================

void AudioCapturer::StartQueueProcessor()
{
    m_stopQueueProcessor = false;
    m_queueProcessorThread = std::thread(&AudioCapturer::QueueProcessorThread, this);
}

void AudioCapturer::StopQueueProcessor()
{
    // Check if already stopped or never started
    if (m_stopQueueProcessor || !m_queueProcessorThread.joinable()) {
        return;
    }

    {
        std::lock_guard<std::mutex> lock(m_queueMutex);
        m_stopQueueProcessor = true;
        m_queueCondition.notify_all();
    }

    // Wait for the thread to finish with a timeout to prevent hanging
    if (m_queueProcessorThread.joinable()) {
        try {
            m_queueProcessorThread.join();
        } catch (const std::system_error& e) {
            std::wcerr << L"[AudioCapturer] Error joining queue processor thread: " << e.what() << std::endl;
        }
    }
}

void AudioCapturer::QueueProcessorThread()
{
    std::wcout << L"[AudioCapturer] Queue processor thread started" << std::endl;

    while (!m_stopQueueProcessor) {
        std::unique_lock<std::mutex> lock(m_queueMutex);

        // Wait for packets or shutdown signal
        m_queueCondition.wait(lock, [this]() {
            return !m_audioQueue.empty() || m_stopQueueProcessor;
        });

        if (m_stopQueueProcessor) {
            // Process any remaining packets before shutdown
            while (!m_audioQueue.empty()) {
                AudioPacket packet = std::move(m_audioQueue.front());
                m_audioQueue.pop();

                lock.unlock();

                // Send remaining packets during shutdown (don't block shutdown)
                int result = sendAudioPacket(packet.data.data(),
                                           static_cast<int>(packet.data.size()),
                                           packet.timestampUs);
                if (result != 0) {
                    std::wcerr << L"[AudioQueue] Failed to send final audio packet during shutdown. Error: " << result << std::endl;
                }

                lock.lock();
            }
            break;
        }

        // Process queued packets (minimal queue depth for low latency)
        while (!m_audioQueue.empty() && !m_stopQueueProcessor) {
            AudioPacket packet = std::move(m_audioQueue.front());
            m_audioQueue.pop();

            // Unlock while processing to allow new packets to be queued immediately
            lock.unlock();

            // Send to WebRTC (this is the potentially blocking FFI call)
            // With minimal buffering, this should rarely block due to WebRTC congestion control
            int result = sendAudioPacket(packet.data.data(),
                                       static_cast<int>(packet.data.size()),
                                       packet.timestampUs);
            if (result != 0) {
                AUDIO_LOG_ERROR(L"[AudioQueue] Failed to send audio packet to WebRTC. Error: " << result);
            }

            lock.lock();

            // With minimal queue depth (1), check if we should yield to allow encoder thread
            // This prevents the queue processor from monopolizing CPU when queue is empty
            if (m_audioQueue.empty()) {
                std::this_thread::yield();
            }
        }
    }

    std::wcout << L"[AudioCapturer] Queue processor thread stopped" << std::endl;
}

// ============================================================================
// DEDICATED ENCODER THREAD - Moves Opus encoding off capture thread
// ============================================================================

void AudioCapturer::StartEncoderThread()
{
    m_stopEncoder = false;

    // Read thread affinity settings from config
    m_useThreadAffinity = s_audioConfig.useThreadAffinity;
    m_encoderThreadAffinityMask = s_audioConfig.encoderThreadAffinityMask;

    // Set thread affinity if requested (for heavy encoder loads)
    if (m_useThreadAffinity && m_encoderThreadAffinityMask != 0) {
        m_encoderThread = std::thread([this]() {
            // Register encoder thread with MMCSS for real-time priority
            m_hEncoderMmcssTask = AvSetMmThreadCharacteristicsW(L"Pro Audio", &m_encoderMmcssTaskIndex);
            if (m_hEncoderMmcssTask == nullptr) {
                // Fallback to "Audio" if "Pro Audio" is not available
                m_hEncoderMmcssTask = AvSetMmThreadCharacteristicsW(L"Audio", &m_encoderMmcssTaskIndex);
            }

            if (m_hEncoderMmcssTask != nullptr) {
                BOOL prioritySet = AvSetMmThreadPriority(m_hEncoderMmcssTask, AVRT_PRIORITY_HIGH);
                AUDIO_LOG_INFO(L"[AudioEncoder] MMCSS registered successfully (task index: " << m_encoderMmcssTaskIndex
                             << L", priority: HIGH)");
            } else {
                AUDIO_LOG_ERROR(L"[AudioEncoder] Failed to register encoder thread with MMCSS: " << GetLastError()
                               << L" (encoding may be affected by system load)");
            }

            // Set thread affinity before starting work
            if (!SetThreadAffinityMask(GetCurrentThread(), m_encoderThreadAffinityMask)) {
                AUDIO_LOG_ERROR(L"[AudioEncoder] Failed to set thread affinity mask: " << GetLastError());
            }

            // Set thread priority for real-time audio processing
            AUDIO_SET_THREAD_PRIORITY_TIME_CRITICAL();
            EncoderThread();
        });
        AUDIO_LOG_INFO(L"[AudioEncoder] Started with thread affinity mask: 0x" << std::hex
                      << m_encoderThreadAffinityMask << std::dec);
    } else {
        m_encoderThread = std::thread([this]() {
            // Register encoder thread with MMCSS even without thread affinity
            m_hEncoderMmcssTask = AvSetMmThreadCharacteristicsW(L"Pro Audio", &m_encoderMmcssTaskIndex);
            if (m_hEncoderMmcssTask == nullptr) {
                // Fallback to "Audio" if "Pro Audio" is not available
                m_hEncoderMmcssTask = AvSetMmThreadCharacteristicsW(L"Audio", &m_encoderMmcssTaskIndex);
            }

            if (m_hEncoderMmcssTask != nullptr) {
                BOOL prioritySet = AvSetMmThreadPriority(m_hEncoderMmcssTask, AVRT_PRIORITY_HIGH);
                AUDIO_LOG_INFO(L"[AudioEncoder] MMCSS registered successfully (task index: " << m_encoderMmcssTaskIndex
                             << L", priority: HIGH)");
            } else {
                AUDIO_LOG_ERROR(L"[AudioEncoder] Failed to register encoder thread with MMCSS: " << GetLastError()
                               << L" (encoding may be affected by system load)");
            }

            // Set thread priority for real-time audio processing
            AUDIO_SET_THREAD_PRIORITY_TIME_CRITICAL();
            EncoderThread();
        });
        AUDIO_LOG_INFO(L"[AudioEncoder] Started with MMCSS (no thread affinity)");
    }
}

void AudioCapturer::StopEncoderThread()
{
    if (m_stopEncoder) return;

    {
        std::lock_guard<std::mutex> lock(m_rawFrameMutex);
        m_stopEncoder = true;
        m_rawFrameCondition.notify_all();
    }

    if (m_encoderThread.joinable()) {
        try {
            m_encoderThread.join();
        } catch (const std::system_error& e) {
            AUDIO_LOG_ERROR(L"[AudioEncoder] Error joining encoder thread: " << e.what());
        }
    }

    // Clean up MMCSS registration
    if (m_hEncoderMmcssTask != nullptr) {
        AvRevertMmThreadCharacteristics(m_hEncoderMmcssTask);
        m_hEncoderMmcssTask = nullptr;
        m_encoderMmcssTaskIndex = 0;
        AUDIO_LOG_DEBUG(L"[AudioEncoder] MMCSS registration cleaned up");
    }
}

void AudioCapturer::EncoderThread()
{
    AUDIO_LOG_INFO(L"[AudioEncoder] Dedicated encoder thread started");

    // Ensure encoder is initialized
    if (!m_opusEncoder) {
        AUDIO_LOG_ERROR(L"[AudioEncoder] Opus encoder not initialized!");
        return;
    }

    AUDIO_LOG_INFO(L"[AudioEncoder] Encoder thread entering main loop");

    while (!m_stopEncoder) {
        // Check for frames in ring buffer (polling approach for now)
        std::vector<float> frame;
        int64_t timestamp;

        AUDIO_LOG_INFO(L"[AudioEncoder] Encoder thread polling ring buffer");

        if (PopFrameFromRingBuffer(frame, timestamp)) {
            // We have a frame to encode
            AUDIO_LOG_DEBUG(L"[AudioEncoder] Processing frame: " << frame.size() << L" samples, timestamp: " << timestamp);
            AUDIO_LOG_INFO(L"[AudioEncoder] FRAME RECEIVED FROM RING BUFFER - Size: " << frame.size() << L" samples");
            RawAudioFrame rawFrame;
            rawFrame.samples = std::move(frame);
            rawFrame.timestampUs = timestamp;
            EncodeAndQueueFrame(rawFrame);
        } else {
            // No frames available, sleep briefly to avoid busy waiting
            AUDIO_LOG_INFO(L"[AudioEncoder] No frames in ring buffer, sleeping");

            // Use shorter sleep in ultra-low-latency mode for more responsive processing
            if (s_audioConfig.latency.ultraLowLatencyProfile) {
                Sleep(0); // Yield to other threads immediately in ultra-low-latency mode
            } else {
                Sleep(1); // Standard 1ms sleep for normal operation
            }
        }

        // Check for parameter updates
        CheckForParameterUpdates();
    }

    // Process any remaining frames in ring buffer before shutdown
    std::vector<float> frame;
    int64_t timestamp;
    while (PopFrameFromRingBuffer(frame, timestamp)) {
        RawAudioFrame rawFrame;
        rawFrame.samples = std::move(frame);
        rawFrame.timestampUs = timestamp;
        EncodeAndQueueFrame(rawFrame);
    }

    std::wcout << L"[AudioEncoder] Dedicated encoder thread stopped" << std::endl;
}

void AudioCapturer::EncodeAndQueueFrame(RawAudioFrame frame)
{
    // Debug: Check input audio levels before encoding (every frame for first 10, then every 100)
    static int encodeDebugCount = 0;
    encodeDebugCount++;
    bool shouldLog = (encodeDebugCount <= 10) || (encodeDebugCount % 100 == 0);


    // Calculate RMS level to check if audio is too quiet for Opus
    float rmsLevel = 0.0f;
    for (size_t i = 0; i < frame.samples.size(); ++i) {
        rmsLevel += frame.samples[i] * frame.samples[i];
    }
    rmsLevel = sqrtf(rmsLevel / frame.samples.size());

    // Optional DC removal and auto-gain (disabled by default)
    float dcOffset = 0.0f;
    float rmsAfterDc = rmsLevel;
    if (s_audioConfig.processing.enableDCRemoval) {
        for (size_t i = 0; i < frame.samples.size(); ++i) {
            dcOffset += frame.samples[i];
        }
        dcOffset /= frame.samples.size();
        for (size_t i = 0; i < frame.samples.size(); ++i) {
            frame.samples[i] -= dcOffset;
        }
        rmsAfterDc = 0.0f;
        for (size_t i = 0; i < frame.samples.size(); ++i) {
            rmsAfterDc += frame.samples[i] * frame.samples[i];
        }
        rmsAfterDc = sqrtf(rmsAfterDc / frame.samples.size());
    }

    if (s_audioConfig.processing.enableAutoGain) {
        if (rmsAfterDc < 0.03f) {
            float boostFactor = (rmsAfterDc < 0.01f) ? 30.0f : 20.0f;
            for (size_t i = 0; i < frame.samples.size(); ++i) {
                frame.samples[i] *= boostFactor;
                if (frame.samples[i] > 1.0f) frame.samples[i] = 1.0f;
                if (frame.samples[i] < -1.0f) frame.samples[i] = -1.0f;
            }
        }
    }

    if (shouldLog) {
        float maxAmp = 0.0f, minAmp = 0.0f;
        bool hasSignal = false;
        int nonZeroCount = 0;
        size_t checkSamples = (frame.samples.size() < 1000) ? frame.samples.size() : 1000;

        for (size_t i = 0; i < checkSamples; ++i) {
            float sample = frame.samples[i];
            if (std::abs(sample) > maxAmp) maxAmp = std::abs(sample);
            if (sample < minAmp) minAmp = sample;
            if (sample != 0.0f) {
                hasSignal = true;
                nonZeroCount++;
            }
        }

        AUDIO_LOG_INFO(L"[AudioEncoder] Frame " << encodeDebugCount << L" - Input Max: " << maxAmp
                      << L", Min: " << minAmp << L", Signal: " << (hasSignal ? L"YES" : L"NO")
                      << L", Non-zero: " << nonZeroCount << L"/" << checkSamples);
    }

    // Use reusable buffer for encoding (zero allocations)
    int encodedSize = this->m_opusEncoder->encodeFrameToBuffer(frame.samples.data(),
                                                              this->m_encodedBuffer.data(),
                                                              this->m_encodedBuffer.size());

    if (encodedSize > 0) {
        // Debug: Check if encoded data is valid (not all zeros) - reduced frequency
        static int debugFrameCount = 0;
        if (++debugFrameCount % 500 == 0) {
            bool hasNonZeroData = false;
            size_t checkSize = (encodedSize < 10) ? encodedSize : 10;
            for (size_t i = 0; i < checkSize; ++i) {
                if (m_encodedBuffer[i] != 0) {
                    hasNonZeroData = true;
                    break;
                }
            }
            AUDIO_LOG_INFO(L"[AudioEncoder] Frame " << debugFrameCount << L" - size: " << encodedSize
                          << L" bytes, has data: " << (hasNonZeroData ? L"YES" : L"NO"));
        }

        // Monitor buffer usage for optimization
        static size_t maxEncodedSizeSeen = 0;
        if (encodedSize > maxEncodedSizeSeen) {
            maxEncodedSizeSeen = encodedSize;
            if (s_enableBufferMonitoring) {
                std::wcout << L"[AudioEncoder] New max Opus packet size: " << maxEncodedSizeSeen
                          << L" bytes (buffer: " << ENCODED_BUFFER_SIZE << L" bytes)" << std::endl;
            }
        }

        // Verify buffer size is sufficient (should never trigger with 512-byte buffer)
        if (encodedSize > ENCODED_BUFFER_SIZE) {
            std::wcerr << L"[AudioEncoder] ERROR: Encoded size " << encodedSize
                      << L" exceeds buffer size " << ENCODED_BUFFER_SIZE << L" - packet truncated!" << std::endl;
            return; // Don't queue corrupted packet
        }

        // Calculate RTP timestamp
        uint32_t rtpTimestamp = 0;
        if (frame.timestampUs > 0 && this->m_initialAudioClockTime > 0) {
            int64_t relativeTimeUs = frame.timestampUs - this->m_initialAudioClockTime;
            rtpTimestamp = static_cast<uint32_t>((relativeTimeUs * 48LL) / 1000LL);
        } else {
            this->m_rtpTimestamp += static_cast<uint32_t>(this->m_frameSizeSamples);
            rtpTimestamp = this->m_rtpTimestamp;
        }

        // Queue encoded packet for WebRTC transmission (zero-copy approach)
        // Pass buffer data directly without intermediate vector allocation
        if (!this->QueueAudioPacket(m_encodedBuffer.data(), encodedSize, frame.timestampUs, rtpTimestamp)) {
            AUDIO_LOG_DEBUG(L"[AudioEncoder] Failed to queue encoded packet - WebRTC congestion detected");
            // Don't retry immediately - let WebRTC congestion control work
            // The encoder thread will continue processing new frames
        } else {
            // Log successful encoding/queuing occasionally (reduced frequency)
            static int encodeCount = 0;
            if (++encodeCount % 250 == 0) {
                AUDIO_LOG_INFO(L"[AudioEncoder] Audio pipeline active: " << encodeCount << " packets encoded");
            }
        }
    } else {
        AUDIO_LOG_ERROR(L"[AudioEncoder] Failed to encode audio frame");
    }
}


bool AudioCapturer::QueueRawFrame(std::vector<float>& samples, int64_t timestampUs)
{
    std::lock_guard<std::mutex> lock(m_rawFrameMutex);

    // Check for buffering violations in strict latency mode (capture → encode path)
    if (s_audioConfig.latency.enforceSingleFrameBuffering && m_rawFrameQueue.size() >= MAX_RAW_FRAME_QUEUE_SIZE) {
        if (s_audioConfig.latency.warnOnBuffering) {
            std::wcerr << L"[AudioLatency] WARNING: Single frame buffering violated on capture→encode path! Queue size: "
                      << m_rawFrameQueue.size() << L"/" << MAX_RAW_FRAME_QUEUE_SIZE
                      << L" - Encoder congestion may increase latency beyond target "
                      << s_audioConfig.latency.targetOneWayLatencyMs << L"ms" << std::endl;
        }
        return false; // Signal congestion to capture thread
    }

    // With minimal queue depth (1), we should rarely have queue full situations
    // If queue is full, this indicates encoder thread congestion
    if (m_rawFrameQueue.size() >= MAX_RAW_FRAME_QUEUE_SIZE) {
        if (s_enableBufferMonitoring) {
            std::wcerr << L"[AudioEncoder] Raw frame queue full - encoder thread congested. Size: "
                      << m_rawFrameQueue.size() << L"/" << MAX_RAW_FRAME_QUEUE_SIZE << std::endl;
        }
        return false; // Signal congestion to capture thread
    }

    // Optional buffer monitoring for performance analysis
    if (s_enableBufferMonitoring) {
        static int operationCount = 0;
        if (++operationCount % s_bufferMonitorInterval == 0) {
            std::wcout << L"[AudioEncoder] Raw frame queue monitoring: current="
                      << m_rawFrameQueue.size() << L", max=" << MAX_RAW_FRAME_QUEUE_SIZE << std::endl;
        }
    }

    // Create frame and add to queue
    RawAudioFrame frame;
    frame.samples = std::move(samples); // Move data to avoid copy
    frame.timestampUs = timestampUs;

    m_rawFrameQueue.push(std::move(frame));
    m_rawFrameCondition.notify_one(); // Wake encoder thread

    return true;
}

bool AudioCapturer::QueueRawFrameRef(const std::vector<float>& samples, int64_t timestampUs)
{
    std::lock_guard<std::mutex> lock(m_rawFrameMutex);

    // With minimal queue depth (1), we should rarely have queue full situations
    // If queue is full, this indicates encoder thread congestion
    if (m_rawFrameQueue.size() >= MAX_RAW_FRAME_QUEUE_SIZE) {
        if (s_enableBufferMonitoring) {
            std::wcerr << L"[AudioEncoder] Raw frame queue full - encoder thread congested. Size: "
                      << m_rawFrameQueue.size() << L"/" << MAX_RAW_FRAME_QUEUE_SIZE << std::endl;
        }
        return false; // Signal congestion to capture thread
    }

    // Optional buffer monitoring for performance analysis
    if (s_enableBufferMonitoring) {
        static int operationCount = 0;
        if (++operationCount % s_bufferMonitorInterval == 0) {
            std::wcout << L"[AudioEncoder] Raw frame queue monitoring: current="
                      << m_rawFrameQueue.size() << L", max=" << MAX_RAW_FRAME_QUEUE_SIZE << std::endl;
        }
    }

    // Create frame and copy data (necessary since we have const reference)
    RawAudioFrame frame;
    frame.samples = samples; // Copy data to avoid reference issues
    frame.timestampUs = timestampUs;

    m_rawFrameQueue.push(std::move(frame));
    m_rawFrameCondition.notify_one(); // Wake encoder thread

    return true;
}

// Queue parameter update for encoder thread
void AudioCapturer::QueueParameterUpdate(int bitrate, int expectedLossPerc, int complexity, int fecEnabled) {
    std::lock_guard<std::mutex> lock(m_parameterMutex);

    OpusParameterUpdate update;
    update.bitrate = bitrate;
    update.expectedLossPerc = expectedLossPerc;
    update.complexity = complexity;
    update.fecEnabled = fecEnabled;

    // Replace any existing update (only keep the latest)
    if (!m_parameterUpdateQueue.empty()) {
        m_parameterUpdateQueue.back() = update;
    } else {
        m_parameterUpdateQueue.push(update);
    }
}

// Check for and apply parameter updates (called by encoder thread)
bool AudioCapturer::CheckForParameterUpdates() {
    std::lock_guard<std::mutex> lock(m_parameterMutex);

    if (m_parameterUpdateQueue.empty()) {
        return false;
    }

    OpusParameterUpdate update = m_parameterUpdateQueue.front();
    m_parameterUpdateQueue.pop();

    // Lock will be released when lock_guard goes out of scope

    bool updated = false;

    // Apply bitrate update
    if (update.bitrate > 0 && update.bitrate != s_currentAudioBitrate.load()) {
        // Update the Opus encoder bitrate
        if (m_opusEncoder) {
            // Note: Opus encoder doesn't support runtime bitrate changes in all configurations
            // This would need to be implemented based on specific Opus library capabilities
            // For now, we'll log the intent and update our tracking variable
            s_currentAudioBitrate.store(update.bitrate);
            std::wcout << L"[AudioEncoder] Updated target bitrate to " << update.bitrate << L" bps" << std::endl;
            updated = true;
        }
    }

    // Apply FEC enable/disable update
    if (update.fecEnabled >= 0) {
        if (m_opusEncoder) {
            // Enable/disable Opus in-band FEC
            int fecValue = update.fecEnabled ? 1 : 0;
            int result = opus_encoder_ctl(reinterpret_cast<OpusEncoder*>(m_opusEncoder->GetEncoder()),
                                        OPUS_SET_INBAND_FEC(fecValue));
            if (result == OPUS_OK) {
                std::wcout << L"[AudioEncoder] " << (fecValue ? L"Enabled" : L"Disabled")
                          << L" Opus in-band FEC" << std::endl;
                updated = true;
            } else {
                std::wcerr << L"[AudioEncoder] Failed to update FEC setting, error: " << result << std::endl;
            }
        }
    }

    // Apply expected loss percentage update
    if (update.expectedLossPerc >= 0) {
        if (m_opusEncoder) {
            // Update expected packet loss percentage for FEC tuning
            int clampedLoss = (std::min)(100, (std::max)(0, update.expectedLossPerc));
            int result = opus_encoder_ctl(reinterpret_cast<OpusEncoder*>(m_opusEncoder->GetEncoder()),
                                        OPUS_SET_PACKET_LOSS_PERC(clampedLoss));
            if (result == OPUS_OK) {
                std::wcout << L"[AudioEncoder] Updated expected loss to " << clampedLoss << L"%" << std::endl;
                updated = true;
            } else {
                std::wcerr << L"[AudioEncoder] Failed to update expected loss, error: " << result << std::endl;
            }
        } else {
            std::wcout << L"[AudioEncoder] Updated expected loss to " << update.expectedLossPerc << L"% (encoder not ready)" << std::endl;
        }
    }

    // Apply complexity update
    if (update.complexity >= 0 && update.complexity <= 10) {
        if (m_opusEncoder) {
            // Update Opus encoder complexity
            int result = opus_encoder_ctl(reinterpret_cast<OpusEncoder*>(m_opusEncoder->GetEncoder()),
                                        OPUS_SET_COMPLEXITY(update.complexity));
            if (result == OPUS_OK) {
                std::wcout << L"[AudioEncoder] Updated complexity to " << update.complexity << std::endl;
                updated = true;
            } else {
                std::wcerr << L"[AudioEncoder] Failed to update complexity, error: " << result << std::endl;
            }
        } else {
            std::wcout << L"[AudioEncoder] Updated complexity to " << update.complexity << L" (encoder not ready)" << std::endl;
        }
    }

    if (updated) {
        std::wcout << L"[AudioEncoder] Applied parameter updates successfully" << std::endl;
    }

    return updated;
}

bool AudioCapturer::QueueAudioPacket(std::vector<uint8_t>& data, int64_t timestampUs, uint32_t rtpTimestamp)
{
    std::lock_guard<std::mutex> lock(m_queueMutex);

    // Check for buffering violations in strict latency mode
    if (s_audioConfig.latency.enforceSingleFrameBuffering && m_audioQueue.size() >= MAX_QUEUE_SIZE) {
        if (s_audioConfig.latency.warnOnBuffering) {
            std::wcerr << L"[AudioLatency] WARNING: Single frame buffering violated! Queue size: "
                      << m_audioQueue.size() << L"/" << MAX_QUEUE_SIZE
                      << L" - WebRTC congestion may increase latency beyond target "
                      << s_audioConfig.latency.targetOneWayLatencyMs << L"ms" << std::endl;
        }
        return false; // Signal congestion to encoder thread
    }

    // With minimal queue size (1), we should rarely have queue full situations
    // If queue is full, this indicates WebRTC congestion - let WebRTC handle it
    if (m_audioQueue.size() >= MAX_QUEUE_SIZE) {
        // Instead of dropping, signal congestion to encoder thread
        // This allows WebRTC's congestion control to work optimally
        if (s_enableBufferMonitoring) {
            std::wcerr << L"[AudioQueue] Queue full - WebRTC congestion detected. Size: "
                      << m_audioQueue.size() << L"/" << MAX_QUEUE_SIZE << std::endl;
        }
        return false; // Signal congestion to encoder thread
    }

    // Optional buffer monitoring for performance analysis
    if (s_enableBufferMonitoring) {
        static int operationCount = 0;
        if (++operationCount % s_bufferMonitorInterval == 0) {
            std::wcout << L"[AudioQueue] Queue depth monitoring: current="
                      << m_audioQueue.size() << L", max=" << MAX_QUEUE_SIZE << std::endl;
        }
    }

    // Create packet and add to queue
    AudioPacket packet;
    packet.data = std::move(data); // Move data to avoid copy
    packet.timestampUs = timestampUs;
    packet.rtpTimestamp = rtpTimestamp;

    m_audioQueue.push(std::move(packet));
    m_queueCondition.notify_one(); // Wake queue processor

    return true;
}

// Overloaded method for zero-copy buffer data (avoid intermediate vector allocation)
bool AudioCapturer::QueueAudioPacket(const uint8_t* buffer, size_t size, int64_t timestampUs, uint32_t rtpTimestamp)
{
    std::lock_guard<std::mutex> lock(m_queueMutex);

    // With minimal queue size (1), we should rarely have queue full situations
    // If queue is full, this indicates WebRTC congestion - let WebRTC handle it
    if (m_audioQueue.size() >= MAX_QUEUE_SIZE) {
        // Instead of dropping, signal congestion to encoder thread
        // This allows WebRTC's congestion control to work optimally
        if (s_enableBufferMonitoring) {
            std::wcerr << L"[AudioQueue] Queue full - WebRTC congestion detected. Size: "
                      << m_audioQueue.size() << L"/" << MAX_QUEUE_SIZE << std::endl;
        }
        return false; // Signal congestion to encoder thread
    }

    // Optional buffer monitoring for performance analysis
    if (s_enableBufferMonitoring) {
        static int operationCount = 0;
        if (++operationCount % s_bufferMonitorInterval == 0) {
            std::wcout << L"[AudioQueue] Queue depth monitoring: current="
                      << m_audioQueue.size() << L", max=" << MAX_QUEUE_SIZE
                      << L", buffer_size=" << size << L" bytes" << std::endl;
        }
    }

    // Create packet with copied buffer data (minimal allocation)
    AudioPacket packet;
    packet.data.assign(buffer, buffer + size); // Copy data from fixed buffer
    packet.timestampUs = timestampUs;
    packet.rtpTimestamp = rtpTimestamp;

    m_audioQueue.push(std::move(packet));
    m_queueCondition.notify_one(); // Wake queue processor

    return true;
}

void AudioCapturer::CaptureThread(DWORD targetProcessId)
{
    std::wcout << L"[AudioCapturer] DEBUG: CaptureThread started for PID=" << targetProcessId << std::endl;
    HRESULT hr;
    // Calculate requested duration based on WASAPI configuration
    REFERENCE_TIME hnsRequestedDuration = static_cast<REFERENCE_TIME>(s_audioConfig.wasapi.devicePeriodMs * 10000.0); // Convert ms to 100ns units
    UINT32 bufferFrameCount;
    UINT32 numFramesAvailable;
    BYTE* pData;
    DWORD flags;
    WAVEFORMATEX* pwfx = NULL;
    DWORD sleepMs = 10; // polling interval (fallback when event mode is unavailable)
    bool eventMode = false; // declared early to avoid goto skipping initialization

    // Declare variables that might be skipped by goto statements
    DWORD streamFlags = 0;
    AUDCLNT_SHAREMODE shareMode = AUDCLNT_SHAREMODE_SHARED;
    WAVEFORMATEX* targetFormat = NULL;
    size_t requiredSamplesDefault = 0;

    // Debug state for process loopback attempt/fallback
    bool processLoopbackAttempted = false;
    bool processLoopbackSucceeded = false;
    std::wstring processLoopbackFallbackReason;

    // Opus frame timing constant (declared early to avoid goto skip issues)
    const DWORD OPUS_FRAME_MS = 10; // 10ms Opus frame duration

    // Performance monitoring counters
    uint64_t captureCycles = 0;
    uint64_t dataPacketsProcessed = 0;
    uint64_t timeoutCount = 0;
    uint64_t lastLogTime = GetTickCount64();

    hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    if (FAILED(hr))
    {
        std::wcerr << L"Unable to initialize COM in render thread: " << _com_error(hr).ErrorMessage() << std::endl;
        return;
    }

    // Register this thread with MMCSS for proper scheduling priority
    // This helps prevent audio glitching under system load
    m_hMmcssTask = AvSetMmThreadCharacteristicsW(L"Pro Audio", &m_mmcssTaskIndex);

    // Add delay for gaming applications to initialize audio sessions
    // Games like CS2.exe may not have started audio sessions immediately
    std::wcout << L"[AudioCapturer] Waiting 3 seconds for target process audio session initialization..." << std::endl;
    Sleep(3000); // 3 second delay to allow games to start audio
    std::wcout << L"[AudioCapturer] 3-second delay completed, proceeding with device scan..." << std::endl;
    if (m_hMmcssTask == nullptr) {
        // Fallback to "Audio" if "Pro Audio" is not available
        m_hMmcssTask = AvSetMmThreadCharacteristicsW(L"Audio", &m_mmcssTaskIndex);
    }

    if (m_hMmcssTask != nullptr) {
        // Set thread priority for low-latency audio processing
        BOOL prioritySet = AvSetMmThreadPriority(m_hMmcssTask, AVRT_PRIORITY_HIGH);
        if (prioritySet) {
            std::wcout << L"[AudioCapturer] MMCSS registered successfully (task index: " << m_mmcssTaskIndex
                       << L", priority: HIGH)" << std::endl;
        } else {
            std::wcerr << L"[AudioCapturer] Failed to set MMCSS thread priority: " << GetLastError() << std::endl;
        }
    } else {
        std::wcerr << L"[AudioCapturer] Failed to register thread with MMCSS: " << GetLastError()
                   << L" (audio may glitch under system load)" << std::endl;
    }

    IMMDeviceCollection* pCollection = NULL;
    IAudioSessionManager2* pSessionManager = NULL;
    IAudioSessionEnumerator* pSessionEnumerator = NULL;
    IAudioSessionControl* pSessionControl = NULL;
    IAudioSessionControl2* pSessionControl2 = NULL;

    // ============================================================================
    // Application-specific audio capture via ActivateAudioInterfaceAsync
    // ============================================================================

    // For gaming applications, log helpful information about audio capture methods
    std::wcout << L"[AudioCapturer] Starting audio capture for PID=" << targetProcessId << std::endl;
    std::wcout << L"[AudioCapturer] Process loopback enabled: " << (s_audioConfig.processLoopback.enabled ? L"YES" : L"NO") << std::endl;

    // Store target process name for session matching (works with any process name)
    std::string targetProcessNameLower = m_targetProcessName;
    if (!targetProcessNameLower.empty()) {
        std::transform(targetProcessNameLower.begin(), targetProcessNameLower.end(), targetProcessNameLower.begin(), ::tolower);
    }

    // Provide guidance based on process loopback configuration
    if (s_audioConfig.processLoopback.enabled) {
        std::wcout << L"[AudioCapturer] Process loopback is enabled - attempting direct process audio capture" << std::endl;
        std::wcout << L"[AudioCapturer] This works best for applications that don't have special audio routing" << std::endl;
    } else {
        std::wcout << L"[AudioCapturer] Process loopback disabled - will use device loopback instead" << std::endl;
    }

    if (s_audioConfig.processLoopback.enabled && targetProcessId != 0)
    {
        std::wcout << L"[AudioCapturer] Attempting process loopback capture via ActivateAudioInterfaceAsync for PID="
                   << targetProcessId << std::endl;

        // Initialize fallback reason
        processLoopbackFallbackReason = L"Process loopback initialization started";
        processLoopbackAttempted = true;

        // ============================================================================
        // Enhanced Process Loopback Implementation with Diagnostics
        // ============================================================================

        // Step 1: Validate target process exists and is accessible
        std::wcout << L"[AudioCapturer] Step 1: Checking process access for PID=" << targetProcessId << std::endl;
        HANDLE hProcess = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, targetProcessId);
        if (!hProcess)
        {
            DWORD lastError = GetLastError();
            std::wcerr << L"[AudioCapturer] Cannot access target process " << targetProcessId
                       << L": " << _com_error(HRESULT_FROM_WIN32(lastError)).ErrorMessage()
                       << L" (Error code: " << lastError << L")" << std::endl;

            // Provide specific guidance for gaming applications
            if (lastError == ERROR_ACCESS_DENIED)
            {
                std::wcerr << L"[AudioCapturer] ACCESS DENIED - This is common with:" << std::endl;
                std::wcerr << L"[AudioCapturer]   • Protected processes (games like CS2.exe)" << std::endl;
                std::wcerr << L"[AudioCapturer]   • Elevated/UAC processes" << std::endl;
                std::wcerr << L"[AudioCapturer]   • Anti-cheat protected games" << std::endl;
                std::wcerr << L"[AudioCapturer]   → Will fallback to device loopback" << std::endl;
            }
            else if (lastError == ERROR_INVALID_PARAMETER)
            {
                std::wcerr << L"[AudioCapturer] Process does not exist or has terminated" << std::endl;
            }
            processLoopbackFallbackReason = L"Cannot access target process";
        }
        else
        {
            std::wcout << L"[AudioCapturer] Step 1: Process access successful" << std::endl;
            CloseHandle(hProcess);

            // Step 2: Check Windows version compatibility (requires Windows 10 1903+)
            // Use a simpler approach - try to load the required function
            std::wcout << L"[AudioCapturer] Step 2: Checking Windows version compatibility" << std::endl;
            HMODULE hModule = LoadLibraryW(L"mmdevapi.dll");
            if (!hModule)
            {
                std::wcerr << L"[AudioCapturer] Failed to load mmdevapi.dll - Windows version may be too old." << std::endl;
                processLoopbackFallbackReason = L"mmdevapi.dll not available";
            }
            else
            {
                std::wcout << L"[AudioCapturer] Step 2: mmdevapi.dll loaded successfully" << std::endl;

                // Try to get the function pointer for ActivateAudioInterfaceAsync
                typedef HRESULT(WINAPI* ActivateAudioInterfaceAsyncFn)(
                    LPCWSTR deviceInterface,
                    REFIID iid,
                    PROPVARIANT* activationParams,
                    IActivateAudioInterfaceCompletionHandler* completionHandler,
                    IActivateAudioInterfaceAsyncOperation** asyncOp
                    );

                std::wcout << L"[AudioCapturer] Step 3: Checking for ActivateAudioInterfaceAsync function" << std::endl;
                ActivateAudioInterfaceAsyncFn pActivateAudioInterfaceAsync =
                    (ActivateAudioInterfaceAsyncFn)GetProcAddress(hModule, "ActivateAudioInterfaceAsync");

                if (!pActivateAudioInterfaceAsync)
                {
                    std::wcerr << L"[AudioCapturer] ActivateAudioInterfaceAsync function not available - Windows version too old for process loopback." << std::endl;
                    processLoopbackFallbackReason = L"ActivateAudioInterfaceAsync not available";
                    FreeLibrary(hModule);
                }
                else
                {
                    std::wcout << L"[AudioCapturer] Step 3: ActivateAudioInterfaceAsync function found" << std::endl;
                    FreeLibrary(hModule);

                    // Windows version check passed - proceed with ActivateAudioInterfaceAsync
                    AUDIOCLIENT_ACTIVATION_PARAMS activationParams = {};
                    activationParams.ActivationType = AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK;
                    activationParams.ProcessLoopbackParams.TargetProcessId = targetProcessId;
                    activationParams.ProcessLoopbackParams.ProcessLoopbackMode =
                        (s_audioConfig.processLoopback.includeProcessTree
                            ? PROCESS_LOOPBACK_MODE_INCLUDE_TARGET_PROCESS_TREE
                            : PROCESS_LOOPBACK_MODE_EXCLUDE_TARGET_PROCESS_TREE);

                    PROPVARIANT pv; PropVariantInit(&pv);
                    hr = InitPropVariantFromBuffer(&activationParams, sizeof(activationParams), &pv);
                    if (SUCCEEDED(hr))
                    {
                        processLoopbackAttempted = true;
                        HANDLE hActivate = CreateEvent(nullptr, FALSE, FALSE, nullptr);
                        if (!hActivate)
                        {
                            std::wcerr << L"[AudioCapturer] Failed to create activation event: " << GetLastError() << std::endl;
                            processLoopbackFallbackReason = L"CreateEvent failed for activation";
                        }
                        else
                        {
                            IActivateAudioInterfaceCompletionHandler* handler = new (std::nothrow) ActivateAudioCompletionHandler(hActivate);
                            if (!handler)
                            {
                                std::wcerr << L"[AudioCapturer] Failed to allocate activation handler" << std::endl;
                                processLoopbackFallbackReason = L"Allocation failure for completion handler";
                            }
                            else
                            {
                                // Step 4: Try ActivateAudioInterfaceAsync with enhanced error diagnostics
                                Microsoft::WRL::ComPtr<IActivateAudioInterfaceAsyncOperation> op;
                                std::wcout << L"[AudioCapturer] Step 4: Attempting ActivateAudioInterfaceAsync call..." << std::endl;

                                std::wcout << L"[AudioCapturer] About to call ActivateAudioInterfaceAsync..." << std::endl;

                                hr = ActivateAudioInterfaceAsync(
                                    L"VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK",
                                    __uuidof(IAudioClient),
                                    &pv,
                                    handler,
                                    op.ReleaseAndGetAddressOf());

                                std::wcout << L"[AudioCapturer] ActivateAudioInterfaceAsync returned: 0x" << std::hex << hr << std::dec << std::endl;

                                if (SUCCEEDED(hr))
                                {
                                    std::wcout << L"[AudioCapturer] ActivateAudioInterfaceAsync call succeeded, waiting for completion..." << std::endl;
                                    DWORD waitResult = WaitForSingleObject(hActivate, 5000); // 5 second timeout

                                    std::wcout << L"[AudioCapturer] WaitForSingleObject returned: " << waitResult << std::endl;

                                    if (waitResult == WAIT_TIMEOUT)
                                    {
                                        std::wcerr << L"[AudioCapturer] ActivateAudioInterfaceAsync timed out after 5 seconds" << std::endl;
                                        processLoopbackFallbackReason = L"ActivateAudioInterfaceAsync timeout";
                                    }
                                    else if (waitResult == WAIT_OBJECT_0)
                                    {
                                        // Retrieve result and IAudioClient from handler
                                        HRESULT hrActivate = static_cast<ActivateAudioCompletionHandler*>(handler)->Result();
                                        auto client = static_cast<ActivateAudioCompletionHandler*>(handler)->GetClient();

                                        std::wcout << L"[AudioCapturer] Async operation completed. Result: " << std::hex << hrActivate << std::dec
                                                   << L", Client: " << (client ? L"Valid" : L"NULL") << std::endl;

                                        if (SUCCEEDED(hrActivate) && client)
                                        {
                                            m_pAudioClient = client; // Use process-specific IAudioClient
                                            std::wcout << L"[AudioCapturer] Process loopback activation succeeded for PID=" << targetProcessId << std::endl;
                                            processLoopbackSucceeded = true;
                                            processLoopbackFallbackReason = L"Success - Process loopback activated";

                                            // Initialize client for capture
                                            hr = m_pAudioClient->GetMixFormat(&pwfx);
                                            if (FAILED(hr))
                                            {
                                                std::wcerr << L"[AudioCapturer] Unable to get mix format for process loopback: "
                                                    << _com_error(hr).ErrorMessage() << std::endl;
                                                processLoopbackFallbackReason = L"GetMixFormat failed (process loopback)";
                                            }
                                            else
                                            {
                                                std::wcout << L"[AudioCapturer] Process mix format retrieved successfully" << std::endl;

                                                // Track active format for WAV header
                                                m_activeChannels = static_cast<uint16_t>(pwfx->nChannels);
                                                m_activeSampleRate = static_cast<uint32_t>(pwfx->nSamplesPerSec);
                                                m_activeBitsPerSample = pwfx->wBitsPerSample;
                                                if (pwfx->wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
                                                    WAVEFORMATEXTENSIBLE* pEx = reinterpret_cast<WAVEFORMATEXTENSIBLE*>(pwfx);
                                                    m_activeWavAudioFormat = (pEx->SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT) ? 3 : 1;
                                                }
                                                else if (pwfx->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) {
                                                    m_activeWavAudioFormat = 3;
                                                }
                                                else {
                                                    m_activeWavAudioFormat = 1;
                                                }

                                                // For process loopback, use shared mode for broad compatibility
                                                streamFlags = AUDCLNT_STREAMFLAGS_LOOPBACK | AUDCLNT_STREAMFLAGS_EVENTCALLBACK;
                                                targetFormat = const_cast<WAVEFORMATEX*>(pwfx);

                                                if (s_audioConfig.wasapi.force48kHzStereo) {
                                                    bool isAlreadyOptimal = (pwfx->nSamplesPerSec == 48000 &&
                                                        pwfx->nChannels == 2 &&
                                                        pwfx->wBitsPerSample == 16 &&
                                                        pwfx->wFormatTag == WAVE_FORMAT_PCM);
                                                    if (!isAlreadyOptimal) {
                                                        static WAVEFORMATEX forcedFormat = {};
                                                        forcedFormat.wFormatTag = WAVE_FORMAT_PCM;
                                                        forcedFormat.nChannels = 2;
                                                        forcedFormat.nSamplesPerSec = 48000;
                                                        forcedFormat.nAvgBytesPerSec = 48000 * 2 * 2;
                                                        forcedFormat.nBlockAlign = 2 * 2;
                                                        forcedFormat.wBitsPerSample = 16;
                                                        forcedFormat.cbSize = 0;
                                                        targetFormat = &forcedFormat;
                                                        std::wcout << L"[AudioCapturer] Forcing 48kHz stereo PCM format for process loopback" << std::endl;
                                                    }
                                                }

                                                hr = m_pAudioClient->Initialize(
                                                    AUDCLNT_SHAREMODE_SHARED,
                                                    streamFlags,
                                                    hnsRequestedDuration,
                                                    0,
                                                    targetFormat,
                                                    NULL);

                                                if (FAILED(hr))
                                                {
                                                    if (s_audioConfig.wasapi.enforceEventDriven) {
                                                        std::wcerr << L"[AudioCapturer] Process loopback event-driven init failed and enforcement is enabled. Error: "
                                                            << _com_error(hr).ErrorMessage() << std::endl;
                                                        processLoopbackFallbackReason = L"IAudioClient Initialize failed (event-driven, enforced)";
                                                    }
                                                    else {
                                                        std::wcerr << L"[AudioCapturer] Process loopback event-driven init failed; retrying in polling mode. Error: "
                                                            << _com_error(hr).ErrorMessage() << std::endl;
                                                        hr = m_pAudioClient->Initialize(
                                                            AUDCLNT_SHAREMODE_SHARED,
                                                            AUDCLNT_STREAMFLAGS_LOOPBACK,
                                                            hnsRequestedDuration,
                                                            0,
                                                            const_cast<const WAVEFORMATEX*>(pwfx),
                                                            NULL);
                                                        if (FAILED(hr)) {
                                                            processLoopbackFallbackReason = L"IAudioClient Initialize failed (polling fallback)";
                                                        }
                                                    }
                                                }

                                                if (SUCCEEDED(hr))
                                                {
                                                    hr = m_pAudioClient->GetBufferSize(&bufferFrameCount);
                                                    if (FAILED(hr)) {
                                                        std::wcerr << L"[AudioCapturer] Unable to get buffer size for process loopback: "
                                                            << _com_error(hr).ErrorMessage() << std::endl;
                                                        processLoopbackFallbackReason = L"GetBufferSize failed (process loopback)";
                                                    }
                                                    else {
                                                        size_t requiredSamples = static_cast<size_t>(bufferFrameCount) * static_cast<size_t>(pwfx->nChannels);
                                                        if (m_floatBuffer.size() < requiredSamples) {
                                                            m_floatBuffer.resize(requiredSamples);
                                                        }

                                                        hr = m_pAudioClient->GetService(__uuidof(IAudioCaptureClient), reinterpret_cast<void**>(m_pCaptureClient.ReleaseAndGetAddressOf()));
                                                        if (FAILED(hr)) {
                                                            std::wcerr << L"[AudioCapturer] Unable to get capture client for process loopback: "
                                                                << _com_error(hr).ErrorMessage() << std::endl;
                                                            processLoopbackFallbackReason = L"GetService(IAudioCaptureClient) failed (process loopback)";
                                                        }
                                                        else {
                                                            // Query IAudioClock for precise timestamps
                                                            hr = m_pAudioClient->GetService(__uuidof(IAudioClock), reinterpret_cast<void**>(m_pAudioClock.ReleaseAndGetAddressOf()));
                                                            if (SUCCEEDED(hr) && m_pAudioClock) {
                                                                UINT64 freq = 0;
                                                                if (SUCCEEDED(m_pAudioClock->GetFrequency(&freq))) {
                                                                    m_audioClockFreq = freq;
                                                                    std::wcout << L"[AudioCapturer] Process loopback AudioClock frequency: " << freq << std::endl;
                                                                }
                                                            }

                                                            std::wcout << L"[AudioCapturer] Successfully initialized process loopback capture." << std::endl;
                                                            std::wcout << L"[AudioCapturer] Capture path selected: PROCESS_LOOPBACK (PID="
                                                                << targetProcessId << L", includeTree="
                                                                << (s_audioConfig.processLoopback.includeProcessTree ? L"true" : L"false")
                                                                << L")" << std::endl;
                                                            // Skip endpoint enumeration and default fallback
                                                            goto AfterDeviceSelection;
                                                        }
                                                    }
                                                }
                                            }
                                        }
                                        else
                                        {
                                            std::wcerr << L"[AudioCapturer] Process loopback activation failed: "
                                                       << _com_error(SUCCEEDED(hr) ? hrActivate : hr).ErrorMessage()
                                                       << std::endl;
                                            if (SUCCEEDED(hr)) {
                                                std::wcerr << L"[AudioCapturer] Activation completed but returned failure code" << std::endl;
                                            }
                                            processLoopbackFallbackReason = L"ActivateAudioInterfaceAsync completion failed";
                                        }
                                    }
                                    else
                                    {
                                        std::wcerr << L"[AudioCapturer] WaitForSingleObject failed: " << GetLastError() << std::endl;
                                        processLoopbackFallbackReason = L"WaitForSingleObject failed";
                                    }
                                }
                                else
                                {
                                    std::wcerr << L"[AudioCapturer] ActivateAudioInterfaceAsync call failed: " << _com_error(hr).ErrorMessage()
                                               << L" (HRESULT: 0x" << std::hex << hr << std::dec << L")" << std::endl;

                                    // Provide specific guidance for common errors
                                    if (hr == 0x80000010) // RPC_E_WRONG_THREAD
                                    {
                                        std::wcerr << L"[AudioCapturer] RPC_E_WRONG_THREAD error - this may be due to:" << std::endl;
                                        std::wcerr << L"[AudioCapturer]   1. COM apartment threading issue" << std::endl;
                                        std::wcerr << L"[AudioCapturer]   2. Process loopback not supported on this system" << std::endl;
                                        std::wcerr << L"[AudioCapturer]   3. Target process permissions or state issue" << std::endl;
                                        processLoopbackFallbackReason = L"RPC_E_WRONG_THREAD - Process loopback not supported";
                                    }
                                    else if (hr == E_ACCESSDENIED)
                                    {
                                        std::wcerr << L"[AudioCapturer] Access denied - check process permissions" << std::endl;
                                        processLoopbackFallbackReason = L"Access denied to target process";
                                    }
                                    else if (hr == E_INVALIDARG)
                                    {
                                        std::wcerr << L"[AudioCapturer] Invalid arguments - process may not exist or be accessible" << std::endl;
                                        processLoopbackFallbackReason = L"Invalid process arguments";
                                    }
                                    else
                                    {
                                        processLoopbackFallbackReason = L"ActivateAudioInterfaceAsync API call failed";
                                    }
                                }

                                handler->Release();
                            }

                            CloseHandle(hActivate);
                        }

                        PropVariantClear(&pv);
                    }
                    else
                    {
                        std::wcerr << L"[AudioCapturer] InitPropVariantFromBuffer failed: " << _com_error(hr).ErrorMessage() << std::endl;
                        processLoopbackAttempted = true;
                        processLoopbackFallbackReason = L"InitPropVariantFromBuffer failed";
                    }
                }
            }
        }

        if (processLoopbackFallbackReason == L"Process loopback initialization started")
        {
            processLoopbackFallbackReason = L"Process loopback logic completed without setting success flag";
        }
    }

    // Log final diagnostic summary
    if (processLoopbackAttempted && !processLoopbackSucceeded)
    {
        std::wcout << L"[AudioCapturer] Process loopback diagnostic summary:" << std::endl;
        std::wcout << L"[AudioCapturer]   - Process access: SUCCESS" << std::endl;
        std::wcout << L"[AudioCapturer]   - Windows version: SUCCESS" << std::endl;
        std::wcout << L"[AudioCapturer]   - API availability: SUCCESS" << std::endl;
        std::wcout << L"[AudioCapturer]   - API call result: FAILED (" << processLoopbackFallbackReason << L")" << std::endl;
        std::wcout << L"[AudioCapturer]   - System may not support process loopback for this configuration" << std::endl;
    }
    else
    {
        if (!s_audioConfig.processLoopback.enabled) {
            std::wcout << L"[AudioCapturer] Process loopback disabled by config; using device loopback fallback." << std::endl;
        } else if (targetProcessId == 0) {
            std::wcout << L"[AudioCapturer] No target PID provided; using device loopback fallback." << std::endl;
        }
    }

    AUDIO_LOG_INFO(L"[AudioCapturer] Target Process ID: " << targetProcessId);

    const CLSID CLSID_MMDeviceEnumerator = __uuidof(MMDeviceEnumerator);
    const IID IID_IMMDeviceEnumerator = __uuidof(IMMDeviceEnumerator);
    hr = CoCreateInstance(
        CLSID_MMDeviceEnumerator, NULL,
        CLSCTX_ALL, IID_IMMDeviceEnumerator,
        reinterpret_cast<void**>(m_pEnumerator.ReleaseAndGetAddressOf()));

    if (FAILED(hr))
    {
        std::wcerr << L"[AudioCapturer] Unable to instantiate device enumerator: " << _com_error(hr).ErrorMessage() << std::endl;
        goto Exit;
    }

    hr = m_pEnumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &pCollection);
    if (FAILED(hr))
    {
        std::wcerr << L"[AudioCapturer] Unable to enumerate audio endpoints: " << _com_error(hr).ErrorMessage() << std::endl;
        goto Exit;
    }

    UINT deviceCount;
    hr = pCollection->GetCount(&deviceCount);
    if (FAILED(hr))
    {
        std::wcerr << L"[AudioCapturer] Unable to get device count: " << _com_error(hr).ErrorMessage() << std::endl;
        goto Exit;
    }

    AUDIO_LOG_INFO(L"[AudioCapturer] Found " << deviceCount << L" audio devices");

    for (UINT i = 0; i < deviceCount; ++i)
    {
        IMMDevice* pDevice = NULL;
        hr = pCollection->Item(i, &pDevice);
        if (FAILED(hr))
        {
            std::wcerr << L"[AudioCapturer] Failed to get device item " << i << L": " << _com_error(hr).ErrorMessage() << std::endl;
            continue;
        }

        LPWSTR deviceId = NULL;
        hr = pDevice->GetId(&deviceId);
        if (SUCCEEDED(hr))
        {
            std::wcout << L"[AudioCapturer] Device " << i << L" ID: " << deviceId << std::endl;
            CoTaskMemFree(deviceId);
        }

        hr = pDevice->Activate(__uuidof(IAudioSessionManager2), CLSCTX_ALL, NULL, (void**)&pSessionManager);
        if (FAILED(hr))
        {
            std::wcerr << L"[AudioCapturer] Failed to activate IAudioSessionManager2 for device " << i << L": " << _com_error(hr).ErrorMessage() << std::endl;
            pDevice->Release();
            continue;
        }

        hr = pSessionManager->GetSessionEnumerator(&pSessionEnumerator);
        if (FAILED(hr))
        {
            std::wcerr << L"[AudioCapturer] Failed to get session enumerator for device " << i << L": " << _com_error(hr).ErrorMessage() << std::endl;
            pSessionManager->Release();
            pDevice->Release();
            continue;
        }

        int sessionCount;
        hr = pSessionEnumerator->GetCount(&sessionCount);
        if (FAILED(hr))
        {
            std::wcerr << L"[AudioCapturer] Failed to get session count for device " << i << L": " << _com_error(hr).ErrorMessage() << std::endl;
            pSessionEnumerator->Release();
            pSessionManager->Release();
            pDevice->Release();
            continue;
        }

        std::wcout << L"[AudioCapturer] Device " << i << L" has " << sessionCount << L" sessions." << std::endl;

        for (int j = 0; j < sessionCount; ++j)
        {
            hr = pSessionEnumerator->GetSession(j, &pSessionControl);
            if (FAILED(hr))
            {
                std::wcerr << L"[AudioCapturer] Failed to get session " << j << L" for device " << i << L": " << _com_error(hr).ErrorMessage() << std::endl;
                continue;
            }

            hr = pSessionControl->QueryInterface(__uuidof(IAudioSessionControl2), (void**)&pSessionControl2);
            if (FAILED(hr))
            {
                std::wcerr << L"[AudioCapturer] Failed to query IAudioSessionControl2 for session " << j << L": " << _com_error(hr).ErrorMessage() << std::endl;
                pSessionControl->Release();
                continue;
            }

            DWORD currentProcessId = 0;
            hr = pSessionControl2->GetProcessId(&currentProcessId);
            if (FAILED(hr))
            {
                std::wcerr << L"[AudioCapturer] Failed to get process ID for session " << j << L": " << _com_error(hr).ErrorMessage() << std::endl;
                pSessionControl2->Release();
                pSessionControl->Release();
                continue;
            }

            std::wcout << L"[AudioCapturer] Session " << j << L" Process ID: " << currentProcessId << std::endl;

            if (currentProcessId == targetProcessId)
            {
                std::wcout << L"[AudioCapturer] Found matching session for target process ID: " << targetProcessId << std::endl;
                // Found the session for the target process
                m_pDevice = pDevice; // ComPtr takes ownership (AddRef)
                if (pDevice) { pDevice->Release(); pDevice = NULL; }
                m_pSessionControl2 = pSessionControl2; // ComPtr takes ownership (AddRef)
                if (pSessionControl2) { pSessionControl2->Release(); pSessionControl2 = NULL; }
                
                // Fallback to device loopback
                std::wstring capturePath = L"FALLBACK_DEVICE_LOOPBACK";
                bool loopbackActivated = false;

                // Always activate device loopback here
                hr = m_pDevice->Activate(__uuidof(IAudioClient), CLSCTX_ALL, NULL, reinterpret_cast<void**>(m_pAudioClient.ReleaseAndGetAddressOf()));
                if (FAILED(hr))
                {
                    std::wcerr << L"[AudioCapturer] Unable to activate audio client for target process device: " << _com_error(hr).ErrorMessage() << std::endl;
                    goto Exit;
                }

                if (processLoopbackAttempted && !processLoopbackSucceeded) {
                    std::wcout << L"[AudioCapturer] Fallback to device loopback. Reason: "
                               << (processLoopbackFallbackReason.empty() ? L"Unknown" : processLoopbackFallbackReason) << std::endl;
                }
                std::wcout << L"[AudioCapturer] Capture path selected: " << capturePath << std::endl;

                hr = m_pAudioClient->GetMixFormat(&pwfx);
                if (FAILED(hr))
                {
                    std::wcerr << L"[AudioCapturer] Unable to get mix format for target process: " << _com_error(hr).ErrorMessage() << std::endl;
                    goto Exit;
                }

                std::wcout << L"[AudioCapturer] Mix Format: wFormatTag=" << pwfx->wFormatTag
                    << L", nChannels=" << pwfx->nChannels
                    << L", nSamplesPerSec=" << pwfx->nSamplesPerSec
                    << L", nAvgBytesPerSec=" << pwfx->nAvgBytesPerSec
                    << L", nBlockAlign=" << pwfx->nBlockAlign
                    << L", wBitsPerSample=" << pwfx->wBitsPerSample
                    << L", cbSize=" << pwfx->cbSize << std::endl;
                
                // Track active format for WAV header
                m_activeChannels = static_cast<uint16_t>(pwfx->nChannels);
                m_activeSampleRate = static_cast<uint32_t>(pwfx->nSamplesPerSec);
                m_activeBitsPerSample = pwfx->wBitsPerSample;
                // Determine WAV audio format tag: 1 = PCM, 3 = IEEE float
                if (pwfx->wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
                    WAVEFORMATEXTENSIBLE* pEx = reinterpret_cast<WAVEFORMATEXTENSIBLE*>(pwfx);
                    if (pEx->SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT) {
                        m_activeWavAudioFormat = 3;
                    } else {
                        m_activeWavAudioFormat = 1;
                    }
                } else if (pwfx->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) {
                    m_activeWavAudioFormat = 3;
                } else {
                    m_activeWavAudioFormat = 1;
                }

                // Note: We'll resample to 48kHz if needed

                // No per-process session GUID when using pure device loopback
                GUID targetSessionGuid = GUID_NULL;

                // Try exclusive mode first if preferred, then shared mode with event-driven capture
                streamFlags = AUDCLNT_STREAMFLAGS_LOOPBACK | AUDCLNT_STREAMFLAGS_EVENTCALLBACK;
                shareMode = s_audioConfig.wasapi.preferExclusiveMode ?
                    AUDCLNT_SHAREMODE_EXCLUSIVE : AUDCLNT_SHAREMODE_SHARED;

                // If forcing 48kHz stereo, modify the format before initialization
                targetFormat = const_cast<WAVEFORMATEX*>(pwfx);
                if (s_audioConfig.wasapi.force48kHzStereo) {
                    // Check if device already provides optimal format to avoid unnecessary conversions
                    bool isAlreadyOptimal = (pwfx->nSamplesPerSec == 48000 &&
                                           pwfx->nChannels == 2 &&
                                           ((pwfx->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) ||
                                            (pwfx->wFormatTag == WAVE_FORMAT_EXTENSIBLE)));

                    if (isAlreadyOptimal) {
                        std::wcout << L"[AudioCapturer] Target process device already provides optimal 48kHz stereo PCM format - no conversion needed" << std::endl;
                        targetFormat = const_cast<WAVEFORMATEX*>(pwfx);
                    } else {
                        // Create a new format structure with 48kHz stereo float if preferred
                        static WAVEFORMATEXTENSIBLE forcedFloat = {};
                        if (s_audioConfig.wasapi.preferFloat) {
                            forcedFloat.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
                            forcedFloat.Format.nChannels = 2;
                            forcedFloat.Format.nSamplesPerSec = 48000;
                            forcedFloat.Format.wBitsPerSample = 32;
                            forcedFloat.Format.nBlockAlign = (forcedFloat.Format.nChannels * forcedFloat.Format.wBitsPerSample) / 8;
                            forcedFloat.Format.nAvgBytesPerSec = forcedFloat.Format.nSamplesPerSec * forcedFloat.Format.nBlockAlign;
                            forcedFloat.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
                            forcedFloat.Samples.wValidBitsPerSample = 32;
                            forcedFloat.dwChannelMask = SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT;
                            forcedFloat.SubFormat = KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;
                            targetFormat = reinterpret_cast<WAVEFORMATEX*>(&forcedFloat);
                        } else {
                            static WAVEFORMATEX forcedPcm = {};
                            forcedPcm.wFormatTag = WAVE_FORMAT_PCM;
                            forcedPcm.nChannels = 2;
                            forcedPcm.nSamplesPerSec = 48000;
                            forcedPcm.wBitsPerSample = 16;
                            forcedPcm.nBlockAlign = (forcedPcm.nChannels * forcedPcm.wBitsPerSample) / 8;
                            forcedPcm.nAvgBytesPerSec = forcedPcm.nSamplesPerSec * forcedPcm.nBlockAlign;
                            forcedPcm.cbSize = 0;
                            targetFormat = &forcedPcm;
                        }
                        std::wcout << L"[AudioCapturer] Forcing 48kHz stereo PCM format for target process optimal latency" << std::endl;
                    }
                }

                hr = m_pAudioClient->Initialize(
                    shareMode,
                    streamFlags,
                    hnsRequestedDuration,
                    0,
                    targetFormat,
                    (targetSessionGuid == GUID_NULL ? NULL : &targetSessionGuid));

                // If exclusive mode failed and we prefer it, try shared mode as fallback
                if (FAILED(hr) && shareMode == AUDCLNT_SHAREMODE_EXCLUSIVE) {
                    std::wcerr << L"[AudioCapturer] Exclusive mode failed, falling back to shared mode. Error: " << _com_error(hr).ErrorMessage() << std::endl;
                    // In shared mode, use the device mix format (pwfx) rather than forcing PCM16
                    targetFormat = const_cast<WAVEFORMATEX*>(pwfx);
                    hr = m_pAudioClient->Initialize(
                        AUDCLNT_SHAREMODE_SHARED,
                        streamFlags,
                        hnsRequestedDuration,
                        0,
                        targetFormat,
                        (targetSessionGuid == GUID_NULL ? NULL : &targetSessionGuid));
                }

                if (FAILED(hr))
                {
                    if (s_audioConfig.wasapi.enforceEventDriven) {
                        std::wcerr << L"[AudioCapturer] Event-driven mode failed and enforcement is enabled. Error: " << _com_error(hr).ErrorMessage() << std::endl;
                        goto Exit;
                    } else {
                        std::wcerr << L"[AudioCapturer] Event-driven init failed; retrying in polling mode. Error: " << _com_error(hr).ErrorMessage() << std::endl;
                        // Retry without EVENTCALLBACK
                        hr = m_pAudioClient->Initialize(
                            AUDCLNT_SHAREMODE_SHARED,
                            AUDCLNT_STREAMFLAGS_LOOPBACK,
                            hnsRequestedDuration,
                            0,
                            const_cast<const WAVEFORMATEX*>(pwfx),
                            (targetSessionGuid == GUID_NULL ? NULL : &targetSessionGuid));
                        if (FAILED(hr)) {
                            std::wcerr << L"[AudioCapturer] Unable to initialize audio client for target process: " << _com_error(hr).ErrorMessage() << std::endl;
                            goto Exit;
                        }
                    }
                }

                // Update active format fields to match actual capture format
                if (targetFormat) {
                    m_activeChannels = static_cast<uint16_t>(targetFormat->nChannels);
                    m_activeSampleRate = static_cast<uint32_t>(targetFormat->nSamplesPerSec);
                    m_activeBitsPerSample = targetFormat->wBitsPerSample;
                    if (targetFormat->wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
                        WAVEFORMATEXTENSIBLE* pEx = reinterpret_cast<WAVEFORMATEXTENSIBLE*>(targetFormat);
                        m_activeWavAudioFormat = (pEx->SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT) ? 3 : 1;
                    } else if (targetFormat->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) {
                        m_activeWavAudioFormat = 3;
                    } else {
                        m_activeWavAudioFormat = 1;
                    }
                }

                hr = m_pAudioClient->GetBufferSize(&bufferFrameCount);
                if (FAILED(hr))
                {
                    std::wcerr << L"[AudioCapturer] Unable to get buffer size for target process device: " << _com_error(hr).ErrorMessage() << std::endl;
                    goto Exit;
                }

                std::wcout << L"[AudioCapturer] Buffer Frame Count: " << bufferFrameCount << std::endl;

                // Ensure working buffers can hold the device buffer size (avoid dropping frames)
                size_t requiredSamples = static_cast<size_t>(bufferFrameCount) * static_cast<size_t>(pwfx->nChannels);
                if (m_floatBuffer.size() < requiredSamples) {
                    m_floatBuffer.resize(requiredSamples);
                }

                hr = m_pAudioClient->GetService(__uuidof(IAudioCaptureClient), reinterpret_cast<void**>(m_pCaptureClient.ReleaseAndGetAddressOf()));
                if (FAILED(hr))
                {
                    std::wcerr << L"[AudioCapturer] Unable to get capture client for target process device: " << _com_error(hr).ErrorMessage() << std::endl;
                    goto Exit;
                }
                // Query IAudioClock for precise timestamps
                hr = m_pAudioClient->GetService(__uuidof(IAudioClock), reinterpret_cast<void**>(m_pAudioClock.ReleaseAndGetAddressOf()));
                if (SUCCEEDED(hr) && m_pAudioClock) {
                    UINT64 freq = 0;
                    if (SUCCEEDED(m_pAudioClock->GetFrequency(&freq))) {
                        m_audioClockFreq = freq;
                        std::wcout << L"[AudioCapturer] Process loopback AudioClock frequency: " << freq << std::endl;
                    }
                }
                
                // Found and initialized, break out of loops
                break; 
            }
            pSessionControl2->Release();
            pSessionControl->Release();
        }
        if (m_pAudioClient) break; // If client found, break outer loop too

        if (pSessionEnumerator) {
            pSessionEnumerator->Release();
            pSessionEnumerator = NULL;
        }
        if (pSessionManager) {
            pSessionManager->Release();
            pSessionManager = NULL;
        }
        if (pDevice) {
            pDevice->Release();
            pDevice = NULL;
        }
    }

    // ============================================================================
    // Default/device-selected loopback path
    // ============================================================================
    if (!m_pAudioClient)
    {
        // 1) deviceId override
        if (!s_audioConfig.deviceId.empty()) {
            IMMDevice* dev = nullptr;
            hr = m_pEnumerator->GetDevice(s_audioConfig.deviceId.c_str(), &dev);
            if (SUCCEEDED(hr) && dev) {
                m_pDevice = dev;
                std::wcout << L"[AudioCapturer] Using configured deviceId: " << s_audioConfig.deviceId << std::endl;
            } else {
                std::wcerr << L"[AudioCapturer] deviceId override failed: " << _com_error(hr).ErrorMessage() << std::endl;
            }
        }

        // 2) enumerate endpoints to find one hosting target PID
        if (!m_pDevice && m_targetProcessId != 0) {
            std::wcout << L"[AudioCapturer] 🔍 DEBUG: About to scan audio devices for target PID=" << m_targetProcessId << std::endl;
            std::wcout << L"[AudioCapturer] 🔍 Scanning all active audio devices for target process sessions..." << std::endl;

            // Safety check: ensure enumerator is valid
            if (!m_pEnumerator) {
                std::wcerr << L"[AudioCapturer] ERROR: Audio enumerator is NULL! Cannot scan devices." << std::endl;
                std::wcout << L"[AudioCapturer] Will fallback to default device loopback" << std::endl;
                goto AfterDeviceSelection; // Skip to default device fallback
            }

            IMMDeviceCollection* pCollection = NULL;
            hr = m_pEnumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &pCollection);
            if (FAILED(hr)) {
                std::wcerr << L"[AudioCapturer] ERROR: EnumAudioEndpoints failed with HRESULT: 0x" << std::hex << hr << std::dec << std::endl;
                std::wcout << L"[AudioCapturer] Will fallback to default device loopback" << std::endl;
                goto AfterDeviceSelection; // Skip to default device fallback
            }

            if (!pCollection) {
                std::wcerr << L"[AudioCapturer] ERROR: EnumAudioEndpoints returned NULL collection" << std::endl;
                std::wcout << L"[AudioCapturer] Will fallback to default device loopback" << std::endl;
                goto AfterDeviceSelection; // Skip to default device fallback
            }

            UINT count = 0;
            hr = pCollection->GetCount(&count);
            if (FAILED(hr)) {
                std::wcerr << L"[AudioCapturer] ERROR: GetCount failed with HRESULT: 0x" << std::hex << hr << std::dec << std::endl;
                pCollection->Release();
                std::wcout << L"[AudioCapturer] Will fallback to default device loopback" << std::endl;
                goto AfterDeviceSelection; // Skip to default device fallback
            }

            std::wcout << L"[AudioCapturer] DEBUG: EnumAudioEndpoints succeeded, found " << count << L" active audio devices" << std::endl;
            std::wcout << L"[AudioCapturer] Found " << count << L" active audio devices" << std::endl;

            for (UINT i = 0; i < count && !m_pDevice; ++i) {
                IMMDevice* pDev = NULL;
                hr = pCollection->Item(i, &pDev);
                if (FAILED(hr)) {
                    std::wcerr << L"[AudioCapturer] ERROR: Item(" << i << L") failed with HRESULT: 0x" << std::hex << hr << std::dec << std::endl;
                    continue;
                }

                if (!pDev) {
                    std::wcerr << L"[AudioCapturer] ERROR: Item(" << i << L") returned NULL device" << std::endl;
                    continue;
                }

                // Get device friendly name for logging
                IPropertyStore* pProps = NULL;
                LPWSTR deviceName = NULL;
                if (SUCCEEDED(pDev->OpenPropertyStore(STGM_READ, &pProps)) && pProps) {
                    PROPVARIANT varName;
                    PropVariantInit(&varName);
                    if (SUCCEEDED(pProps->GetValue(PKEY_Device_FriendlyName, &varName))) {
                        deviceName = varName.pwszVal;
                    }
                    PropVariantClear(&varName);
                    pProps->Release();
                }
                std::wcout << L"[AudioCapturer]   Device " << i << L": " << (deviceName ? deviceName : L"Unknown") << std::endl;

                IAudioSessionManager2* pMgr = NULL;
                hr = pDev->Activate(__uuidof(IAudioSessionManager2), CLSCTX_ALL, NULL, (void**)&pMgr);
                if (FAILED(hr)) {
                    std::wcerr << L"[AudioCapturer] ERROR: Activate(IAudioSessionManager2) failed with HRESULT: 0x" << std::hex << hr << std::dec << std::endl;
                    pDev->Release();
                    continue;
                }

                if (!pMgr) {
                    std::wcerr << L"[AudioCapturer] ERROR: Activate returned NULL session manager" << std::endl;
                    pDev->Release();
                    continue;
                }

                IAudioSessionEnumerator* pEnum = NULL;
                hr = pMgr->GetSessionEnumerator(&pEnum);
                if (FAILED(hr)) {
                    std::wcerr << L"[AudioCapturer] ERROR: GetSessionEnumerator failed with HRESULT: 0x" << std::hex << hr << std::dec << std::endl;
                    pMgr->Release();
                    pDev->Release();
                    continue;
                }

                // Now enumerate sessions on this device
                int sc = 0;
                if (SUCCEEDED(pEnum->GetCount(&sc))) {
                    std::wcout << L"[AudioCapturer]     Sessions on this device: " << sc << std::endl;
                    for (int s = 0; s < sc && !m_pDevice; ++s) {
                        IAudioSessionControl* pCtrl = NULL;
                        if (SUCCEEDED(pEnum->GetSession(s, &pCtrl)) && pCtrl) {
                                                IAudioSessionControl2* pCtrl2 = NULL;
                                                if (SUCCEEDED(pCtrl->QueryInterface(__uuidof(IAudioSessionControl2), (void**)&pCtrl2)) && pCtrl2) {
                                                    DWORD pid = 0;
                                                    if (SUCCEEDED(pCtrl2->GetProcessId(&pid))) {
                                                        // Get process name for better logging and matching
                                                        std::wstring processName = L"Unknown";
                                                        std::string processNameLower = "unknown";
                                                        HANDLE hProcess = OpenProcess(PROCESS_QUERY_INFORMATION, FALSE, pid);
                                                        if (hProcess) {
                                                            WCHAR exeName[MAX_PATH];
                                                            DWORD exeNameSize = MAX_PATH;
                                                            if (QueryFullProcessImageNameW(hProcess, 0, exeName, &exeNameSize)) {
                                                                std::filesystem::path exePath(exeName);
                                                                processName = exePath.filename().wstring();
                                                                processNameLower = std::string(processName.begin(), processName.end());
                                                                std::transform(processNameLower.begin(), processNameLower.end(), processNameLower.begin(), ::tolower);
                                                            }
                                                            CloseHandle(hProcess);
                                                        }

                                                        std::wcout << L"[AudioCapturer]       Session " << s << L": PID=" << pid << L" (" << processName << L")" << std::endl;
                                                        // Match either by PID or by process name
                                                        bool isTargetProcess = (pid == m_targetProcessId);
                                                        if (!isTargetProcess && !targetProcessNameLower.empty() && !processNameLower.empty()) {
                                                            // Check if the session process name matches our target process name
                                                            isTargetProcess = (processNameLower.find(targetProcessNameLower) != std::string::npos);
                                                        }

                                                        if (isTargetProcess) {
                                                            m_pDevice = pDev; // take ownership
                                                            std::wcout << L"[AudioCapturer] 🎯 FOUND TARGET PROCESS SESSION! Selected endpoint hosting PID=" << pid << L" (" << processName << L")" << std::endl;
                                                            std::wcout << L"[AudioCapturer] Device: " << (deviceName ? deviceName : L"Unknown") << std::endl;
                                                            pCtrl2->Release();
                                                            pCtrl->Release();
                                                            pEnum->Release();
                                                            pMgr->Release();
                                                            pCollection->Release();
                                                            goto DeviceSelected; // break all
                                                        }
                                                    }
                                                    pCtrl2->Release();
                                                }
                            pCtrl->Release();
                            }
                        pEnum->Release();    
                        pMgr->Release();
                    }
                    pDev->Release();
                }
            }
        }
        pCollection->Release();

        // Log if we didn't find target process on any device
        if (!m_pDevice && m_targetProcessId != 0) {
            std::wcout << L"[AudioCapturer] ❌ Target process sessions NOT found on any audio device!" << std::endl;
            std::wcout << L"[AudioCapturer] This could mean:" << std::endl;
            std::wcout << L"[AudioCapturer]   • Target process hasn't started producing audio yet" << std::endl;
            std::wcout << L"[AudioCapturer]   • Target process is using a different audio device (not listed)" << std::endl;
            std::wcout << L"[AudioCapturer]   • Application is routing audio through middleware/overlay" << std::endl;
            std::wcout << L"[AudioCapturer]   • Target process audio is muted or disabled" << std::endl;
            std::wcout << L"[AudioCapturer] Will fallback to default device loopback" << std::endl;
        }
    }

    // 3) fallback to default device
    std::wcout << L"[AudioCapturer] DEBUG: Attempting to get default audio device..." << std::endl;

    // Safety check: ensure enumerator is still valid
    if (!m_pEnumerator) {
        std::wcerr << L"[AudioCapturer] ERROR: Audio enumerator became NULL before default device fallback!" << std::endl;
        goto Exit;
    }

        hr = m_pEnumerator->GetDefaultAudioEndpoint(eRender, eMultimedia, &m_pDevice);
DeviceSelected:
        if (FAILED(hr)){
            std::wcerr << L"[AudioCapturer] Failed to get default render device: " << _com_error(hr).ErrorMessage()
                       << L" (HRESULT: 0x" << std::hex << hr << std::dec << L")" << std::endl;
            std::wcerr << L"[AudioCapturer] Audio capture initialization failed completely." << std::endl;
            std::wcerr << L"[AudioCapturer] Possible causes:" << std::endl;
            std::wcerr << L"[AudioCapturer]   - No audio devices available" << std::endl;
            std::wcerr << L"[AudioCapturer]   - Audio service not running" << std::endl;
            std::wcerr << L"[AudioCapturer]   - Insufficient permissions" << std::endl;
            std::wcerr << L"[AudioCapturer]   - COM initialization issues" << std::endl;
        goto Exit;
        }

        if (!m_pDevice) {
            std::wcerr << L"[AudioCapturer] ERROR: GetDefaultAudioEndpoint returned NULL device!" << std::endl;
            goto Exit;
        }

        // Log which device we're using
        if (m_pDevice) {
            IPropertyStore* pProps = NULL;
            if (SUCCEEDED(m_pDevice->OpenPropertyStore(STGM_READ, &pProps)) && pProps) {
                PROPVARIANT varName;
                PropVariantInit(&varName);
                if (SUCCEEDED(pProps->GetValue(PKEY_Device_FriendlyName, &varName))) {
                    std::wcout << L"[AudioCapturer] 📢 Using default audio device: " << varName.pwszVal << std::endl;
                }
                PropVariantClear(&varName);
                pProps->Release();
            }
        }

        std::wcout << L"[AudioCapturer] Capture path selected: DEVICE_LOOPBACK (default device)" << std::endl;
        std::wcout << L"[AudioCapturer] Using system audio capture - this will capture ALL audio on the default device" << std::endl;

        // Generic troubleshooting guidance
        if (!m_targetProcessName.empty()) {
            std::wcout << L"[AudioCapturer] 📋 Troubleshooting for " << std::wstring(m_targetProcessName.begin(), m_targetProcessName.end()) << L":" << std::endl;
            std::wcout << L"[AudioCapturer]   • Make sure the application is not muted in Windows Volume Mixer" << std::endl;
            std::wcout << L"[AudioCapturer]   • Check if the application is outputting to the expected audio device" << std::endl;
            std::wcout << L"[AudioCapturer]   • Verify the application is actually producing audio" << std::endl;
            std::wcout << L"[AudioCapturer]   • Try restarting the application after starting this capture" << std::endl;
            std::wcout << L"[AudioCapturer]   • Check if any overlays or middleware are interfering with audio" << std::endl;
        }

        hr = m_pDevice->Activate(__uuidof(IAudioClient), CLSCTX_ALL, NULL, reinterpret_cast<void**>(m_pAudioClient.ReleaseAndGetAddressOf()));
        if (FAILED(hr))
        {
            std::wcerr << L"[AudioCapturer] Unable to activate audio client for default device: " << _com_error(hr).ErrorMessage() << std::endl;
            goto Exit;
        }

        hr = m_pAudioClient->GetMixFormat(&pwfx);
        if (FAILED(hr))
        {
            std::wcerr << L"[AudioCapturer] Unable to get mix format for default device: " << _com_error(hr).ErrorMessage() << std::endl;
            goto Exit;
        }

        std::wcout << L"[AudioCapturer] Default device mix format: wFormatTag=" << pwfx->wFormatTag
            << L", nChannels=" << pwfx->nChannels
            << L", nSamplesPerSec=" << pwfx->nSamplesPerSec
            << L", nAvgBytesPerSec=" << pwfx->nAvgBytesPerSec
            << L", nBlockAlign=" << pwfx->nBlockAlign
            << L", wBitsPerSample=" << pwfx->wBitsPerSample
            << L", cbSize=" << pwfx->cbSize << std::endl;

        // Try exclusive mode first if preferred, then shared mode with event-driven capture
        streamFlags = AUDCLNT_STREAMFLAGS_LOOPBACK | AUDCLNT_STREAMFLAGS_EVENTCALLBACK;
        shareMode = s_audioConfig.wasapi.preferExclusiveMode ?
            AUDCLNT_SHAREMODE_EXCLUSIVE : AUDCLNT_SHAREMODE_SHARED;

        // If forcing 48kHz stereo, modify the format before initialization
        targetFormat = const_cast<WAVEFORMATEX*>(pwfx);
        if (s_audioConfig.wasapi.force48kHzStereo) {
            // Check if device already provides optimal format to avoid unnecessary conversions
            bool isAlreadyOptimal = (pwfx->nSamplesPerSec == 48000 &&
                                   pwfx->nChannels == 2 &&
                                   pwfx->wBitsPerSample == 16 &&
                                   pwfx->wFormatTag == WAVE_FORMAT_PCM);

            if (isAlreadyOptimal) {
                std::wcout << L"[AudioCapturer] Device already provides optimal 48kHz stereo PCM format - no conversion needed" << std::endl;
                targetFormat = const_cast<WAVEFORMATEX*>(pwfx);
            } else {
                // Create a new format structure with 48kHz stereo
                static WAVEFORMATEX forcedFormat = {};
                forcedFormat.wFormatTag = WAVE_FORMAT_PCM;
                forcedFormat.nChannels = 2; // Force stereo
                forcedFormat.nSamplesPerSec = 48000; // Force 48kHz
                forcedFormat.nAvgBytesPerSec = 48000 * 2 * 2; // 48kHz * 2 channels * 2 bytes per sample (16-bit)
                forcedFormat.nBlockAlign = 2 * 2; // 2 channels * 2 bytes per sample
                forcedFormat.wBitsPerSample = 16;
                forcedFormat.cbSize = 0;
                targetFormat = &forcedFormat;
                std::wcout << L"[AudioCapturer] Forcing 48kHz stereo PCM format for optimal latency" << std::endl;
            }
        }

        // Query default device session GUID for per-process loopback only if we found a target session earlier; otherwise fallback
        GUID defaultSessionGuid = GUID_NULL;
        if (m_pSessionControl2) {
            HRESULT hrGp = m_pSessionControl2->GetGroupingParam(&defaultSessionGuid);
            if (FAILED(hrGp)) {
                defaultSessionGuid = GUID_NULL;
            }
        }

        hr = m_pAudioClient->Initialize(
            shareMode,
            streamFlags,
            hnsRequestedDuration,
            0,
            targetFormat,
            (defaultSessionGuid == GUID_NULL ? NULL : &defaultSessionGuid));

        // If exclusive mode failed and we prefer it, try shared mode as fallback
        if (FAILED(hr) && shareMode == AUDCLNT_SHAREMODE_EXCLUSIVE) {
            std::wcerr << L"[AudioCapturer] Default device exclusive mode failed, falling back to shared mode. Error: " << _com_error(hr).ErrorMessage() << std::endl;
            // In shared mode, use the device mix format (pwfx) rather than forcing PCM16
            targetFormat = const_cast<WAVEFORMATEX*>(pwfx);
            hr = m_pAudioClient->Initialize(
                AUDCLNT_SHAREMODE_SHARED,
                streamFlags,
                hnsRequestedDuration,
                0,
                targetFormat,
                (defaultSessionGuid == GUID_NULL ? NULL : &defaultSessionGuid));
        }

        if (FAILED(hr))
        {
            if (s_audioConfig.wasapi.enforceEventDriven) {
                std::wcerr << L"[AudioCapturer] Default device event-driven mode failed and enforcement is enabled. Error: " << _com_error(hr).ErrorMessage() << std::endl;
                goto Exit;
            } else {
                std::wcerr << L"[AudioCapturer] Event-driven init failed for default device; retrying in polling mode. Error: " << _com_error(hr).ErrorMessage() << std::endl;
                // Retry without EVENTCALLBACK
                hr = m_pAudioClient->Initialize(
                    AUDCLNT_SHAREMODE_SHARED,
                    AUDCLNT_STREAMFLAGS_LOOPBACK,
                    hnsRequestedDuration,
                    0,
                    const_cast<const WAVEFORMATEX*>(pwfx),
                    (defaultSessionGuid == GUID_NULL ? NULL : &defaultSessionGuid));
                if (FAILED(hr)) {
                    std::wcerr << L"[AudioCapturer] Unable to initialize audio client for default device: " << _com_error(hr).ErrorMessage() << std::endl;
                    goto Exit;
                }
            }
        }

        // Use the actual format that was requested for capture path
        if (targetFormat) {
            pwfx = targetFormat;
            // Update active format fields to match actual capture format
            m_activeChannels = static_cast<uint16_t>(pwfx->nChannels);
            m_activeSampleRate = static_cast<uint32_t>(pwfx->nSamplesPerSec);
            m_activeBitsPerSample = pwfx->wBitsPerSample;
            if (pwfx->wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
                WAVEFORMATEXTENSIBLE* pEx = reinterpret_cast<WAVEFORMATEXTENSIBLE*>(pwfx);
                m_activeWavAudioFormat = (pEx->SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT) ? 3 : 1;
            } else if (pwfx->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) {
                m_activeWavAudioFormat = 3;
            } else {
                m_activeWavAudioFormat = 1;
            }
        }

        hr = m_pAudioClient->GetBufferSize(&bufferFrameCount);
        if (FAILED(hr))
        {
            std::wcerr << L"[AudioCapturer] Unable to get buffer size for default device: " << _com_error(hr).ErrorMessage() << std::endl;
            goto Exit;
        }

        std::wcout << L"[AudioCapturer] Default device buffer frame count: " << bufferFrameCount << std::endl;

        // Ensure working buffers can hold the device buffer size (avoid dropping frames)
        requiredSamplesDefault = static_cast<size_t>(bufferFrameCount) * static_cast<size_t>(pwfx->nChannels);
        if (m_floatBuffer.size() < requiredSamplesDefault) {
            m_floatBuffer.resize(requiredSamplesDefault);
        }

        hr = m_pAudioClient->GetService(__uuidof(IAudioCaptureClient), reinterpret_cast<void**>(m_pCaptureClient.ReleaseAndGetAddressOf()));
        if (FAILED(hr))
        {
            std::wcerr << L"[AudioCapturer] Unable to get capture client for default device: " << _com_error(hr).ErrorMessage() << std::endl;
            goto Exit;
        }

        // Query IAudioClock for precise timestamps
        hr = m_pAudioClient->GetService(__uuidof(IAudioClock), reinterpret_cast<void**>(m_pAudioClock.ReleaseAndGetAddressOf()));
        if (SUCCEEDED(hr) && m_pAudioClock) {
            UINT64 freq = 0;
            if (SUCCEEDED(m_pAudioClock->GetFrequency(&freq))) {
                m_audioClockFreq = freq;
                std::wcout << L"[AudioCapturer] Default device AudioClock frequency: " << freq << std::endl;
            }
        }

        std::wcout << L"[AudioCapturer] Successfully initialized default device with loopback capture." << std::endl;
    

AfterDeviceSelection:
    // Release local COM objects that are no longer needed
    if (pSessionEnumerator) pSessionEnumerator->Release();
    if (pSessionManager) pSessionManager->Release();
    if (pCollection) pCollection->Release();
    // pDevice is released when m_pDevice is released, or if not found, it's released in the loop.
    // pSessionControl and pSessionControl2 are released in the loop or assigned to member variable.

    std::wcout << L"[AudioCapturer] Starting audio capture and Opus encoding..." << std::endl;

    // Set up event handles for low-latency event-driven capture and stop signaling
    if (m_pAudioClient) {
        // Create capture event handle if not already created
        if (!m_hCaptureEvent) {
            m_hCaptureEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
            if (!m_hCaptureEvent) {
                std::wcerr << L"[AudioCapturer] Failed to create capture event handle: " << GetLastError() << std::endl;
            }
        }

        // Create stop event handle for clean thread shutdown
        if (!m_hStopEvent) {
            m_hStopEvent = CreateEvent(nullptr, TRUE, FALSE, nullptr); // Manual reset event
            if (!m_hStopEvent) {
                std::wcerr << L"[AudioCapturer] Failed to create stop event handle: " << GetLastError() << std::endl;
            }
        }

        if (m_hCaptureEvent) {
            hr = m_pAudioClient->SetEventHandle(m_hCaptureEvent);
            if (SUCCEEDED(hr)) {
                eventMode = true;
                m_currentlyUsingEventMode = true;
                AUDIO_LOG_INFO(L"[AudioCapturer] Using event-driven capture mode (optimal latency)");
            } else {
                eventMode = false;
                m_currentlyUsingEventMode = false;
                AUDIO_LOG_ERROR(L"[AudioCapturer] Event-driven mode setup failed (HRESULT: 0x" << std::hex << hr
                               << std::dec << L"), falling back to polling mode. Error: " << _com_error(hr).ErrorMessage());

                // Clean up event handle since we can't use it
                CloseHandle(m_hCaptureEvent);
                m_hCaptureEvent = nullptr;
            }
        } else {
            eventMode = false;
            m_currentlyUsingEventMode = false;
            AUDIO_LOG_ERROR(L"[AudioCapturer] Event handle creation failed, using polling mode");
        }
    }

    // Log capture mode and timing parameters
    if (eventMode) {
        std::wcout << L"[AudioCapturer] Event-driven mode: 5ms timeout for low-latency responsiveness." << std::endl;
    } else {
        std::wcout << L"[AudioCapturer] Polling mode: " << sleepMs << L"ms intervals, Opus frame-aligned." << std::endl;
    }

    hr = m_pAudioClient->Start();  // Start recording.
    if (FAILED(hr))
    {
        std::wcerr << L"Unable to start recording: " << _com_error(hr).ErrorMessage() << std::endl;
        goto Exit;
    }

    // Calculate optimal polling interval aligned with Opus frame timing
    // Opus uses 10ms frames (480 samples at 48kHz) for low-latency gaming
    if (pwfx && pwfx->nSamplesPerSec) {
        // Calculate buffer duration in milliseconds
        DWORD bufferMs = (bufferFrameCount * 1000u) / pwfx->nSamplesPerSec;

        // Calculate how many complete Opus frames fit in the buffer
        DWORD framesInBuffer = bufferMs / OPUS_FRAME_MS;

        // Optimal polling strategy for low-latency capture:
        // - Poll at most every OPUS_FRAME_MS (10ms) to align with frame boundaries
        // - For small buffers, poll at quarter-buffer rate (but not more than 10ms)
        // - For large buffers, use fixed 10ms polling aligned with frame timing

        if (bufferMs <= 50) {
            // Small buffer: poll frequently but align with frame boundaries
            sleepMs = (std::max<DWORD>)(1, (std::min<DWORD>)(bufferMs / 4, OPUS_FRAME_MS));
        } else {
            // Large buffer: use frame-aligned polling interval
            sleepMs = OPUS_FRAME_MS;
        }

        // Ensure we never exceed Opus frame duration for optimal alignment
        sleepMs = (std::min<DWORD>)(sleepMs, OPUS_FRAME_MS);

        std::wcout << L"[AudioCapturer] Buffer: " << bufferFrameCount << L" frames (" << bufferMs
                   << L"ms ≈ " << framesInBuffer << L" Opus frames), polling every " << sleepMs
                   << L"ms (aligned with " << OPUS_FRAME_MS << L"ms Opus frames)" << std::endl;
    } else {
        // Fallback: align with Opus frame timing even for unknown formats
        sleepMs = OPUS_FRAME_MS;
        std::wcout << L"[AudioCapturer] Using Opus-aligned polling interval: " << sleepMs << L"ms" << std::endl;
    }

    while (m_stopCapture == false)
    {
        captureCycles++;

        // DEBUG: Log every 100 cycles to verify capture loop is running
        if (captureCycles % 500 == 0) {
            std::wcout << L"[AudioCapturer] DEBUG: Capture loop iteration " << captureCycles << L" - still running" << std::endl;
        }

        // Periodic heartbeat log to verify capture loop is running
        if (captureCycles % 1000 == 0) {
            AUDIO_LOG_DEBUG(L"[AudioCapturer] Capture loop heartbeat: cycle " << captureCycles << L", processed " << dataPacketsProcessed << L" packets");
        }

        // Periodic latency status reporting
        if (captureCycles % 30000 == 0) { // Every 30 seconds (assuming 1000 cycles/second)
            this->ReportLatencyStats();
        }

        // Periodic error statistics logging (every 1000 cycles if there were errors)
        if (captureCycles % 1000 == 0 && m_errorStats.totalErrors > 0) {
            LogErrorStats();
        }

        if (m_currentlyUsingEventMode && m_hCaptureEvent && m_hStopEvent) {
            // Use WaitForMultipleObjects for clean stop signaling
            // Wait on both capture event (data available) and stop event (shutdown requested)
            HANDLE handles[2] = { m_hCaptureEvent, m_hStopEvent };
            DWORD wait = WaitForMultipleObjects(2, handles, FALSE, INFINITE);

            if (wait == WAIT_OBJECT_0) {
                // Capture event signaled - data available, proceed with capture
            } else if (wait == WAIT_OBJECT_0 + 1) {
                // Stop event signaled - clean shutdown requested
                std::wcout << L"[AudioCapturer] Stop event signaled, initiating clean shutdown..." << std::endl;
                break; // Exit the capture loop
            } else if (wait == WAIT_TIMEOUT) {
                // This shouldn't happen with INFINITE timeout, but handle it
                std::wcerr << L"[AudioCapturer] WaitForMultipleObjects timeout (unexpected)" << std::endl;
                continue;
            }

        } else {
            // Fallback polling mode with stop signal check
            // Use shorter sleep intervals to remain responsive to stop signals
            for (DWORD slept = 0; slept < sleepMs && !m_stopCapture; slept += 1) {
                Sleep(1); // Sleep in 1ms increments to check stop flag frequently
            }

            if (m_stopCapture) {
                std::wcout << L"[AudioCapturer] Stop signal detected in polling mode, shutting down..." << std::endl;
                break;
            }
        }

        // std::cout << "[AUDIO_DEBUG] CAPTURE LOOP ITERATION - Checking for audio data" << std::endl;

        hr = m_pCaptureClient->GetNextPacketSize(&numFramesAvailable);
        AUDIO_LOG_DEBUG(L"[AudioCapturer] GetNextPacketSize: hr=" << hr << L", frames=" << numFramesAvailable);

        // Debug: Log when we get audio data
        if (numFramesAvailable > 0) {
            AUDIO_LOG_INFO(L"[AudioCapturer] AUDIO DATA RECEIVED - Frames: " << numFramesAvailable);
        } else {
            AUDIO_LOG_INFO(L"[AudioCapturer] NO AUDIO DATA AVAILABLE - Frames: " << numFramesAvailable);
        }

        if (FAILED(hr))
        {
            LogErrorWithContext(hr, L"Failed to get next packet size", m_consecutiveErrorCount);

            // Try enhanced recovery with exponential backoff
            if (ShouldRetryError(hr, m_consecutiveErrorCount)) {
                m_consecutiveErrorCount++;

                // Try comprehensive recovery
                if (TryRecoverFromError(hr, L"GetNextPacketSize")) {
                    // Recovery successful, continue
                    m_consecutiveErrorCount = 0; // Reset on success
                    continue;
                } else {
                    // Recovery failed, try exponential backoff
                    int delay = CalculateRetryDelay(m_consecutiveErrorCount);
                    AUDIO_LOG_DEBUG(L"[AudioCapturer] Recovery failed, backing off for " << delay << L"ms");
                    Sleep(delay);
                    continue;
                }
            }

            // Check if system is under load and we should be more aggressive with retries
            if (IsSystemUnderLoad() && m_consecutiveErrorCount < m_retryConfig.maxRetries * 2) {
                AUDIO_LOG_DEBUG(L"[AudioCapturer] System under load, extending retry attempts");
                int delay = CalculateRetryDelay(m_consecutiveErrorCount);
                Sleep(delay);
                m_consecutiveErrorCount++;
                continue;
            }

            // Log error statistics before giving up
            LogErrorStats();

            // If we can't recover, exit
            AUDIO_LOG_ERROR(L"[AudioCapturer] Unable to recover from GetNextPacketSize error after "
                          << m_consecutiveErrorCount << L" attempts, terminating capture");
            goto Exit;
        }

        // Reset consecutive error count on successful operation
        m_consecutiveErrorCount = 0;

        AUDIO_LOG_INFO(L"[AudioCapturer] ENTERING FRAME PROCESSING LOOP - Available frames: " << numFramesAvailable);

        while (numFramesAvailable != 0)
        {
            dataPacketsProcessed++;
            AUDIO_LOG_INFO(L"[AudioCapturer] PROCESSING FRAME - Packet " << dataPacketsProcessed << L", Frames: " << numFramesAvailable);

            UINT64 devPos = 0, qpcPos = 0;
            hr = m_pCaptureClient->GetBuffer(
                &pData,
                &numFramesAvailable,
                &flags,
                &devPos,
                &qpcPos);

            AUDIO_LOG_DEBUG(L"[AudioCapturer] GetBuffer result: hr=" << hr << L", frames=" << numFramesAvailable);

            if (FAILED(hr))
            {
                LogErrorWithContext(hr, L"Failed to get buffer", m_consecutiveErrorCount);

                // Try enhanced recovery with exponential backoff
                if (ShouldRetryError(hr, m_consecutiveErrorCount)) {
                    m_consecutiveErrorCount++;

                    // Try comprehensive recovery
                    if (TryRecoverFromError(hr, L"GetBuffer")) {
                        // Recovery successful, break to restart outer loop if needed
                        m_consecutiveErrorCount = 0; // Reset on success
                        break;
                    } else {
                        // Recovery failed, try exponential backoff
                        int delay = CalculateRetryDelay(m_consecutiveErrorCount);
                        AUDIO_LOG_DEBUG(L"[AudioCapturer] GetBuffer recovery failed, backing off for " << delay << L"ms");
                        Sleep(delay);
                        continue;
                    }
                }

                // Check if system is under load and extend retry attempts
                if (IsSystemUnderLoad() && m_consecutiveErrorCount < m_retryConfig.maxRetries * 2) {
                    AUDIO_LOG_DEBUG(L"[AudioCapturer] System under load, extending GetBuffer retry attempts");
                    int delay = CalculateRetryDelay(m_consecutiveErrorCount);
                    Sleep(delay);
                    m_consecutiveErrorCount++;
                    continue;
                }

                // Log error statistics before giving up
                LogErrorStats();

                // If we can't recover, exit
                AUDIO_LOG_ERROR(L"[AudioCapturer] Unable to recover from GetBuffer error after "
                              << m_consecutiveErrorCount << L" attempts, terminating capture");
                goto Exit;
            }

            // Reset consecutive error count on successful operation
            m_consecutiveErrorCount = 0;

            // ============================================================================
            // ZERO-ALLOCATION AUDIO PROCESSING PIPELINE - Optimized for minimal latency
            // ============================================================================
            // 1. Convert PCM to float directly in pre-allocated buffer (eliminates allocation)
            // 2. Resample in-place using DMO with efficient buffer swaps
            // 3. Feed encoder directly from persistent buffers (no intermediate copies)
            // 4. Fixed-size buffers eliminate resize overhead during runtime
            // ============================================================================

            size_t totalSamples = static_cast<size_t>(numFramesAvailable) * static_cast<size_t>(pwfx->nChannels);

            // Ensure working buffer is large enough (use capacity growth instead of dropping)
            if (totalSamples > m_floatBuffer.size()) {
                if (m_floatBuffer.capacity() < totalSamples) {
                    try { m_floatBuffer.reserve(totalSamples); }
                    catch (...) { /* reserve best-effort */ }
                }
                m_floatBuffer.resize(totalSamples);
            }

            // Convert PCM to float directly in the persistent buffer
            if (flags & AUDCLNT_BUFFERFLAGS_SILENT) {
                // Silent frame - fill with zeros
                AUDIO_LOG_DEBUG(L"[AudioCapturer] Silent frame detected");
                std::fill(m_floatBuffer.begin(), m_floatBuffer.begin() + totalSamples, 0.0f);
            } else {
                // Convert PCM to float directly in pre-allocated buffer (zero-allocation)
                AUDIO_LOG_DEBUG(L"[AudioCapturer] Converting PCM data: " << numFramesAvailable << L" frames");
                if (!ConvertPCMToFloatInPlace(pData, numFramesAvailable, static_cast<void*>(pwfx), m_floatBuffer.data(), m_floatBuffer.size())) {
                    AUDIO_LOG_ERROR(L"[AudioCapturer] Failed to convert PCM to float format");
                    goto Exit;
                }

                // Check if the converted data has any non-zero values
                bool hasAudioData = false;
                for (size_t i = 0; i < totalSamples && i < 100; ++i) { // Check first 100 samples
                    if (std::abs(m_floatBuffer[i]) > 0.0001f) { // Threshold for silence
                        hasAudioData = true;
                        break;
                    }
                }
                AUDIO_LOG_DEBUG(L"[AudioCapturer] Audio data check: " << (hasAudioData ? L"Has signal" : L"Silent/near-silent"));
            }

            // Resample to 48kHz if source rate differs (ensure buffer capacity; never skip)
            if (pwfx->nSamplesPerSec != 48000) {
                uint32_t channels = pwfx->nChannels;

                // Calculate expected output size (interleaved samples)
                double ratio = 48000.0 / static_cast<double>(pwfx->nSamplesPerSec);
                size_t expectedOutputSamples = static_cast<size_t>(std::ceil(totalSamples * ratio));

                // Ensure working buffer has enough capacity for the resampled output
                if (m_floatBuffer.capacity() < expectedOutputSamples) {
                    try {
                        m_floatBuffer.reserve(expectedOutputSamples);
                    } catch (...) {
                        std::wcerr << L"[AudioCapturer] Failed to reserve buffer for resampling; proceeding may cause latency/artifacts" << std::endl;
                    }
                }

                // Prefer high-quality path for 44.1 -> 48 kHz; otherwise use constrained in-place
                bool resampled = false;
                if (pwfx->nSamplesPerSec == 44100) {
                    // Use in-place method that attempts DMO first
                    ResampleTo48kInPlace(m_floatBuffer, numFramesAvailable, pwfx->nSamplesPerSec, channels);
                    resampled = true;
                } else {
                    // Use optimized resampling within buffer capacity limits
                    resampled = ResampleTo48kInPlaceConstrained(
                        m_floatBuffer,
                        numFramesAvailable,
                        pwfx->nSamplesPerSec,
                        channels,
                        m_floatBuffer.capacity());
                    if (!resampled) {
                        // Fallback: attempt in-place resampler (may allocate temp)
                        ResampleTo48kInPlace(m_floatBuffer, numFramesAvailable, pwfx->nSamplesPerSec, channels);
                        resampled = true;
                    }
                }

                if (resampled) {
                    // Update totalSamples to reflect the new (resampled) sample count
                    totalSamples = m_floatBuffer.size();
                }
            }

            // Calculate timestamp for this audio data with a single pinned source
            int64_t timestampUs = 0;
            UINT64 audioClockPos = 0;
            UINT64 audioClockQpc = 0;

            // Pin timestamp source once
            if (m_timestampSource == TimestampSource::Unknown) {
                if (m_pAudioClock && m_audioClockFreq > 0 && SUCCEEDED(m_pAudioClock->GetPosition(&audioClockPos, &audioClockQpc))) {
                    m_timestampSource = TimestampSource::AudioClock;
                    timestampUs = static_cast<int64_t>((audioClockPos * 1000000ULL) / m_audioClockFreq);
                    m_initialAudioClockTime = timestampUs;
                    m_lastTimestampUs = timestampUs;
                    std::wcout << L"[AudioCapturer] Timestamp source pinned: IAudioClock (" << m_audioClockFreq << L" Hz), t0=" << m_initialAudioClockTime << L" us" << std::endl;
                } else {
                    m_timestampSource = TimestampSource::SharedReference;
                    timestampUs = GetSharedReferenceTimeUs();
                    m_initialAudioClockTime = timestampUs;
                    m_lastTimestampUs = timestampUs;
                    std::wcout << L"[AudioCapturer] Timestamp source pinned: Shared reference clock, t0=" << m_initialAudioClockTime << L" us" << std::endl;
                }
            } else if (m_timestampSource == TimestampSource::AudioClock) {
                // Stick to IAudioClock; if it momentarily fails, synthesize monotonic time based on last timestamp
                if (m_pAudioClock && m_audioClockFreq > 0 && SUCCEEDED(m_pAudioClock->GetPosition(&audioClockPos, &audioClockQpc))) {
                    timestampUs = static_cast<int64_t>((audioClockPos * 1000000ULL) / m_audioClockFreq);
                } else {
                    // Synthesize timestamp: advance by duration of this captured block
                    uint32_t channels = (pwfx && pwfx->nChannels > 0) ? pwfx->nChannels : s_audioConfig.channels;
                    if (channels == 0) channels = 2;
                    double secondsAdvanced = static_cast<double>(totalSamples) / (48000.0 * static_cast<double>(channels));
                    int64_t deltaUs = static_cast<int64_t>(secondsAdvanced * 1000000.0 + 0.5);
                    timestampUs = m_lastTimestampUs + deltaUs;
                    std::wcerr << L"[AudioCapturer] IAudioClock read failed; synthesizing monotonic timestamp (+" << deltaUs << L" us)" << std::endl;
                }
            } else { // SharedReference
                timestampUs = GetSharedReferenceTimeUs();
            }

            // Initialize base if needed (should be set when source pinned)
            if (m_initialAudioClockTime == 0) {
                m_initialAudioClockTime = timestampUs;
            }
            m_lastTimestampUs = timestampUs;
            
            // Write to WAV (once per captured block) if enabled
            if (m_wavRecordingEnabled) {
                std::lock_guard<std::mutex> wavLock(m_wavMutex);
                WriteWAVData(m_floatBuffer.data(), totalSamples);
            }

            // Process audio frame for Opus encoding and WebRTC transmission
            // Use the persistent buffer directly (zero-copy optimization)
            AUDIO_LOG_DEBUG(L"[AudioCapturer] Processing audio frame: " << totalSamples << L" samples");
            ProcessAudioFrame(m_floatBuffer.data(), totalSamples, timestampUs);

            hr = m_pCaptureClient->ReleaseBuffer(numFramesAvailable);
            if (FAILED(hr))
            {
                LogErrorWithContext(hr, L"Failed to release buffer", m_consecutiveErrorCount);

                // Try enhanced recovery with exponential backoff
                if (ShouldRetryError(hr, m_consecutiveErrorCount)) {
                    m_consecutiveErrorCount++;

                    // Try comprehensive recovery
                    if (TryRecoverFromError(hr, L"ReleaseBuffer")) {
                        // Recovery successful, break to restart outer loop if needed
                        m_consecutiveErrorCount = 0; // Reset on success
                        break;
                    } else {
                        // Recovery failed, try exponential backoff
                        int delay = CalculateRetryDelay(m_consecutiveErrorCount);
                        AUDIO_LOG_DEBUG(L"[AudioCapturer] ReleaseBuffer recovery failed, backing off for " << delay << L"ms");
                        Sleep(delay);
                        continue;
                    }
                }

                // Check if system is under load and extend retry attempts
                if (IsSystemUnderLoad() && m_consecutiveErrorCount < m_retryConfig.maxRetries * 2) {
                    AUDIO_LOG_DEBUG(L"[AudioCapturer] System under load, extending ReleaseBuffer retry attempts");
                    int delay = CalculateRetryDelay(m_consecutiveErrorCount);
                    Sleep(delay);
                    m_consecutiveErrorCount++;
                    continue;
                }

                // Log error statistics before giving up
                LogErrorStats();

                // If we can't recover, exit
                AUDIO_LOG_ERROR(L"[AudioCapturer] Unable to recover from ReleaseBuffer error after "
                              << m_consecutiveErrorCount << L" attempts, terminating capture");
                goto Exit;
            }

            // Reset consecutive error count on successful operation
            m_consecutiveErrorCount = 0;

            hr = m_pCaptureClient->GetNextPacketSize(&numFramesAvailable);
            if (FAILED(hr))
            {
                LogErrorWithContext(hr, L"Failed to get next packet size after release", m_consecutiveErrorCount);

                // Try to recover from the error
                if (ShouldRetryError(hr, m_consecutiveErrorCount)) {
                    m_consecutiveErrorCount++;

                    if (hr == AUDCLNT_E_DEVICE_INVALIDATED) {
                        // Device was invalidated, try to reinitialize
                        if (ReinitializeAudioClient()) {
                            std::wcout << L"[AudioCapturer] Successfully recovered from device invalidation after ReleaseBuffer" << std::endl;
                            break; // Break out of the inner loop to restart the outer loop
                        }
                    } else {
                        // For other transient errors, just retry after a short delay
                        Sleep(100);
                        continue;
                    }
                }

                // If we can't recover, exit
                std::wcerr << L"[AudioCapturer] Unable to recover from GetNextPacketSize error after release, terminating capture" << std::endl;
                goto Exit;
            }

            // Reset consecutive error count on successful operation
            m_consecutiveErrorCount = 0;
        }

        // Periodic performance logging (every 5 seconds)
        uint64_t currentTime = GetTickCount64();
        if (currentTime - lastLogTime >= 5000) {
            double elapsedSeconds = (currentTime - lastLogTime) / 1000.0;
            double cyclesPerSecond = captureCycles / elapsedSeconds;
            double packetsPerSecond = dataPacketsProcessed / elapsedSeconds;

            if (m_currentlyUsingEventMode) {
                double timeoutRate = (timeoutCount * 100.0) / captureCycles;
                AUDIO_LOG_INFO(L"[AudioCapturer] Performance: " << cyclesPerSecond << L" cycles/sec, "
                             << packetsPerSecond << L" packets/sec, timeouts: " << timeoutRate << L"%");
            } else {
                AUDIO_LOG_INFO(L"[AudioCapturer] Performance: " << cyclesPerSecond << L" cycles/sec, "
                             << packetsPerSecond << L" packets/sec");
            }

            // Reset counters
            captureCycles = 0;
            dataPacketsProcessed = 0;
            timeoutCount = 0;
            lastLogTime = currentTime;
        }
    }

    // Cleanup and shutdown

Exit:
    std::wcout << L"[AudioCapturer] Audio capture stopped." << std::endl;

    // Clean up event handles
    if (m_hCaptureEvent) {
        CloseHandle(m_hCaptureEvent);
        m_hCaptureEvent = nullptr;
    }
    if (m_hStopEvent) {
        CloseHandle(m_hStopEvent);
        m_hStopEvent = nullptr;
    }

    // Clean up MMCSS registration
    if (m_hMmcssTask != nullptr) {
        BOOL mmcssReverted = AvRevertMmThreadCharacteristics(m_hMmcssTask);
        if (mmcssReverted) {
            std::wcout << L"[AudioCapturer] MMCSS registration cleaned up successfully." << std::endl;
        } else {
            std::wcerr << L"[AudioCapturer] Failed to clean up MMCSS registration: " << GetLastError() << std::endl;
        }
        m_hMmcssTask = nullptr;
        m_mmcssTaskIndex = 0;
    }

    if (pwfx) { CoTaskMemFree(pwfx); pwfx = nullptr; }
    if (pSessionControl2) pSessionControl2->Release();
    if (pSessionControl) pSessionControl->Release();
    if (pSessionEnumerator) pSessionEnumerator->Release();
    if (pSessionManager) pSessionManager->Release();
    if (pCollection) pCollection->Release();
    // ComPtr members auto-release

    CoUninitialize();
}

bool AudioCapturer::ConvertPCMToFloat(const BYTE* pcmData, UINT32 numFrames, void* format, std::vector<float>& floatData)
{
    if (!pcmData || !format || numFrames == 0) return false;

    // Cast to WAVEFORMATEX - this is safe since we know the type from the caller
    const WAVEFORMATEX* formatStruct = static_cast<const WAVEFORMATEX*>(format);
    const size_t totalSamples = static_cast<size_t>(numFrames) * static_cast<size_t>(formatStruct->nChannels);

    // Use pre-allocated buffer if possible to avoid reallocations
    if (totalSamples <= MAX_AUDIO_FRAME_SAMPLES * MAX_AUDIO_CHANNELS) {
        EnsureAudioBuffersCapacity();
        g_audioConversionBuffer.resize(totalSamples);
        floatData = g_audioConversionBuffer; // Reference to pre-allocated buffer
    } else {
        // Fallback to dynamic allocation for very large frames (rare)
        floatData.resize(totalSamples);
    }

    WORD tag = formatStruct->wFormatTag;
    WORD containerBits = formatStruct->wBitsPerSample;
    WORD validBits = containerBits;

    // Handle WAVE_FORMAT_EXTENSIBLE by inspecting SubFormat
    if (tag == WAVE_FORMAT_EXTENSIBLE && formatStruct->cbSize >= sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX)) {
        const WAVEFORMATEXTENSIBLE* ext = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(formatStruct);
        // Record valid bits if set for PCM
        if (ext->Samples.wValidBitsPerSample) {
            validBits = ext->Samples.wValidBitsPerSample;
        }

        if (IsEqualGUID(ext->SubFormat, KSDATAFORMAT_SUBTYPE_IEEE_FLOAT)) {
            tag = WAVE_FORMAT_IEEE_FLOAT;
        } else if (IsEqualGUID(ext->SubFormat, KSDATAFORMAT_SUBTYPE_PCM)) {
            tag = WAVE_FORMAT_PCM;
        }
    }

    if (tag == WAVE_FORMAT_IEEE_FLOAT) {
        if (containerBits == 32) {
            const float* pcmFloat = reinterpret_cast<const float*>(pcmData);
            std::copy(pcmFloat, pcmFloat + totalSamples, floatData.begin());
            return true;
        }
        return false;
    }

    if (tag == WAVE_FORMAT_PCM) {
        if (containerBits == 16) {
            const int16_t* pcm16 = reinterpret_cast<const int16_t*>(pcmData);
            for (size_t i = 0; i < totalSamples; ++i) {
                floatData[i] = static_cast<float>(pcm16[i]) / 32768.0f;
            }
            return true;
        } else if (containerBits == 24) {
            // 24-bit little-endian signed PCM (packed 3 bytes)
            const uint8_t* p = reinterpret_cast<const uint8_t*>(pcmData);
            const float denom = static_cast<float>(1u << (validBits - 1)); // 2^(validBits-1)
            for (size_t i = 0; i < totalSamples; ++i) {
                int32_t b0 = static_cast<int32_t>(p[i*3 + 0]);
                int32_t b1 = static_cast<int32_t>(p[i*3 + 1]) << 8;
                int32_t b2 = static_cast<int32_t>(p[i*3 + 2]) << 16;
                int32_t sample = b0 | b1 | b2;
                // Sign-extend 24-bit to 32-bit
                if (sample & 0x00800000) sample |= 0xFF000000;
                floatData[i] = static_cast<float>(sample) / denom;
            }
            return true;
        } else if (containerBits == 32) {
            const int32_t* pcm32 = reinterpret_cast<const int32_t*>(pcmData);
            const int shift = (validBits > 0 && validBits < 32) ? (32 - validBits) : 0;
            const float denom = (validBits > 0 && validBits <= 31)
                ? static_cast<float>(1u << (validBits - 1))
                : 2147483648.0f; // 2^31
            for (size_t i = 0; i < totalSamples; ++i) {
                int32_t raw = pcm32[i];
                int32_t sample = (shift > 0) ? (raw >> shift) : raw; // arithmetic shift keeps sign
                floatData[i] = static_cast<float>(sample) / denom;
            }
            return true;
        }
        return false;
    }

    return false;
}

bool AudioCapturer::ConvertPCMToFloatInPlace(const BYTE* pcmData, UINT32 numFrames, void* formatPtr, float* outputBuffer, size_t outputBufferSize)
{
    if (!pcmData || !formatPtr || numFrames == 0 || !outputBuffer) return false;

    // Cast to WAVEFORMATEX - this is safe since we know the type from the caller
    const WAVEFORMATEX* format = static_cast<const WAVEFORMATEX*>(formatPtr);
    const size_t totalSamples = static_cast<size_t>(numFrames) * static_cast<size_t>(format->nChannels);

    // Check if output buffer is large enough
    if (outputBufferSize < totalSamples) {
        std::wcerr << L"[AudioCapturer] Output buffer too small for PCM conversion: " << outputBufferSize << L" < " << totalSamples << std::endl;
        return false;
    }

    WORD tag = format->wFormatTag;
    WORD containerBits = format->wBitsPerSample;
    WORD validBits = containerBits;

    // Handle WAVE_FORMAT_EXTENSIBLE by inspecting SubFormat
    if (tag == WAVE_FORMAT_EXTENSIBLE && format->cbSize >= sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX)) {
        const WAVEFORMATEXTENSIBLE* ext = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(format);
        if (ext->Samples.wValidBitsPerSample) {
            validBits = ext->Samples.wValidBitsPerSample;
        }

        if (IsEqualGUID(ext->SubFormat, KSDATAFORMAT_SUBTYPE_IEEE_FLOAT)) {
            tag = WAVE_FORMAT_IEEE_FLOAT;
        } else if (IsEqualGUID(ext->SubFormat, KSDATAFORMAT_SUBTYPE_PCM)) {
            tag = WAVE_FORMAT_PCM;
        }
    }

    if (tag == WAVE_FORMAT_IEEE_FLOAT) {
        if (containerBits == 32) {
            const float* pcmFloat = reinterpret_cast<const float*>(pcmData);
            std::copy(pcmFloat, pcmFloat + totalSamples, outputBuffer);
            return true;
        }
        return false;
    } else if (tag == WAVE_FORMAT_PCM) {
        if (containerBits == 16) {
            const int16_t* pcm16 = reinterpret_cast<const int16_t*>(pcmData);
            for (size_t i = 0; i < totalSamples; ++i) {
                outputBuffer[i] = static_cast<float>(pcm16[i]) / 32768.0f; // 2^15
            }
            return true;
        } else if (containerBits == 24) {
            // 24-bit little-endian signed PCM (packed 3 bytes)
            const uint8_t* p = reinterpret_cast<const uint8_t*>(pcmData);
            const float denom = static_cast<float>(1u << (validBits - 1)); // 2^(validBits-1)
            for (size_t i = 0; i < totalSamples; ++i) {
                int32_t b0 = static_cast<int32_t>(p[i*3 + 0]);
                int32_t b1 = static_cast<int32_t>(p[i*3 + 1]) << 8;
                int32_t b2 = static_cast<int32_t>(p[i*3 + 2]) << 16;
                int32_t sample = b0 | b1 | b2;
                if (sample & 0x00800000) sample |= 0xFF000000;
                outputBuffer[i] = static_cast<float>(sample) / denom;
            }
            return true;
        } else if (containerBits == 32) {
            const int32_t* pcm32 = reinterpret_cast<const int32_t*>(pcmData);
            const int shift = (validBits > 0 && validBits < 32) ? (32 - validBits) : 0;
            const float denom = (validBits > 0 && validBits <= 31)
                ? static_cast<float>(1u << (validBits - 1))
                : 2147483648.0f; // 2^31
            for (size_t i = 0; i < totalSamples; ++i) {
                int32_t raw = pcm32[i];
                int32_t sample = (shift > 0) ? (raw >> shift) : raw;
                outputBuffer[i] = static_cast<float>(sample) / denom;
            }
            return true;
        }
        return false;
    }

    return false;
}

void AudioCapturer::ProcessAudioFrame(const float* samples, size_t sampleCount, int64_t timestampUs)
{
    // Calculate RMS for raw input samples (every 5 seconds)
    float rawRms = 0.0f;
    for (size_t i = 0; i < sampleCount; ++i) {
        rawRms += samples[i] * samples[i];
    }
    rawRms = sqrtf(rawRms / sampleCount);

    static auto lastRawLog = std::chrono::steady_clock::now();
    auto now = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - lastRawLog).count();

    if (elapsed >= 5000) {  // 5 seconds
        std::cout << "[AUDIO_DEBUG] Raw audio RMS: " << rawRms << std::endl;
        lastRawLog = now;
    }
    if (!samples || sampleCount == 0) return;

    // Zero-copy audio processing pipeline - direct frame processing without accumulation

    // If this is the first sample of a new frame, store the timestamp
    if (m_currentFrameSamples == 0) {
        m_currentFrameTimestamp = timestampUs;
    }

    // Ensure current frame buffer is properly sized
    if (m_currentFrameBuffer.size() < m_samplesPerFrame) {
        m_currentFrameBuffer.resize(m_samplesPerFrame);
    }

    // Calculate how many samples we need to complete the current frame
    size_t samplesNeeded = m_samplesPerFrame - m_currentFrameSamples;
    size_t samplesToCopy = (sampleCount < samplesNeeded) ? sampleCount : samplesNeeded;

    // Copy samples directly into the current frame buffer
    if (m_currentFrameSamples + samplesToCopy <= m_currentFrameBuffer.size()) {
        std::copy(samples, samples + samplesToCopy,
                 m_currentFrameBuffer.begin() + m_currentFrameSamples);
    }
    m_currentFrameSamples += samplesToCopy;

    // If we have a complete frame, push it to the ring buffer
    if (m_currentFrameSamples >= m_samplesPerFrame) {
        AUDIO_LOG_INFO(L"[AudioCapturer] PUSHING FRAME TO RING BUFFER - Samples: " << m_currentFrameSamples);

        // Push the complete frame to ring buffer (zero-copy from working buffer)
        if (PushFrameToRingBuffer(m_currentFrameBuffer, m_currentFrameTimestamp)) {
            // Frame successfully queued to ring buffer
            AUDIO_LOG_INFO(L"[AudioCapturer] Frame successfully pushed to ring buffer");
            // DEBUG: Log successful frame capture
            static uint64_t frameCounter = 0;
            frameCounter++;
            if (frameCounter % 10 == 0) { // Log every 10th frame
                std::wcout << L"[AudioCapturer] DEBUG: Successfully captured and queued frame #" << frameCounter
                           << L" (size: " << m_currentFrameBuffer.size() << L" samples)" << std::endl;
            }
            // Encoder thread polls continuously, so no explicit wake-up needed
        } else {
            std::wcerr << L"[AudioCapturer] Failed to push frame to ring buffer - encoder congestion" << std::endl;
        }

        // Reset for next frame
        m_currentFrameSamples = 0;
        m_currentFrameTimestamp = 0;

        // Handle any remaining samples that didn't fit in the current frame
        if (samplesToCopy < sampleCount) {
            size_t remainingSamples = sampleCount - samplesToCopy;
            // Advance timestamp by the duration of the consumed samples from this chunk
            uint32_t channels = (m_frameSizeSamples > 0) ? static_cast<uint32_t>(m_samplesPerFrame / m_frameSizeSamples) : s_audioConfig.channels;
            if (channels == 0) {
                channels = 2; // conservative fallback to stereo
            }
            double secondsAdvanced = static_cast<double>(samplesToCopy) / (48000.0 * static_cast<double>(channels));
            int64_t deltaUs = static_cast<int64_t>(secondsAdvanced * 1000000.0 + 0.5);
            ProcessAudioFrame(samples + samplesToCopy, remainingSamples, timestampUs + deltaUs);
        }
    }
}





bool AudioCapturer::ResampleTo48kInPlaceConstrained(std::vector<float>& buffer, size_t inFrames, uint32_t inRate, uint32_t channels, size_t maxBufferSize)
{
    // ============================================================================
    // CONSTRAINED RESAMPLING (LINEAR INTERPOLATION, BUFFER-BOUNDED)
    // ============================================================================
    // Performs linear interpolation into a temporary buffer sized to the
    // expected output. Ensures we never exceed maxBufferSize.

    if (inRate == 48000) {
        return true; // No resampling needed
    }

    const double ratio = 48000.0 / static_cast<double>(inRate);
    const size_t outFrames = static_cast<size_t>(std::ceil(static_cast<double>(inFrames) * ratio));
    const size_t outputSamples = outFrames * static_cast<size_t>(channels);

    if (outputSamples > maxBufferSize) {
        std::wcerr << L"[AudioResample] Output size (" << outputSamples << ") exceeds buffer limit (" << maxBufferSize << ")" << std::endl;
        return false;
    }

    // Prepare temp buffer
    EnsureAudioBuffersCapacity();
    g_audioTempBuffer.resize(outputSamples);
    std::vector<float>& temp = g_audioTempBuffer;

    // Linear interpolation
    for (size_t o = 0; o < outFrames; ++o) {
        const double srcPos = static_cast<double>(o) / ratio;
        size_t srcFrame = static_cast<size_t>(srcPos);
        double frac = srcPos - static_cast<double>(srcFrame);
        if (srcFrame >= inFrames) {
            srcFrame = inFrames - 1;
            frac = 0.0;
        }

        for (uint32_t ch = 0; ch < channels; ++ch) {
            const size_t dstIdx = o * channels + ch;
            const size_t srcIdx1 = srcFrame * channels + ch;
            const size_t srcIdx2 = (srcFrame + 1 < inFrames ? (srcFrame + 1) : srcFrame) * channels + ch;

            const float s1 = (srcIdx1 < buffer.size()) ? buffer[srcIdx1] : 0.0f;
            const float s2 = (srcIdx2 < buffer.size()) ? buffer[srcIdx2] : s1;
            temp[dstIdx] = s1 + static_cast<float>(frac) * (s2 - s1);
        }
    }

    // Copy back into original buffer and resize
    if (buffer.size() < outputSamples) buffer.resize(outputSamples);
    std::copy(temp.begin(), temp.begin() + outputSamples, buffer.begin());
    buffer.resize(outputSamples);

    return true;
}

void AudioCapturer::ResampleTo48kInPlace(std::vector<float>& buffer, size_t inFrames, uint32_t inRate, uint32_t channels)
{
    // ============================================================================
    // ZERO-ALLOCATION IN-PLACE RESAMPLING
    // ============================================================================
    // This function resamples directly in the provided buffer, eliminating
    // temporary vector allocations and copies for maximum performance.

    if (inRate == 48000) {
        // No resampling needed - buffer is already at target rate
        return;
    }

    // Choose resampler priority based on configuration
    bool preferLinear = s_audioConfig.wasapi.preferLinearResampling;
    bool useDmoOnlyForHighQuality = s_audioConfig.wasapi.useDmoOnlyForHighQuality;

    // Prefer high-quality DMO for 44.1 -> 48 kHz to avoid artifacts
    if (preferLinear && !useDmoOnlyForHighQuality && inRate != 44100) {
        // Prefer linear interpolation - use it directly
        std::wcout << L"[AudioCapturer] Using linear interpolation resampler (preferred by config)" << std::endl;
    } else {
        // Try DMO first (high-quality mode or DMO preferred)
        if (InitializeDMOResampler(inRate, channels)) {
            // Use DMO resampler directly with the buffer
            if (ProcessResamplerDMOInPlace(buffer)) {
                // Success - DMO resampler handled the conversion in-place
                std::wcout << L"[AudioCapturer] Using DMO resampler (high-quality)" << std::endl;
                return;
            } else {
                std::wcerr << L"[AudioCapturer] DMO in-place resampler failed, falling back to linear interpolation" << std::endl;
            }
        }

        if (useDmoOnlyForHighQuality) {
            std::wcerr << L"[AudioCapturer] DMO required but unavailable, falling back to linear interpolation" << std::endl;
        }
    }

    // Use linear interpolation method with in-place operation
    std::wcout << L"[AudioCapturer] Using in-place linear interpolation resampler" << std::endl;

    double ratio = 48000.0 / static_cast<double>(inRate);
    size_t outFrames = static_cast<size_t>(std::ceil((inFrames) * ratio));
    size_t inputSamples = inFrames * channels;
    size_t outputSamples = outFrames * channels;

    // Ensure buffer has enough space for output
    if (buffer.size() < outputSamples) {
        buffer.resize(outputSamples);
    }

    // For continuity across calls when rates remain the same
    if (m_lastInputRate != inRate) {
        m_resamplePhase = 0.0;
        m_resampleRemainder.assign(channels, 0.0f);
        m_lastInputRate = inRate;
    }

    // Use pre-allocated buffer to avoid heap allocations
    EnsureAudioBuffersCapacity();

    // Perform in-place resampling using pre-allocated temporary buffer
    g_audioTempBuffer.resize(outputSamples);
    std::vector<float>& tempBuffer = g_audioTempBuffer;
    size_t inIndex = 0; // frame index

    for (size_t o = 0; o < outFrames; ++o) {
        double srcPos = (o / ratio);
        size_t srcFrame = static_cast<size_t>(srcPos);
        double frac = srcPos - srcFrame;

        // Handle boundary conditions
        if (srcFrame >= inFrames - 1) {
            // Use last frame for extrapolation
            for (uint32_t ch = 0; ch < channels; ++ch) {
                size_t srcIdx = (inFrames - 1) * channels + ch;
                size_t dstIdx = o * channels + ch;
                if (srcIdx < buffer.size() && dstIdx < tempBuffer.size()) {
                    tempBuffer[dstIdx] = buffer[srcIdx];
                }
            }
        } else {
            // Linear interpolation between adjacent frames (channel-aware)
            for (uint32_t ch = 0; ch < channels; ++ch) {
                size_t srcIdx0 = srcFrame * channels + ch;
                size_t srcIdx1 = (srcFrame + 1 < inFrames ? (srcFrame + 1) : srcFrame) * channels + ch;
                size_t dstIdx = o * channels + ch;

                if (srcIdx0 < buffer.size() && srcIdx1 < buffer.size() && dstIdx < tempBuffer.size()) {
                    float s0 = buffer[srcIdx0];
                    float s1 = buffer[srcIdx1];
                    tempBuffer[dstIdx] = s0 + static_cast<float>(frac) * (s1 - s0);
                }
            }
        }
    }

    // Save last input sample as remainder for continuity
    if (inFrames > 0) {
        m_resampleRemainder.resize(channels);
        for (uint32_t ch = 0; ch < channels; ++ch) {
            size_t lastSampleIdx = (inFrames - 1) * channels + ch;
            if (lastSampleIdx < buffer.size()) {
                m_resampleRemainder[ch] = buffer[lastSampleIdx];
            }
        }
    }

    // Copy back into original buffer and resize (preserve temp buffer capacity)
    std::copy(tempBuffer.begin(), tempBuffer.begin() + outputSamples, buffer.begin());
    buffer.resize(outputSamples);
}

// ============================================================================
// ERROR RECOVERY AND ROBUSTNESS IMPLEMENTATION
// ============================================================================

void AudioCapturer::LogErrorWithContext(HRESULT hr, const std::wstring& context, int retryCount)
{
    std::wostringstream ss;
    ss << L"[AudioCapturer] " << context;

    if (retryCount > 0) {
        ss << L" (retry " << retryCount << L")";
    }

    ss << L": " << _com_error(hr).ErrorMessage()
       << L" (HRESULT: 0x" << std::hex << hr << std::dec << L")";

    std::wcerr << ss.str() << std::endl;
}

bool AudioCapturer::ShouldRetryError(HRESULT hr, int retryCount)
{
    // Maximum retry attempts to prevent infinite loops
    const int MAX_RETRY_ATTEMPTS = 3;

    if (retryCount >= MAX_RETRY_ATTEMPTS) {
        return false;
    }

    // Retry for specific transient errors
    switch (hr) {
        case AUDCLNT_E_DEVICE_INVALIDATED:
            // Device was invalidated - requires reinitialization
            return true;

        case AUDCLNT_E_BUFFER_ERROR:
            // Buffer operation error - might be transient
            return true;

        case E_POINTER:
            // Null pointer error - might be transient COM issue
            return true;

        case HRESULT_FROM_WIN32(ERROR_TIMEOUT):
            // Timeout error - might be transient
            return true;

        default:
            // Don't retry for other errors
            return false;
    }
}

bool AudioCapturer::ReinitializeAudioClient()
{
    std::wcout << L"[AudioCapturer] Attempting to reinitialize audio client (attempt " << m_deviceReinitCount + 1 << L")" << std::endl;

    // Limit the number of reinitialization attempts
    const int MAX_REINIT_ATTEMPTS = 3;
    if (m_deviceReinitCount >= MAX_REINIT_ATTEMPTS) {
        std::wcerr << L"[AudioCapturer] Maximum reinitialization attempts reached, giving up" << std::endl;
        return false;
    }

    m_deviceReinitCount++;

    // Clean up existing resources
    if (m_pAudioClient) {
        m_pAudioClient->Stop();
        m_pAudioClient.Reset();
    }

    if (m_pCaptureClient) {
        m_pCaptureClient.Reset();
    }

    // Clean up DMO resampler
    CleanupDMOResampler();

    // Close event handles
    if (m_hCaptureEvent) {
        CloseHandle(m_hCaptureEvent);
        m_hCaptureEvent = nullptr;
    }

    // Wait a moment for the system to stabilize
    Sleep(500);

    HRESULT hr = S_OK;

    // Try to find the device again if reference is lost
    if (!m_pDevice) {
        std::wcout << L"[AudioCapturer] Device reference lost, attempting to rediscover device..." << std::endl;

        // Clean up existing COM objects
        if (m_pSessionControl2) {
            m_pSessionControl2.Reset();
        }

        // Re-enumerate devices to find the target process
        const CLSID CLSID_MMDeviceEnumerator = __uuidof(MMDeviceEnumerator);
        const IID IID_IMMDeviceEnumerator = __uuidof(IMMDeviceEnumerator);

        hr = CoCreateInstance(
            CLSID_MMDeviceEnumerator, NULL,
            CLSCTX_ALL, IID_IMMDeviceEnumerator,
            reinterpret_cast<void**>(m_pEnumerator.ReleaseAndGetAddressOf()));

        if (FAILED(hr)) {
            LogErrorWithContext(hr, L"Failed to recreate device enumerator during reinitialization", 0);
            return false;
        }

        // Enumerate render endpoints
        IMMDeviceCollection* pCollection = NULL;
        hr = m_pEnumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &pCollection);
        if (FAILED(hr)) {
            LogErrorWithContext(hr, L"Failed to enumerate audio endpoints during reinitialization", 0);
            return false;
        }

        UINT deviceCount;
        hr = pCollection->GetCount(&deviceCount);
        if (FAILED(hr)) {
            LogErrorWithContext(hr, L"Failed to get device count during reinitialization", 0);
            pCollection->Release();
            return false;
        }

        // Look for the target process session
        bool foundSession = false;
        for (UINT i = 0; i < deviceCount && !foundSession; ++i) {
            IMMDevice* pDevice = NULL;
            hr = pCollection->Item(i, &pDevice);
            if (FAILED(hr)) {
                continue;
            }

            // Check if this device has our target process session
            IAudioSessionManager2* pSessionManager = NULL;
            hr = pDevice->Activate(__uuidof(IAudioSessionManager2), CLSCTX_ALL, NULL, (void**)&pSessionManager);
            if (SUCCEEDED(hr)) {
                IAudioSessionEnumerator* pSessionEnumerator = NULL;
                hr = pSessionManager->GetSessionEnumerator(&pSessionEnumerator);
                if (SUCCEEDED(hr)) {
                    int sessionCount;
                    hr = pSessionEnumerator->GetCount(&sessionCount);
                    if (SUCCEEDED(hr)) {
                        for (int j = 0; j < sessionCount && !foundSession; ++j) {
                            IAudioSessionControl* pSessionControl = NULL;
                            hr = pSessionEnumerator->GetSession(j, &pSessionControl);
                            if (SUCCEEDED(hr)) {
                                IAudioSessionControl2* pSessionControl2 = NULL;
                                hr = pSessionControl->QueryInterface(__uuidof(IAudioSessionControl2), (void**)&pSessionControl2);
                                if (SUCCEEDED(hr)) {
                                    DWORD currentProcessId = 0;
                                    hr = pSessionControl2->GetProcessId(&currentProcessId);
                                    if (SUCCEEDED(hr) && currentProcessId == m_targetProcessId) {
                                        // Found our target session
                                        std::wcout << L"[AudioCapturer] Rediscovered target process session on device " << i << std::endl;
                                        m_pDevice = pDevice; // ComPtr takes ownership
                                        m_pSessionControl2 = pSessionControl2; // ComPtr takes ownership
                                        foundSession = true;
                                    } else {
                                        pSessionControl2->Release();
                                    }
                                }
                                pSessionControl->Release();
                            }
                        }
                    }
                    pSessionEnumerator->Release();
                }
                pSessionManager->Release();
            }

            if (!foundSession) {
                pDevice->Release();
            }
        }

        pCollection->Release();

        if (!foundSession) {
            std::wcerr << L"[AudioCapturer] Could not rediscover target process session during reinitialization" << std::endl;
            return false;
        }
    }

    // Reinitialize the audio client
    hr = m_pDevice->Activate(__uuidof(IAudioClient), CLSCTX_ALL, NULL,
                           reinterpret_cast<void**>(m_pAudioClient.ReleaseAndGetAddressOf()));
    if (FAILED(hr)) {
        LogErrorWithContext(hr, L"Failed to reactivate audio client during reinitialization", 0);
        return false;
    }

    // Get the mix format again
    WAVEFORMATEX* pwfx = nullptr;
    hr = m_pAudioClient->GetMixFormat(&pwfx);
    if (FAILED(hr)) {
        LogErrorWithContext(hr, L"Failed to get mix format during reinitialization", 0);
        return false;
    }

    // Try to initialize with the same parameters
    DWORD streamFlags = AUDCLNT_STREAMFLAGS_LOOPBACK;
    hr = m_pAudioClient->Initialize(AUDCLNT_SHAREMODE_SHARED, streamFlags,
                                  m_hnsRequestedDuration, 0, pwfx, NULL);
    if (FAILED(hr)) {
        LogErrorWithContext(hr, L"Failed to reinitialize audio client", 0);
        CoTaskMemFree(pwfx);
        return false;
    }

    // Get buffer size
    UINT32 bufferFrameCount;
    hr = m_pAudioClient->GetBufferSize(&bufferFrameCount);
    if (FAILED(hr)) {
        LogErrorWithContext(hr, L"Failed to get buffer size during reinitialization", 0);
        CoTaskMemFree(pwfx);
        return false;
    }

    // Get capture client
    hr = m_pAudioClient->GetService(__uuidof(IAudioCaptureClient),
                                  reinterpret_cast<void**>(m_pCaptureClient.ReleaseAndGetAddressOf()));
    if (FAILED(hr)) {
        LogErrorWithContext(hr, L"Failed to get capture client during reinitialization", 0);
        CoTaskMemFree(pwfx);
        return false;
    }

    // Start the audio client
    hr = m_pAudioClient->Start();
    if (FAILED(hr)) {
        LogErrorWithContext(hr, L"Failed to restart audio client", 0);
        CoTaskMemFree(pwfx);
        return false;
    }

    CoTaskMemFree(pwfx);

    // Reset error counters on successful reinitialization
    m_consecutiveErrorCount = 0;

    std::wcout << L"[AudioCapturer] Successfully reinitialized audio client" << std::endl;
    return true;
}

// ============================================================================
// ENHANCED ERROR HANDLING AND RECOVERY
// ============================================================================

int AudioCapturer::CalculateRetryDelay(int retryCount)
{
    // Exponential backoff with jitter to prevent thundering herd
    int delay = static_cast<int>(m_retryConfig.baseDelayMs * pow(m_retryConfig.backoffMultiplier, retryCount));

    // Cap at maximum delay
    if (delay > m_retryConfig.maxDelayMs) {
        delay = m_retryConfig.maxDelayMs;
    }

    // Add small random jitter (±25%) to prevent synchronized retries
    int jitter = (rand() % (delay / 2)) - (delay / 4);
    delay += jitter;

    return (std::max)(1, delay); // Ensure minimum 1ms delay
}

bool AudioCapturer::TryRecoverFromError(HRESULT hr, const std::wstring& operation)
{
    m_errorStats.totalErrors++;
    m_errorStats.lastErrorCode = hr;
    m_errorStats.lastErrorTime = GetTickCount64();

    // Categorize the error for statistics
    if (hr == AUDCLNT_E_DEVICE_INVALIDATED) {
        m_errorStats.deviceInvalidationErrors++;
    } else if (hr == AUDCLNT_E_BUFFER_ERROR) {
        m_errorStats.bufferErrors++;
    } else if (hr == HRESULT_FROM_WIN32(ERROR_TIMEOUT)) {
        m_errorStats.timeoutErrors++;
    } else if (hr == E_POINTER) {
        m_errorStats.pointerErrors++;
    } else {
        m_errorStats.otherErrors++;
    }

    // Try recovery strategies in order of preference
    if (hr == AUDCLNT_E_DEVICE_INVALIDATED) {
        AUDIO_LOG_INFO(L"[AudioCapturer] Device invalidated, attempting recovery...");

        // Try device reinitialization first
        if (ReinitializeAudioClient()) {
            m_errorStats.successfulRecoveries++;
            AUDIO_LOG_INFO(L"[AudioCapturer] Successfully recovered from device invalidation");
            return true;
        }

        // If reinitialization fails and fallback is enabled, try mode switching
        if (m_retryConfig.enableModeFallback) {
            if (m_currentlyUsingEventMode) {
                AUDIO_LOG_INFO(L"[AudioCapturer] Switching to polling mode as fallback");
                SwitchToPollingMode();
                m_errorStats.modeFallbacks++;
                return true;
            } else {
                AUDIO_LOG_INFO(L"[AudioCapturer] Already in polling mode, trying event mode");
                SwitchToEventMode();
                m_errorStats.modeFallbacks++;
                return true;
            }
        }
    } else if (hr == AUDCLNT_E_BUFFER_ERROR || hr == HRESULT_FROM_WIN32(ERROR_TIMEOUT)) {
        // For buffer/timeout errors, try a quick mode switch
        if (m_retryConfig.enableModeFallback) {
            if (m_currentlyUsingEventMode) {
                AUDIO_LOG_DEBUG(L"[AudioCapturer] Buffer error in event mode, switching to polling");
                SwitchToPollingMode();
                m_errorStats.modeFallbacks++;
                return true;
            }
        }
    }

    return false; // Recovery failed
}

void AudioCapturer::SwitchToPollingMode()
{
    if (!m_currentlyUsingEventMode) {
        return; // Already in polling mode
    }

    AUDIO_LOG_INFO(L"[AudioCapturer] Switching from event-driven to polling mode");

    // Clean up event handles
    if (m_hCaptureEvent) {
        CloseHandle(m_hCaptureEvent);
        m_hCaptureEvent = nullptr;
    }

    m_currentlyUsingEventMode = false;
    m_errorStats.modeFallbacks++;
}

void AudioCapturer::SwitchToEventMode()
{
    if (m_currentlyUsingEventMode) {
        return; // Already in event mode
    }

    AUDIO_LOG_INFO(L"[AudioCapturer] Attempting to switch from polling to event-driven mode");

    // Recreate event handle if it was closed during polling mode
    if (!m_hCaptureEvent) {
        m_hCaptureEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
        if (!m_hCaptureEvent) {
            AUDIO_LOG_ERROR(L"[AudioCapturer] Failed to create capture event for mode switch");
            return;
        }
    }

    // Try to set up event-driven mode
    if (m_pAudioClient) {
        HRESULT hr = m_pAudioClient->SetEventHandle(m_hCaptureEvent);
        if (SUCCEEDED(hr)) {
            m_currentlyUsingEventMode = true;
            AUDIO_LOG_INFO(L"[AudioCapturer] Successfully switched to event-driven mode");
            m_errorStats.modeFallbacks++;
        } else {
            AUDIO_LOG_ERROR(L"[AudioCapturer] Failed to switch to event mode: " << _com_error(hr).ErrorMessage());
            // Clean up the event handle if setting it failed
            CloseHandle(m_hCaptureEvent);
            m_hCaptureEvent = nullptr;
        }
    }
}

void AudioCapturer::LogErrorStats()
{
    if (!s_enableBufferMonitoring) {
        return;
    }

    uint64_t currentTime = GetTickCount64();
    if (currentTime - m_errorStats.lastErrorTime > 60000) { // Log every minute if there were errors
        AUDIO_LOG_INFO(L"[AudioCapturer] Error Stats - Total: " << m_errorStats.totalErrors
                      << L", Device Invalidations: " << m_errorStats.deviceInvalidationErrors
                      << L", Buffer Errors: " << m_errorStats.bufferErrors
                      << L", Timeouts: " << m_errorStats.timeoutErrors
                      << L", Recoveries: " << m_errorStats.successfulRecoveries
                      << L", Mode Switches: " << m_errorStats.modeFallbacks);
    }
}

bool AudioCapturer::IsSystemUnderLoad()
{
    // Simple heuristic: check if we're getting frequent errors
    uint64_t currentTime = GetTickCount64();
    uint64_t timeSinceLastError = currentTime - m_errorStats.lastErrorTime;

    // Consider system under load if errors are frequent (< 30 seconds apart)
    return (m_errorStats.totalErrors > 0 && timeSinceLastError < 30000);
}

// ============================================================================
// RING BUFFER MANAGEMENT - Zero-Copy Audio Pipeline
// ============================================================================

void AudioCapturer::InitializeRingBuffer()
{
    // Preallocate ring buffer frames (each frame matches encoder requirements exactly)
    m_frameRingBuffer.resize(RING_BUFFER_SIZE);
    for (auto& frame : m_frameRingBuffer) {
        frame.resize(m_samplesPerFrame);
        std::fill(frame.begin(), frame.end(), 0.0f); // Initialize to silence
    }

    // Initialize timestamp array
    m_frameTimestamps.resize(RING_BUFFER_SIZE);
    std::fill(m_frameTimestamps.begin(), m_frameTimestamps.end(), 0);

    // Reset ring buffer indices
    m_ringBufferWriteIndex = 0;
    m_ringBufferReadIndex = 0;
    m_ringBufferCount = 0;

    std::wcout << L"[AudioCapturer] Initialized ring buffer with " << RING_BUFFER_SIZE
              << L" frames of " << m_samplesPerFrame << L" samples each" << std::endl;
}

bool AudioCapturer::PushFrameToRingBuffer(const std::vector<float>& frame, int64_t timestamp)
{
    AUDIO_LOG_INFO(L"[RingBuffer] PushFrameToRingBuffer called - Frame size: " << frame.size() << L", Timestamp: " << timestamp);

    if (IsRingBufferFull()) {
        AUDIO_LOG_INFO(L"[RingBuffer] Ring buffer is FULL - Count: " << m_ringBufferCount << L"/" << RING_BUFFER_SIZE);
        std::wcerr << L"[AudioCapturer] Ring buffer full - dropping frame (encoder congestion)" << std::endl;
        return false;
    }

    AUDIO_LOG_INFO(L"[RingBuffer] Pushing frame to index " << m_ringBufferWriteIndex);

    // Copy frame data directly into preallocated ring buffer slot
    auto& ringBufferFrame = m_frameRingBuffer[m_ringBufferWriteIndex];
    if (frame.size() <= ringBufferFrame.size()) {
        std::copy(frame.begin(), frame.end(), ringBufferFrame.begin());
        AUDIO_LOG_INFO(L"[RingBuffer] Frame data copied successfully");
    } else {
        AUDIO_LOG_INFO(L"[RingBuffer] Frame size mismatch - Frame: " << frame.size() << L", Buffer: " << ringBufferFrame.size());
        std::wcerr << L"[AudioCapturer] Frame size mismatch in ring buffer push" << std::endl;
        return false;
    }

    // Store timestamp in separate array (no audio corruption!)
    m_frameTimestamps[m_ringBufferWriteIndex] = timestamp;
    AUDIO_LOG_INFO(L"[RingBuffer] Timestamp stored: " << timestamp);

    // Update ring buffer indices
    size_t oldWriteIndex = m_ringBufferWriteIndex;
    m_ringBufferWriteIndex = (m_ringBufferWriteIndex + 1) % RING_BUFFER_SIZE;
    m_ringBufferCount++;

    AUDIO_LOG_INFO(L"[RingBuffer] Indices updated - Old write: " << oldWriteIndex << L", New write: " << m_ringBufferWriteIndex << L", Count: " << m_ringBufferCount);

    return true;
}

bool AudioCapturer::PopFrameFromRingBuffer(std::vector<float>& frame, int64_t& timestamp)
{
    AUDIO_LOG_INFO(L"[RingBuffer] PopFrameFromRingBuffer called");

    if (IsRingBufferEmpty()) {
        AUDIO_LOG_INFO(L"[RingBuffer] Ring buffer is EMPTY");
        return false;
    }

    AUDIO_LOG_INFO(L"[RingBuffer] Popping frame from index " << m_ringBufferReadIndex << L", Count: " << m_ringBufferCount);

    // Get frame from ring buffer (audio data is intact!)
    const auto& ringBufferFrame = m_frameRingBuffer[m_ringBufferReadIndex];
    frame = ringBufferFrame; // Direct copy - no corruption!
    AUDIO_LOG_INFO(L"[RingBuffer] Frame copied - Size: " << frame.size() << L" samples");

    // Get timestamp from separate array
    timestamp = m_frameTimestamps[m_ringBufferReadIndex];
    AUDIO_LOG_INFO(L"[RingBuffer] Timestamp retrieved: " << timestamp);

    // Update ring buffer indices
    size_t oldReadIndex = m_ringBufferReadIndex;
    m_ringBufferReadIndex = (m_ringBufferReadIndex + 1) % RING_BUFFER_SIZE;
    m_ringBufferCount--;

    AUDIO_LOG_INFO(L"[RingBuffer] Indices updated - Old read: " << oldReadIndex << L", New read: " << m_ringBufferReadIndex << L", Count: " << m_ringBufferCount);

    return true;
}

bool AudioCapturer::IsRingBufferEmpty() const
{
    return m_ringBufferCount == 0;
}

bool AudioCapturer::IsRingBufferFull() const
{
    return m_ringBufferCount >= RING_BUFFER_SIZE;
}

size_t AudioCapturer::GetRingBufferCount() const
{
    return m_ringBufferCount;
}

void AudioCapturer::ResampleTo48k(const float* in, size_t inFrames, uint32_t inRate, uint32_t channels, std::vector<float>& out)
{
    // ============================================================================
    // HIGH-QUALITY RESAMPLING WITH DMO FALLBACK
    // ============================================================================
    // This function now uses Windows Audio Resampler DMO for superior quality
    // compared to linear interpolation, which introduces aliasing and HF loss.
    // Optimized to minimize buffer allocations and copies.

    if (inRate == 0 || channels == 0) { out.clear(); return; }
    if (inRate == 48000) {
        // No resampling needed - copy directly to output buffer
        size_t totalSamples = inFrames * channels;
        out.resize(totalSamples);
        std::copy(in, in + totalSamples, out.begin());
        return;
    }

    // Choose resampler priority based on configuration
    bool preferLinear = s_audioConfig.wasapi.preferLinearResampling;
    bool useDmoOnlyForHighQuality = s_audioConfig.wasapi.useDmoOnlyForHighQuality;

    // Prefer high-quality DMO for 44.1 -> 48 kHz to avoid artifacts
    if (preferLinear && !useDmoOnlyForHighQuality && inRate != 44100) {
        // Prefer linear interpolation - skip DMO
        std::wcout << L"[AudioCapturer] Using linear interpolation resampler (preferred by config)" << std::endl;
    } else {
        // Try DMO first (high-quality mode or DMO preferred)
        if (InitializeDMOResampler(inRate, channels)) {
            size_t inputSamples = inFrames * channels;
            if (ProcessResamplerDMO(in, inputSamples, out)) {
                // Success - DMO resampler handled the conversion
                std::wcout << L"[AudioCapturer] Using DMO resampler (high-quality)" << std::endl;
                return;
            } else {
                std::wcerr << L"[AudioCapturer] DMO resampler failed, falling back to linear interpolation" << std::endl;
            }
        }

        if (useDmoOnlyForHighQuality) {
            std::wcerr << L"[AudioCapturer] DMO required but unavailable, falling back to linear interpolation" << std::endl;
        }
    }

    // Use linear interpolation method
    std::wcout << L"[AudioCapturer] Using linear interpolation resampler (DMO unavailable)" << std::endl;

    double ratio = 48000.0 / static_cast<double>(inRate);
    size_t outFrames = static_cast<size_t>(std::ceil((inFrames) * ratio));
    out.resize(outFrames * channels);

    // For continuity across calls when rates remain the same
    if (m_lastInputRate != inRate) {
        m_resamplePhase = 0.0;
        m_resampleRemainder.assign(channels, 0.0f);
        m_lastInputRate = inRate;
    }

    // Start with previous remainder sample for interpolation continuity
    // Use pre-allocated buffer to avoid heap allocations
    EnsureAudioBuffersCapacity();
    g_audioResampleBuffer.resize(channels);
    std::vector<float>& prev = g_audioResampleBuffer;
    std::fill(prev.begin(), prev.end(), 0.0f);
    if (!m_resampleRemainder.empty()) {
        prev = m_resampleRemainder;
    }

    size_t inIndex = 0; // frame index
    for (size_t o = 0; o < outFrames; ++o) {
        double srcPos = (o / ratio);
        size_t i0 = static_cast<size_t>(srcPos);
        double frac = srcPos - static_cast<double>(i0);

        for (uint32_t ch = 0; ch < channels; ++ch) {
            float s0, s1;
            if (i0 == 0) {
                s0 = prev[ch];
            } else {
                s0 = in[(i0 - 1) * channels + ch];
            }
            if (i0 < inFrames) {
                s1 = in[i0 * channels + ch];
            } else {
                s1 = in[(inFrames - 1) * channels + ch];
            }
            out[o * channels + ch] = s0 + static_cast<float>(frac) * (s1 - s0);
        }
    }

    // Save last input sample as remainder for continuity
    if (inFrames > 0) {
        m_resampleRemainder.resize(channels);
        for (uint32_t ch = 0; ch < channels; ++ch) {
            m_resampleRemainder[ch] = in[(inFrames - 1) * channels + ch];
        }
    }
}

// Static audio configuration instance
AudioCapturer::AudioConfig AudioCapturer::s_audioConfig;

// ============================================================================
// SHARED REFERENCE CLOCK - AV Synchronization
// Ensures audio and video use the same reference time to prevent drift
// ============================================================================

void AudioCapturer::InitializeSharedReferenceClock() {
    bool expected = false;
    if (s_sharedReferenceInitialized.compare_exchange_strong(expected, true)) {
        // Only initialize once across all instances
        s_sharedReferenceTime = std::chrono::steady_clock::now();
        auto nowUs = std::chrono::duration_cast<std::chrono::microseconds>(
            s_sharedReferenceTime.time_since_epoch()).count();
        std::wcout << L"[AV-Sync] Shared reference clock initialized at " << nowUs << L" us" << std::endl;
    }
}

int64_t AudioCapturer::GetSharedReferenceTimeUs() {
    if (!s_sharedReferenceInitialized.load()) {
        InitializeSharedReferenceClock();
    }

    auto now = std::chrono::steady_clock::now();
    auto duration = now - s_sharedReferenceTime;
    return std::chrono::duration_cast<std::chrono::microseconds>(duration).count();
}

void AudioCapturer::LogAVSyncStatus() {
    if (!s_sharedReferenceInitialized.load()) {
        std::wcout << L"[AV-Sync] Shared reference clock not initialized" << std::endl;
        return;
    }

    auto currentTime = GetSharedReferenceTimeUs();
    auto wallClock = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();

    std::wcout << L"[AV-Sync] Status:" << std::endl;
    std::wcout << L"  Shared reference time: " << currentTime << L" us" << std::endl;
    std::wcout << L"  Wall clock time: " << wallClock << L" us" << std::endl;
    std::wcout << L"  Time since init: " << (wallClock - currentTime) << L" us" << std::endl;

    if (s_activeInstance && s_activeInstance->m_initialAudioClockTime > 0) {
        std::wcout << L"  Audio initial time: " << s_activeInstance->m_initialAudioClockTime << L" us" << std::endl;
    }
}

// ============================================================================
// AUDIO BITRATE ADAPTATION - RTCP feedback integration
// Fully functional parameter update system for Opus encoder
// ============================================================================

void AudioCapturer::OnRtcpFeedback(double packetLoss, double /*rtt*/, double /*jitter*/) {
    auto now = std::chrono::steady_clock::now();
    auto since = std::chrono::duration_cast<std::chrono::milliseconds>(now - s_lastAudioChange).count();

    // Check for audio queue congestion (complements packet loss detection)
    bool queueCongested = false;
    if (s_activeInstance != nullptr) {
        // Call Go function to check queue congestion
        queueCongested = (checkAudioQueueCongestionGo() != 0);
    }

    // Determine if FEC should be enabled/disabled based on packet loss
    bool shouldEnableFec = (packetLoss >= s_fecEnableThreshold);
    bool shouldDisableFec = (packetLoss <= s_fecDisableThreshold);

    // Ultra-low-latency profile: be more conservative with FEC
    if (s_audioConfig.latency.ultraLowLatencyProfile && s_audioConfig.latency.disableFecInLowLatency) {
        // Only enable FEC for extremely high loss (>10%) in ultra-low-latency mode
        shouldEnableFec = (packetLoss >= 0.10);
        // Keep normal disable threshold for low loss
        shouldDisableFec = (packetLoss <= s_fecDisableThreshold);

        if (packetLoss > s_fecEnableThreshold && packetLoss < 0.10) {
            std::wcout << L"[AudioAdapt] Ultra-low-latency profile: Skipping FEC enable (loss: " << (packetLoss * 100.0)
                      << L"%, threshold: 10.0% for ultra-low-latency)" << std::endl;
            shouldEnableFec = false;
        }
    }

    bool fecStateChanged = false;

    // FEC Control: Enable when loss is high, disable when loss is very low
    if (shouldEnableFec && !s_fecCurrentlyEnabled) {
        s_fecCurrentlyEnabled = true;
        fecStateChanged = true;
        std::wcout << L"[AudioAdapt] Enabling Opus FEC (loss: " << (packetLoss * 100.0) << L"%)" << std::endl;
    } else if (shouldDisableFec && s_fecCurrentlyEnabled) {
        s_fecCurrentlyEnabled = false;
        fecStateChanged = true;
        std::wcout << L"[AudioAdapt] Disabling Opus FEC (loss: " << (packetLoss * 100.0) << L"%)" << std::endl;
    }

    // High packet loss OR queue congestion: Decrease bitrate aggressively
    if (packetLoss >= s_highLossThreshold || queueCongested) {
        if (since >= s_decreaseCooldownMs) {
            // More aggressive decrease for high loss
            double factor = (packetLoss >= 0.10) ? 0.5 : 0.7;
            int target = static_cast<int>(s_currentAudioBitrate.load() * factor);
            int newBitrate = (std::max)(s_minAudioBitrate, target);

            s_currentAudioBitrate.store(newBitrate);
            s_lastAudioChange = now;

            // Update FEC based on loss (higher loss = more FEC) and ensure FEC is enabled
            int newLossPerc = (std::min)(20, static_cast<int>(packetLoss * 100.0));
            int newComplexity = (packetLoss >= 0.10) ? 3 : 5; // Lower complexity for very high loss
            UpdateOpusParameters(newBitrate, newLossPerc, newComplexity, 1); // Force FEC on

            std::wcout << L"[AudioAdapt] High loss detected (" << (packetLoss * 100.0)
                      << L"%)" << (queueCongested ? L" + queue congested" : L"")
                      << L", decreased bitrate to " << newBitrate << L" bps" << std::endl;
        }
        s_cleanSamples = 0;
        return;
    }

    // Low packet loss: Gradually increase bitrate
    s_cleanSamples++;
    if (packetLoss <= s_lowLossThreshold &&
        since >= s_increaseIntervalMs &&
        s_cleanSamples >= s_cleanSamplesRequired) {

        int current = s_currentAudioBitrate.load();
        int target = current + s_increaseStep;
        int newBitrate = (std::min)(s_maxAudioBitrate, target);

        if (newBitrate > current) {
            s_currentAudioBitrate.store(newBitrate);
            s_lastAudioChange = now;

            // Reduce FEC as loss decreases
            int newLossPerc = (std::max)(1, static_cast<int>(packetLoss * 100.0));
            int newComplexity = (packetLoss <= 0.02) ? 6 : 5; // Higher complexity for very low loss

            // Apply FEC change if state changed, otherwise keep current
            int fecUpdate = fecStateChanged ? (s_fecCurrentlyEnabled ? 1 : 0) : -1;
            UpdateOpusParameters(newBitrate, newLossPerc, newComplexity, fecUpdate);

            std::wcout << L"[AudioAdapt] Low loss detected (" << (packetLoss * 100.0)
                      << L"%), increased bitrate to " << newBitrate << L" bps" << std::endl;
        }

        s_cleanSamples = 0;
    } else if (fecStateChanged) {
        // FEC state changed but no bitrate change - still update FEC
        int currentBitrate = s_currentAudioBitrate.load();
        int currentLossPerc = (std::max)(1, static_cast<int>(packetLoss * 100.0));
        int fecUpdate = s_fecCurrentlyEnabled ? 1 : 0;
        UpdateOpusParameters(currentBitrate, currentLossPerc, -1, fecUpdate); // -1 means no complexity change
    }
}





// Audio latency optimization methods
bool AudioCapturer::ValidateOpusPacketization(int frameSizeMs) {
    if (frameSizeMs < 5 || frameSizeMs > 10) {
        std::wcerr << L"[AudioLatency] WARNING: Opus frame size " << frameSizeMs
                  << L"ms outside optimal 5-10ms latency range. Consider using 5-10ms for gaming." << std::endl;
        return false;
    }
    std::wcout << L"[AudioLatency] Opus packetization validated: " << frameSizeMs
              << L"ms frame size within optimal latency range (5-10ms)" << std::endl;
    return true;
}

void AudioCapturer::ReportLatencyStats() {
    if (!s_audioConfig.latency.strictLatencyMode) {
        return; // Only report in strict latency mode
    }

    static auto lastReport = std::chrono::steady_clock::now();
    auto now = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - lastReport);

    if (elapsed.count() >= 30) { // Report every 30 seconds
        std::wcout << L"[AudioLatency] Latency Status Report:" << std::endl;
        std::wcout << L"[AudioLatency]   - Target one-way latency: " << s_audioConfig.latency.targetOneWayLatencyMs << L"ms" << std::endl;
        std::wcout << L"[AudioLatency]   - Opus frame size: " << s_audioConfig.frameSizeMs << L"ms" << std::endl;
        std::wcout << L"[AudioLatency]   - Single frame buffering: "
                  << (s_audioConfig.latency.enforceSingleFrameBuffering ? L"enforced" : L"flexible") << std::endl;
        std::wcout << L"[AudioLatency]   - WASAPI exclusive mode: "
                  << (s_audioConfig.wasapi.preferExclusiveMode ? L"preferred" : L"disabled") << std::endl;
        std::wcout << L"[AudioLatency]   - Event-driven capture: "
                  << (s_audioConfig.wasapi.enforceEventDriven ? L"enforced" : L"fallback-allowed") << std::endl;

        // Estimate current latency based on configuration
        int estimatedLatency = s_audioConfig.frameSizeMs; // Base frame latency
        if (s_audioConfig.wasapi.enforceEventDriven) {
            estimatedLatency += 3; // Event-driven processing overhead
        } else {
            estimatedLatency += 10; // Polling latency
        }

        if (s_audioConfig.wasapi.preferExclusiveMode) {
            estimatedLatency += 2; // Exclusive mode buffer
        } else {
            estimatedLatency += 10; // Shared mode buffer
        }

        std::wcout << L"[AudioLatency]   - Estimated current latency: " << estimatedLatency << L"ms" << std::endl;

        if (estimatedLatency > s_audioConfig.latency.targetOneWayLatencyMs) {
            std::wcerr << L"[AudioLatency] WARNING: Estimated latency (" << estimatedLatency << L"ms) exceeds target ("
                      << s_audioConfig.latency.targetOneWayLatencyMs << L"ms)" << std::endl;
            std::wcerr << L"[AudioLatency] Consider: smaller frame size, exclusive mode, event-driven capture" << std::endl;
        } else {
            std::wcout << L"[AudioLatency] ✓ Within target latency range" << std::endl;
        }

        lastReport = now;
    }
}

// Static method to update Opus encoder parameters dynamically
void AudioCapturer::UpdateOpusParameters(int bitrate, int expectedLossPerc, int complexity, int fecEnabled) {
    // Get the active AudioCapturer instance
    AudioCapturer* activeInstance = s_activeInstance;

    if (!activeInstance) {
        std::wcerr << L"[AudioAdapt] No active AudioCapturer instance found for parameter update" << std::endl;
        return;
    }

    // Queue parameter update to the encoder thread
    activeInstance->QueueParameterUpdate(bitrate, expectedLossPerc, complexity < 0 ? -1 : complexity, fecEnabled);

    std::wcout << L"[AudioAdapt] Queued Opus parameter update: bitrate=" << bitrate
              << L" bps, loss=" << expectedLossPerc << L"%, complexity="
              << (complexity >= 0 ? std::to_wstring(complexity) : L"unchanged") << std::endl;
}

// ============================================================================
// AUDIO CONFIGURATION - Static method for config.json integration
// ============================================================================

void AudioCapturer::SetAudioConfig(const nlohmann::json& config)
{
    try {
        // Read bitrate (64-96 kbps recommended for stereo gaming)
        if (config.contains("bitrate")) {
            int bitrate = config["bitrate"].get<int>();
            s_audioConfig.bitrate = (bitrate < 32000) ? 32000 : ((bitrate > 128000) ? 128000 : bitrate);
        }

        // Read complexity (5-6 recommended for low-latency gaming)
        if (config.contains("complexity")) {
            int complexity = config["complexity"].get<int>();
            s_audioConfig.complexity = (complexity < 0) ? 0 : ((complexity > 10) ? 10 : complexity);
        }

        // Read expected loss percentage (for FEC tuning)
        if (config.contains("expectedLossPerc")) {
            int lossPerc = config["expectedLossPerc"].get<int>();
            s_audioConfig.expectedLossPerc = (lossPerc < 0) ? 0 : ((lossPerc > 100) ? 100 : lossPerc);
        }

        // Read FEC enable flag
        if (config.contains("enableFec")) {
            s_audioConfig.enableFec = config["enableFec"].get<bool>();
        }

        // Read DTX enable flag
        if (config.contains("enableDtx")) {
            s_audioConfig.enableDtx = config["enableDtx"].get<bool>();
        }

        // Read application type
        if (config.contains("application")) {
            s_audioConfig.application = config["application"].get<int>();
        }

        // Read optional deviceId override (UTF-8 in JSON → UTF-16 here)
        if (config.contains("deviceId") && config["deviceId"].is_string()) {
            std::string idUtf8 = config["deviceId"].get<std::string>();
            int wlen = MultiByteToWideChar(CP_UTF8, 0, idUtf8.c_str(), -1, nullptr, 0);
            if (wlen > 0) {
                std::wstring wid(static_cast<size_t>(wlen), L'\0');
                MultiByteToWideChar(CP_UTF8, 0, idUtf8.c_str(), -1, &wid[0], wlen);
                // Trim trailing null inserted by MultiByteToWideChar
                if (!wid.empty() && wid.back() == L'\0') wid.pop_back();
                s_audioConfig.deviceId = wid;
            }
        }

        // Read frame size in milliseconds (enforce 5-10ms for low latency when strict mode enabled)
        if (config.contains("frameSizeMs")) {
            int frameSizeMs = config["frameSizeMs"].get<int>();

            // Check if strict latency mode or ultra-low-latency profile is enabled
            bool isStrictMode = false;
            bool isUltraLowLatency = false;
            if (config.contains("latency")) {
                auto latencyConfig = config["latency"];
                isStrictMode = latencyConfig.value("strictLatencyMode", true);
                isUltraLowLatency = latencyConfig.value("ultraLowLatencyProfile", false);
            }

            if (isUltraLowLatency) {
                // Ultra-low-latency profile: enforce 5ms frames
                if (frameSizeMs == 5) {
                    s_audioConfig.frameSizeMs = 5;
                } else {
                    std::wcout << L"[AudioConfig] Ultra-low-latency profile: Forcing 5ms frames (configured: " << frameSizeMs << L"ms)" << std::endl;
                    s_audioConfig.frameSizeMs = 5; // Force 5ms for ultra-low-latency
                }
            } else if (isStrictMode) {
                // Strict latency mode: enforce 5-10ms range
                if (frameSizeMs >= 5 && frameSizeMs <= 10) {
                    s_audioConfig.frameSizeMs = frameSizeMs;
                } else {
                    std::wcerr << L"[AudioConfig] Strict latency mode: Frame size " << frameSizeMs
                              << L"ms outside 5-10ms range, using 10ms" << std::endl;
                    s_audioConfig.frameSizeMs = 10; // Default to 10ms
                }
            } else {
                // Legacy validation for backward compatibility
                if (frameSizeMs == 10 || frameSizeMs == 20 || frameSizeMs == 40) {
                    s_audioConfig.frameSizeMs = frameSizeMs;
                }
            }
        }

        // Read number of channels
        if (config.contains("channels")) {
            int channels = config["channels"].get<int>();
            if (channels == 1 || channels == 2) {
                s_audioConfig.channels = channels;
            }
        }

        // Read thread affinity settings
        if (config.contains("useThreadAffinity")) {
            s_audioConfig.useThreadAffinity = config["useThreadAffinity"].get<bool>();
        }

        if (config.contains("encoderThreadAffinityMask")) {
            try {
                s_audioConfig.encoderThreadAffinityMask = static_cast<DWORD>(config["encoderThreadAffinityMask"].get<int>());
            } catch (...) {
                s_audioConfig.encoderThreadAffinityMask = 0;
            }
        }

        // Read WASAPI-specific configuration
        if (config.contains("wasapi")) {
            auto wasapiConfig = config["wasapi"];

            s_audioConfig.wasapi.preferExclusiveMode = wasapiConfig.value("preferExclusiveMode", true);
            s_audioConfig.wasapi.enforceEventDriven = wasapiConfig.value("enforceEventDriven", true);
            s_audioConfig.wasapi.devicePeriodMs = wasapiConfig.value("devicePeriodMs", 2.5);
            s_audioConfig.wasapi.fallbackPeriodMs = wasapiConfig.value("fallbackPeriodMs", 5.0);
            s_audioConfig.wasapi.force48kHzStereo = wasapiConfig.value("force48kHzStereo", true);
            s_audioConfig.wasapi.preferLinearResampling = wasapiConfig.value("preferLinearResampling", true);
            s_audioConfig.wasapi.useDmoOnlyForHighQuality = wasapiConfig.value("useDmoOnlyForHighQuality", false);

            std::wcout << L"[AudioCapturer] WASAPI config: exclusive=" << (s_audioConfig.wasapi.preferExclusiveMode ? L"preferred" : L"disabled")
                      << L", event-driven=" << (s_audioConfig.wasapi.enforceEventDriven ? L"enforced" : L"fallback-allowed")
                      << L", device_period=" << s_audioConfig.wasapi.devicePeriodMs << L"ms"
                      << L", force_48kHz=" << (s_audioConfig.wasapi.force48kHzStereo ? L"enabled" : L"disabled")
                      << L", linear_resample=" << (s_audioConfig.wasapi.preferLinearResampling ? L"preferred" : L"DMO-first")
                      << std::endl;
        }

        // Per-process loopback config (optional)
        if (config.contains("processLoopback")) {
            auto plCfg = config["processLoopback"];
            s_audioConfig.processLoopback.enabled = plCfg.value("enabled", true);
            s_audioConfig.processLoopback.includeProcessTree = plCfg.value("includeProcessTree", true);
            std::wcout << L"[AudioCapturer] Process loopback: enabled="
                       << (s_audioConfig.processLoopback.enabled ? L"true" : L"false")
                       << L", includeTree="
                       << (s_audioConfig.processLoopback.includeProcessTree ? L"true" : L"false")
                       << std::endl;
        }

        // Read latency optimization configuration
        if (config.contains("latency")) {
            auto latencyConfig = config["latency"];

            s_audioConfig.latency.enforceSingleFrameBuffering = latencyConfig.value("enforceSingleFrameBuffering", true);
            s_audioConfig.latency.maxFrameSizeMs = latencyConfig.value("maxFrameSizeMs", 10);
            s_audioConfig.latency.minFrameSizeMs = latencyConfig.value("minFrameSizeMs", 5);
            s_audioConfig.latency.strictLatencyMode = latencyConfig.value("strictLatencyMode", true);
            s_audioConfig.latency.warnOnBuffering = latencyConfig.value("warnOnBuffering", true);
            s_audioConfig.latency.targetOneWayLatencyMs = latencyConfig.value("targetOneWayLatencyMs", 20);
            s_audioConfig.latency.ultraLowLatencyProfile = latencyConfig.value("ultraLowLatencyProfile", false);
            s_audioConfig.latency.disableFecInLowLatency = latencyConfig.value("disableFecInLowLatency", true);

            std::wcout << L"[AudioCapturer] Latency config: strict_mode=" << (s_audioConfig.latency.strictLatencyMode ? L"enabled" : L"disabled")
                      << L", ultra_low_latency=" << (s_audioConfig.latency.ultraLowLatencyProfile ? L"enabled" : L"disabled")
                      << L", frame_size=" << s_audioConfig.latency.minFrameSizeMs << L"-" << s_audioConfig.latency.maxFrameSizeMs << L"ms"
                      << L", single_frame_buffering=" << (s_audioConfig.latency.enforceSingleFrameBuffering ? L"enforced" : L"flexible")
                      << L", target_latency=" << s_audioConfig.latency.targetOneWayLatencyMs << L"ms"
                      << std::endl;
        }

        // Read bitrate adaptation settings
        if (config.contains("bitrateAdaptation")) {
            auto adaptConfig = config["bitrateAdaptation"];

            bool adaptationEnabled = adaptConfig.value("enabled", true);
            if (adaptationEnabled) {
                s_minAudioBitrate = adaptConfig.value("minBitrate", 8000);
                s_maxAudioBitrate = adaptConfig.value("maxBitrate", 128000);
                s_decreaseCooldownMs = adaptConfig.value("decreaseCooldownMs", 2000);
                s_increaseIntervalMs = adaptConfig.value("increaseIntervalMs", 10000);
                s_increaseStep = adaptConfig.value("increaseStep", 8000);
                s_highLossThreshold = adaptConfig.value("highLossThreshold", 0.05);
                s_lowLossThreshold = adaptConfig.value("lowLossThreshold", 0.01);
                s_cleanSamplesRequired = adaptConfig.value("cleanSamplesRequired", 30);

                // Read FEC control thresholds
                s_fecEnableThreshold = adaptConfig.value("fecEnableThreshold", 0.03);
                s_fecDisableThreshold = adaptConfig.value("fecDisableThreshold", 0.005);

                std::wcout << L"[AudioAdapt] Configured: min=" << s_minAudioBitrate
                          << L" max=" << s_maxAudioBitrate << L" bps, decrease_cooldown="
                          << s_decreaseCooldownMs << L"ms, high_loss=" << (s_highLossThreshold * 100.0) << L"%, "
                          << L"FEC_enable=" << (s_fecEnableThreshold * 100.0) << L"%, FEC_disable=" << (s_fecDisableThreshold * 100.0) << L"%"
                          << std::endl;
            } else {
                std::wcout << L"[AudioAdapt] Bitrate adaptation disabled in config" << std::endl;
            }
        }

        std::wcout << L"[AudioCapturer] Audio config loaded: bitrate=" << s_audioConfig.bitrate
                   << L" bps, complexity=" << s_audioConfig.complexity
                   << L", frameSize=" << s_audioConfig.frameSizeMs << L"ms, channels=" << s_audioConfig.channels
                   << L", threadAffinity=" << (s_audioConfig.useThreadAffinity ? L"enabled" : L"disabled")
                   << std::endl;

    } catch (const std::exception& e) {
        std::wcerr << L"[AudioCapturer] Error parsing audio config: " << e.what() << std::endl;
    } catch (...) {
        std::wcerr << L"[AudioCapturer] Unknown error parsing audio config" << std::endl;
    }
}

// ============================================================================
// DMO RESAMPLER IMPLEMENTATION - High-quality audio resampling
// ============================================================================

bool AudioCapturer::InitializeDMOResampler(uint32_t inputSampleRate, uint32_t inputChannels)
{
    // Skip if already initialized with same parameters
    if (m_resamplerInitialized &&
        m_currentInputSampleRate == inputSampleRate &&
        m_currentInputChannels == inputChannels) {
        return true;
    }

    // Clean up existing resampler if parameters changed
    if (m_resamplerInitialized) {
        CleanupDMOResampler();
    }

    HRESULT hr;

    // Create Audio Resampler DMO
    hr = CoCreateInstance(CLSID_CResamplerMediaObject, nullptr, CLSCTX_INPROC_SERVER,
                         IID_IMediaObject, reinterpret_cast<void**>(m_audioResamplerDMO.GetAddressOf()));
    if (FAILED(hr)) {
        std::wcerr << L"[AudioCapturer] Failed to create Audio Resampler DMO: " << _com_error(hr).ErrorMessage() << std::endl;
        return false;
    }

    // Set up input media type (source format)
    ZeroMemory(&m_inputMediaType, sizeof(DMO_MEDIA_TYPE));
    m_inputMediaType.majortype = MEDIATYPE_Audio;
    m_inputMediaType.subtype = MEDIASUBTYPE_IEEE_FLOAT;
    m_inputMediaType.bFixedSizeSamples = TRUE;
    m_inputMediaType.bTemporalCompression = FALSE;
    m_inputMediaType.lSampleSize = inputChannels * sizeof(float);
    m_inputMediaType.formattype = FORMAT_WaveFormatEx;
    m_inputMediaType.cbFormat = sizeof(WAVEFORMATEX);

    WAVEFORMATEX* pInputWfx = reinterpret_cast<WAVEFORMATEX*>(CoTaskMemAlloc(sizeof(WAVEFORMATEX)));
    if (!pInputWfx) {
        std::wcerr << L"[AudioCapturer] Failed to allocate input WAVEFORMATEX" << std::endl;
        CleanupDMOResampler();
        return false;
    }

    pInputWfx->wFormatTag = WAVE_FORMAT_IEEE_FLOAT;
    pInputWfx->nChannels = static_cast<WORD>(inputChannels);
    pInputWfx->nSamplesPerSec = inputSampleRate;
    pInputWfx->nAvgBytesPerSec = inputSampleRate * inputChannels * sizeof(float);
    pInputWfx->nBlockAlign = static_cast<WORD>(inputChannels * sizeof(float));
    pInputWfx->wBitsPerSample = 32;
    pInputWfx->cbSize = 0;

    m_inputMediaType.pbFormat = reinterpret_cast<BYTE*>(pInputWfx);

    // Set up output media type (target format: 48kHz)
    ZeroMemory(&m_outputMediaType, sizeof(DMO_MEDIA_TYPE));
    m_outputMediaType.majortype = MEDIATYPE_Audio;
    m_outputMediaType.subtype = MEDIASUBTYPE_IEEE_FLOAT;
    m_outputMediaType.bFixedSizeSamples = TRUE;
    m_outputMediaType.bTemporalCompression = FALSE;
    m_outputMediaType.lSampleSize = inputChannels * sizeof(float);
    m_outputMediaType.formattype = FORMAT_WaveFormatEx;
    m_outputMediaType.cbFormat = sizeof(WAVEFORMATEX);

    WAVEFORMATEX* pOutputWfx = reinterpret_cast<WAVEFORMATEX*>(CoTaskMemAlloc(sizeof(WAVEFORMATEX)));
    if (!pOutputWfx) {
        std::wcerr << L"[AudioCapturer] Failed to allocate output WAVEFORMATEX" << std::endl;
        CleanupDMOResampler();
        return false;
    }

    pOutputWfx->wFormatTag = WAVE_FORMAT_IEEE_FLOAT;
    pOutputWfx->nChannels = static_cast<WORD>(inputChannels);
    pOutputWfx->nSamplesPerSec = 48000; // Target sample rate
    pOutputWfx->nAvgBytesPerSec = 48000 * inputChannels * sizeof(float);
    pOutputWfx->nBlockAlign = static_cast<WORD>(inputChannels * sizeof(float));
    pOutputWfx->wBitsPerSample = 32;
    pOutputWfx->cbSize = 0;

    m_outputMediaType.pbFormat = reinterpret_cast<BYTE*>(pOutputWfx);

    // Set input and output types
    hr = m_audioResamplerDMO->SetInputType(0, &m_inputMediaType, 0);
    if (FAILED(hr)) {
        std::wcerr << L"[AudioCapturer] Failed to set input type: " << _com_error(hr).ErrorMessage() << std::endl;
        CleanupDMOResampler();
        return false;
    }

    hr = m_audioResamplerDMO->SetOutputType(0, &m_outputMediaType, 0);
    if (FAILED(hr)) {
        std::wcerr << L"[AudioCapturer] Failed to set output type: " << _com_error(hr).ErrorMessage() << std::endl;
        CleanupDMOResampler();
        return false;
    }

    // Allocate input and output buffers
    m_inputBuffer.Attach(new CMediaBuffer(sizeof(float) * 4096)); // 4KB buffer
    if (!m_inputBuffer) {
        std::wcerr << L"[AudioCapturer] Failed to create input buffer" << std::endl;
        CleanupDMOResampler();
        return false;
    }

    m_outputBuffer.Attach(new CMediaBuffer(sizeof(float) * 4096)); // 4KB buffer
    if (!m_outputBuffer) {
        std::wcerr << L"[AudioCapturer] Failed to create output buffer" << std::endl;
        CleanupDMOResampler();
        return false;
    }

    // Store current parameters
    m_currentInputSampleRate = inputSampleRate;
    m_currentInputChannels = inputChannels;
    m_resamplerInitialized = true;

    std::wcout << L"[AudioCapturer] DMO Resampler initialized: " << inputSampleRate << L"Hz -> 48kHz, "
               << inputChannels << L" channels" << std::endl;

    return true;
}

bool AudioCapturer::ProcessResamplerDMOInPlace(std::vector<float>& buffer)
{
    // ============================================================================
    // ZERO-COPY DMO RESAMPLING
    // ============================================================================
    // This function performs DMO resampling directly in the provided buffer,
    // eliminating temporary vector allocations and copies.

    if (!m_resamplerInitialized || buffer.empty()) {
        return false;
    }

    size_t inputSamples = buffer.size();
    HRESULT hr;
    DWORD dwStatus = 0;
    size_t totalOutputSamples = 0;

    // Calculate input buffer size needed
    size_t inputBytes = inputSamples * sizeof(float);

    // Process input in chunks if needed
    BYTE* pInputBuffer = nullptr;
    hr = m_inputBuffer->GetBufferAndLength(&pInputBuffer, nullptr);
    if (FAILED(hr)) {
        std::wcerr << L"[AudioCapturer] Failed to get input buffer: " << _com_error(hr).ErrorMessage() << std::endl;
        return false;
    }

    // Copy input data to DMO buffer
    memcpy(pInputBuffer, buffer.data(), inputBytes);

    hr = m_inputBuffer->SetLength(static_cast<DWORD>(inputBytes));
    if (FAILED(hr)) {
        std::wcerr << L"[AudioCapturer] Failed to set input buffer length: " << _com_error(hr).ErrorMessage() << std::endl;
        return false;
    }

    // Process the input
    hr = m_audioResamplerDMO->ProcessInput(0, m_inputBuffer.Get(), 0, 0, 0);
    if (FAILED(hr)) {
        std::wcerr << L"[AudioCapturer] ProcessInput failed: " << _com_error(hr).ErrorMessage() << std::endl;
        return false;
    }

    // Get output directly into the provided buffer
    buffer.clear();
    std::vector<float> tempBuffer;

    while (true) {
        hr = m_audioResamplerDMO->ProcessOutput(0, 0, nullptr, &dwStatus);

        if (hr == S_FALSE) {
            // Need more input
            break;
        } else if (FAILED(hr)) {
            std::wcerr << L"[AudioCapturer] ProcessOutput failed: " << _com_error(hr).ErrorMessage() << std::endl;
            return false;
        }

        if (dwStatus & 0x00000001) {  // DMO_OUTPUT_DATA_BUFFERFILLED - Output buffer contains valid data
            // Get output data
            BYTE* pOutputBuffer = nullptr;
            DWORD outputLength = 0;
            hr = m_outputBuffer->GetBufferAndLength(&pOutputBuffer, &outputLength);
            if (FAILED(hr)) {
                std::wcerr << L"[AudioCapturer] Failed to get output buffer: " << _com_error(hr).ErrorMessage() << std::endl;
                return false;
            }

            // Copy output data directly to temp buffer
            size_t outputSamples = outputLength / sizeof(float);
            tempBuffer.resize(outputSamples);
            memcpy(tempBuffer.data(), pOutputBuffer, outputLength);

            // Append to final output (buffer)
            buffer.insert(buffer.end(), tempBuffer.begin(), tempBuffer.end());
            totalOutputSamples += outputSamples;
        }
    }

    if (buffer.empty()) {
        std::wcerr << L"[AudioCapturer] DMO in-place resampler produced no output" << std::endl;
        return false;
    }

    // Validate frame alignment for Opus encoding
    // Opus frames should be multiples of 480 samples per channel (10ms at 48kHz)
    size_t outputFramesPerChannel = buffer.size() / m_currentInputChannels;
    const size_t OPUS_FRAME_SAMPLES = 480; // 10ms at 48kHz per channel

    if (outputFramesPerChannel % OPUS_FRAME_SAMPLES != 0) {
        std::wcerr << L"[AudioCapturer] Warning: DMO in-place output not aligned with Opus frames. "
                   << L"Output samples per channel: " << outputFramesPerChannel
                   << L", expected multiple of " << OPUS_FRAME_SAMPLES << std::endl;
    }

    return true;
}

bool AudioCapturer::ProcessResamplerDMO(const float* inputData, size_t inputSamples, std::vector<float>& outputData)
{
    if (!m_resamplerInitialized || !inputData || inputSamples == 0) {
        return false;
    }

    HRESULT hr;
    DWORD dwStatus = 0;
    size_t totalOutputSamples = 0;

    // Calculate input buffer size needed
    size_t inputBytes = inputSamples * sizeof(float);

    // Process input in chunks if needed
    BYTE* pInputBuffer = nullptr;
    hr = m_inputBuffer->GetBufferAndLength(&pInputBuffer, nullptr);
    if (FAILED(hr)) {
        std::wcerr << L"[AudioCapturer] Failed to get input buffer: " << _com_error(hr).ErrorMessage() << std::endl;
        return false;
    }

    // Copy input data to DMO buffer
    memcpy(pInputBuffer, inputData, inputBytes);

    hr = m_inputBuffer->SetLength(static_cast<DWORD>(inputBytes));
    if (FAILED(hr)) {
        std::wcerr << L"[AudioCapturer] Failed to set input buffer length: " << _com_error(hr).ErrorMessage() << std::endl;
        return false;
    }

    // Process the input
    hr = m_audioResamplerDMO->ProcessInput(0, m_inputBuffer.Get(), 0, 0, 0);
    if (FAILED(hr)) {
        std::wcerr << L"[AudioCapturer] ProcessInput failed: " << _com_error(hr).ErrorMessage() << std::endl;
        return false;
    }

    // Get output
    outputData.clear();
    std::vector<float> tempBuffer;

    while (true) {
        hr = m_audioResamplerDMO->ProcessOutput(0, 0, nullptr, &dwStatus);

        if (hr == S_FALSE) {
            // Need more input
            break;
        } else if (FAILED(hr)) {
            std::wcerr << L"[AudioCapturer] ProcessOutput failed: " << _com_error(hr).ErrorMessage() << std::endl;
            return false;
        }

        if (dwStatus & 0x00000001) {  // DMO_OUTPUT_DATA_BUFFERFILLED - Output buffer contains valid data
            // Get output data
            BYTE* pOutputBuffer = nullptr;
            DWORD outputLength = 0;
            hr = m_outputBuffer->GetBufferAndLength(&pOutputBuffer, &outputLength);
            if (FAILED(hr)) {
                std::wcerr << L"[AudioCapturer] Failed to get output buffer: " << _com_error(hr).ErrorMessage() << std::endl;
                return false;
            }

            // Copy output data
            size_t outputSamples = outputLength / sizeof(float);
            tempBuffer.resize(outputSamples);
            memcpy(tempBuffer.data(), pOutputBuffer, outputLength);

            // Append to final output
            outputData.insert(outputData.end(), tempBuffer.begin(), tempBuffer.end());
            totalOutputSamples += outputSamples;
        }
    }

    if (outputData.empty()) {
        std::wcerr << L"[AudioCapturer] DMO resampler produced no output" << std::endl;
        return false;
    }

    // Validate frame alignment for Opus encoding
    // Opus frames should be multiples of 480 samples per channel (10ms at 48kHz)
    size_t outputFramesPerChannel = outputData.size() / m_currentInputChannels;
    const size_t OPUS_FRAME_SAMPLES = 480; // 10ms at 48kHz per channel

    if (outputFramesPerChannel % OPUS_FRAME_SAMPLES != 0) {
        std::wcerr << L"[AudioCapturer] Warning: DMO output not aligned with Opus frames. "
                   << L"Output samples per channel: " << outputFramesPerChannel
                   << L", expected multiple of " << OPUS_FRAME_SAMPLES << std::endl;
    }

    return true;
}

void AudioCapturer::CleanupDMOResampler()
{
    if (m_audioResamplerDMO) {
        m_audioResamplerDMO->Flush();
        m_audioResamplerDMO.Reset();
    }

    if (m_inputBuffer) {
        m_inputBuffer.Reset();
    }

    if (m_outputBuffer) {
        m_outputBuffer.Reset();
    }

    // Free media type formats
    if (m_inputMediaType.pbFormat) {
        CoTaskMemFree(m_inputMediaType.pbFormat);
        m_inputMediaType.pbFormat = nullptr;
    }

    if (m_outputMediaType.pbFormat) {
        CoTaskMemFree(m_outputMediaType.pbFormat);
        m_outputMediaType.pbFormat = nullptr;
    }

    ZeroMemory(&m_inputMediaType, sizeof(DMO_MEDIA_TYPE));
    ZeroMemory(&m_outputMediaType, sizeof(DMO_MEDIA_TYPE));

    m_resamplerInitialized = false;
    m_currentInputSampleRate = 0;
    m_currentInputChannels = 0;

    std::wcout << L"[AudioCapturer] DMO Resampler cleaned up" << std::endl;
}

// ============================================================================
// RESAMPLER QUALITY TESTING AND VALIDATION
// ============================================================================

void AudioCapturer::TestResamplerQuality(uint32_t testSampleRate, uint32_t testChannels)
{
    std::wcout << L"[AudioCapturer] Testing resampler quality: " << testSampleRate << L"Hz -> 48kHz, "
               << testChannels << L" channels" << std::endl;

    // Generate a test sine wave
    const size_t TEST_DURATION_MS = 100; // 100ms test
    const size_t testSamples = (testSampleRate * TEST_DURATION_MS) / 1000;
    const float testFrequency = 1000.0f; // 1kHz test tone

    std::vector<float> inputData(testSamples * testChannels);

    // Generate sine wave
    for (size_t i = 0; i < testSamples; ++i) {
        float sample = sinf(2.0f * 3.14159f * testFrequency * static_cast<float>(i) / testSampleRate) * 0.5f;
        for (uint32_t ch = 0; ch < testChannels; ++ch) {
            inputData[i * testChannels + ch] = sample;
        }
    }

    // Test DMO resampler
    std::vector<float> dmoOutput;
    bool dmoSuccess = false;

    if (InitializeDMOResampler(testSampleRate, testChannels)) {
        dmoSuccess = ProcessResamplerDMO(inputData.data(), inputData.size(), dmoOutput);
        if (dmoSuccess) {
            std::wcout << L"[AudioCapturer] DMO resampler test passed: "
                       << inputData.size() << L" -> " << dmoOutput.size() << L" samples" << std::endl;
        }
    }

    // Test linear interpolation fallback
    std::vector<float> linearOutput;
    ResampleTo48k(inputData.data(), testSamples, testSampleRate, testChannels, linearOutput);

    // Compare results
    if (dmoSuccess && !dmoOutput.empty() && !linearOutput.empty()) {
        // Calculate RMS difference
        size_t minSize = (dmoOutput.size() < linearOutput.size()) ? dmoOutput.size() : linearOutput.size();
        double rmsDifference = 0.0;
        size_t validSamples = 0;

        for (size_t i = 0; i < minSize; ++i) {
            if (i < linearOutput.size()) {
                double diff = dmoOutput[i] - linearOutput[i];
                rmsDifference += diff * diff;
                validSamples++;
            }
        }

        if (validSamples > 0) {
            rmsDifference = sqrt(rmsDifference / validSamples);
            std::wcout << L"[AudioCapturer] Quality comparison: RMS difference = " << rmsDifference << L" "
                       << L"(lower is better for DMO resampler)" << std::endl;
        }
    }

    // Clean up
    CleanupDMOResampler();

    std::wcout << L"[AudioCapturer] Resampler quality test completed" << std::endl;
}

void AudioCapturer::FinalizeWAVOnExit()
{
    // Best effort: this static hook cannot access instance members reliably
    // but if a file is left open (rare), it is safer to leave OS to close it.
    // Instance-level destructor already finalizes per-instance files.
}

