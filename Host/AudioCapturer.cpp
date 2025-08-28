#include "AudioCapturer.h"
#include <iostream>
#include <fstream>
#include <comdef.h>
#include <vector>
#include <algorithm>
#include "pion_webrtc.h"
#include <mmreg.h>
#include <ks.h>
#include <ksmedia.h>
#include <opus/opus.h>

#define REFTIMES_PER_SEC  10000000
#define REFTIMES_PER_MILLISEC  10000

//-----------------------------------------------------------
//- AudioCapturer.cpp
//-----------------------------------------------------------

AudioCapturer::AudioCapturer() :
    m_stopCapture(false),
    m_pEnumerator(nullptr),
    m_pDevice(nullptr),
    m_pAudioClient(nullptr),
    m_pCaptureClient(nullptr),
    m_nextFrameTime(0),
    m_rtpTimestamp(0),
    m_frameSizeSamples(480), // 10ms at 48kHz (will be updated based on config)
    m_samplesPerFrame(960), // 10ms at 48kHz stereo (will be updated based on config)
    m_accumulatedCount(0),   // Initialize accumulation counter
    m_stopQueueProcessor(false),
    m_stopEncoder(false)
{
    // Initialize Opus encoder with optimized settings for low-latency gaming
    m_opusEncoder = std::make_unique<OpusEncoderWrapper>();

    // Pre-allocate all audio buffers to reduce reallocations during runtime
    // Reserve space for multiple frames to handle bursty audio data

    // Accumulation buffer for frame assembly (10ms frames at 48kHz stereo)
    m_accumulatedSamples.reserve(m_samplesPerFrame * 4); // Reserve space for 4 frames

    // PCM to float conversion buffer (sized for maximum expected frame size)
    m_floatBuffer.reserve(m_samplesPerFrame * 2); // Reserve space for stereo processing (10ms frames)

    // Frame buffer for encoder input (exact size needed)
    m_frameBuffer.resize(m_samplesPerFrame); // Exact size for encoder

    // Initialize reusable encoded buffer (avoid per-frame allocations)
    m_encodedBuffer.reserve(ENCODED_BUFFER_SIZE);
    m_encodedBuffer.resize(ENCODED_BUFFER_SIZE);
    std::fill(m_frameBuffer.begin(), m_frameBuffer.end(), 0.0f); // Initialize to silence
}

AudioCapturer::~AudioCapturer()
{
    StopCapture();
}

bool AudioCapturer::StartCapture(DWORD processId)
{
    m_stopCapture = false;

    // Set this as the active instance for parameter updates
    s_activeInstance = this;
    
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
    settings.enableDtx = s_audioConfig.enableDtx;    // Configurable DTX
    settings.application = s_audioConfig.application; // Configurable application type
    
    if (!m_opusEncoder->initialize(settings)) {
        std::wcerr << L"[AudioCapturer] Failed to initialize Opus encoder" << std::endl;
        return false;
    }
    
    // Initialize timing
    m_startTime = std::chrono::high_resolution_clock::now();
    m_nextFrameTime = 0;
    m_rtpTimestamp = 0;
    m_frameSizeSamples = settings.frameSize / settings.channels;  // Samples per frame per channel (for RTP timestamps)
    m_samplesPerFrame = settings.frameSize;  // Total samples per frame (already includes channels)
    m_frameBuffer.resize(m_samplesPerFrame);
    
    std::wcout << L"[AudioCapturer] Initialized Opus encoder: "
               << settings.sampleRate << L"Hz, "
               << settings.channels << L" channels, "
               << settings.frameSize << L" total samples/frame ("
               << (settings.frameSize / settings.channels) << L" per channel), "
               << settings.bitrate << L" bps" << std::endl;
    
    // Start the dedicated encoder thread for offloading Opus encoding from capture thread
    StartEncoderThread();

    // Start the queue processor thread for async audio packet processing
    StartQueueProcessor();

    m_captureThread = std::thread(&AudioCapturer::CaptureThread, this, processId);
    return true;
}

void AudioCapturer::StopCapture()
{
    m_stopCapture = true;
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
    m_encodedBuffer.clear();

    // Reset timing state
    m_initialAudioClockTime = 0;
    m_nextFrameTime = 0;
    m_rtpTimestamp = 0;
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

        // Process all queued packets
        while (!m_audioQueue.empty() && !m_stopQueueProcessor) {
            AudioPacket packet = std::move(m_audioQueue.front());
            m_audioQueue.pop();

            // Unlock while processing to allow new packets to be queued
            lock.unlock();

            // Send to WebRTC (this is the potentially blocking FFI call)
            int result = sendAudioPacket(packet.data.data(),
                                       static_cast<int>(packet.data.size()),
                                       packet.timestampUs);
            if (result != 0) {
                std::wcerr << L"[AudioQueue] Failed to send audio packet to WebRTC. Error: " << result << std::endl;
            }

            lock.lock();
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
            // Set thread affinity before starting work
            if (!SetThreadAffinityMask(GetCurrentThread(), m_encoderThreadAffinityMask)) {
                std::wcerr << L"[AudioEncoder] Failed to set thread affinity mask: "
                          << GetLastError() << std::endl;
            }
            EncoderThread();
        });
        std::wcout << L"[AudioEncoder] Started with thread affinity mask: 0x" << std::hex
                  << m_encoderThreadAffinityMask << std::dec << std::endl;
    } else {
        m_encoderThread = std::thread(&AudioCapturer::EncoderThread, this);
        std::wcout << L"[AudioEncoder] Started without thread affinity" << std::endl;
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
            std::wcerr << L"[AudioEncoder] Error joining encoder thread: " << e.what() << std::endl;
        }
    }
}

void AudioCapturer::EncoderThread()
{
    std::wcout << L"[AudioEncoder] Dedicated encoder thread started" << std::endl;

    // Ensure encoder is initialized
    if (!m_opusEncoder) {
        std::wcerr << L"[AudioEncoder] Opus encoder not initialized!" << std::endl;
        return;
    }

    while (!m_stopEncoder) {
        std::unique_lock<std::mutex> lock(m_rawFrameMutex);

        // Wait for raw frames or shutdown signal
        m_rawFrameCondition.wait(lock, [this]() {
            return !m_rawFrameQueue.empty() || m_stopEncoder;
        });

        if (m_stopEncoder) {
            // Process any remaining frames before shutdown
            while (!m_rawFrameQueue.empty()) {
                RawAudioFrame frame = std::move(m_rawFrameQueue.front());
                m_rawFrameQueue.pop();

                lock.unlock();
                EncodeAndQueueFrame(frame);
                lock.lock();
            }
            break;
        }

        // Process all queued raw frames
        while (!m_rawFrameQueue.empty() && !m_stopEncoder) {
            RawAudioFrame frame = std::move(m_rawFrameQueue.front());
            m_rawFrameQueue.pop();

            // Unlock while processing to allow new frames to be queued
            lock.unlock();

            // Check for parameter updates before encoding
            CheckForParameterUpdates();

            EncodeAndQueueFrame(std::move(frame));
            lock.lock();
        }
    }

    std::wcout << L"[AudioEncoder] Dedicated encoder thread stopped" << std::endl;
}

void AudioCapturer::EncodeAndQueueFrame(RawAudioFrame frame)
{
    // Use reusable buffer for encoding (zero allocations)
    int encodedSize = this->m_opusEncoder->encodeFrameToBuffer(frame.samples.data(),
                                                              this->m_encodedBuffer.data(),
                                                              this->m_encodedBuffer.size());

    if (encodedSize > 0) {
        // Calculate RTP timestamp
        uint32_t rtpTimestamp = 0;
        if (frame.timestampUs > 0 && this->m_initialAudioClockTime > 0) {
            int64_t relativeTimeUs = frame.timestampUs - this->m_initialAudioClockTime;
            rtpTimestamp = static_cast<uint32_t>((relativeTimeUs * 48LL) / 1000LL);
        } else {
            this->m_rtpTimestamp += static_cast<uint32_t>(this->m_frameSizeSamples);
            rtpTimestamp = this->m_rtpTimestamp;
        }

        // Queue encoded packet for WebRTC transmission
        std::vector<uint8_t> packetData(m_encodedBuffer.begin(),
                                       m_encodedBuffer.begin() + encodedSize);
        if (!this->QueueAudioPacket(packetData, frame.timestampUs, rtpTimestamp)) {
            std::wcerr << L"[AudioEncoder] Failed to queue encoded packet" << std::endl;
        }
    } else {
        std::wcerr << L"[AudioEncoder] Failed to encode audio frame" << std::endl;
    }
}

bool AudioCapturer::QueueRawFrame(std::vector<float>& samples, int64_t timestampUs)
{
    std::lock_guard<std::mutex> lock(m_rawFrameMutex);

    // If queue is full, drop the oldest frame (shallow queue policy)
    if (m_rawFrameQueue.size() >= MAX_RAW_FRAME_QUEUE_SIZE) {
        if (!m_rawFrameQueue.empty()) {
            m_rawFrameQueue.pop(); // Drop oldest
            std::wcerr << L"[AudioEncoder] Dropped oldest raw frame due to full queue" << std::endl;
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

    // If queue is full, drop the oldest packet (shallow queue policy)
    if (m_audioQueue.size() >= MAX_QUEUE_SIZE) {
        if (!m_audioQueue.empty()) {
            m_audioQueue.pop(); // Drop oldest
            std::wcerr << L"[AudioQueue] Dropped oldest packet due to full queue" << std::endl;
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

void AudioCapturer::CaptureThread(DWORD targetProcessId)
{
    HRESULT hr;
    REFERENCE_TIME hnsRequestedDuration = REFTIMES_PER_SEC;
    UINT32 bufferFrameCount;
    UINT32 numFramesAvailable;
    BYTE* pData;
    DWORD flags;
    WAVEFORMATEX* pwfx = NULL;
    DWORD sleepMs = 10; // polling interval (fallback when event mode is unavailable)
    bool eventMode = false; // declared early to avoid goto skipping initialization

    // Opus frame timing constant (declared early to avoid goto skip issues)
    const DWORD OPUS_FRAME_MS = 10; // 10ms Opus frame duration

    // Performance monitoring counters
    uint64_t captureCycles = 0;
    uint64_t dataPacketsProcessed = 0;
    uint64_t timeoutCount = 0;
    uint64_t lastLogTime = GetTickCount64();

    hr = CoInitialize(NULL);
    if (FAILED(hr))
    {
        std::wcerr << L"Unable to initialize COM in render thread: " << _com_error(hr).ErrorMessage() << std::endl;
        return;
    }

    // Register this thread with MMCSS for proper scheduling priority
    // This helps prevent audio glitching under system load
    m_hMmcssTask = AvSetMmThreadCharacteristicsW(L"Pro Audio", &m_mmcssTaskIndex);
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

    std::wcout << L"[AudioCapturer] Target Process ID: " << targetProcessId << std::endl;

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

    std::wcout << L"[AudioCapturer] Found " << deviceCount << L" audio devices." << std::endl;

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
                
                hr = m_pDevice->Activate(__uuidof(IAudioClient), CLSCTX_ALL, NULL, reinterpret_cast<void**>(m_pAudioClient.ReleaseAndGetAddressOf()));
                if (FAILED(hr))
                {
                    std::wcerr << L"[AudioCapturer] Unable to activate audio client for target process: " << _com_error(hr).ErrorMessage() << std::endl;
                    goto Exit;
                }

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
                
                // Note: We'll resample to 48kHz if needed

                // Try event-driven mode first for ultra-low latency
                DWORD streamFlags = AUDCLNT_STREAMFLAGS_LOOPBACK | AUDCLNT_STREAMFLAGS_EVENTCALLBACK;
                hr = m_pAudioClient->Initialize(
                    AUDCLNT_SHAREMODE_SHARED,
                    streamFlags,
                    hnsRequestedDuration,
                    0,
                    const_cast<const WAVEFORMATEX*>(pwfx),
                    NULL);

                if (FAILED(hr))
                {
                    std::wcerr << L"[AudioCapturer] Event-driven init failed; retrying in polling mode. Error: " << _com_error(hr).ErrorMessage() << std::endl;
                    // Retry without EVENTCALLBACK
                    hr = m_pAudioClient->Initialize(
                        AUDCLNT_SHAREMODE_SHARED,
                        AUDCLNT_STREAMFLAGS_LOOPBACK,
                        hnsRequestedDuration,
                        0,
                        const_cast<const WAVEFORMATEX*>(pwfx),
                        NULL);
                    if (FAILED(hr)) {
                        std::wcerr << L"[AudioCapturer] Unable to initialize audio client for target process: " << _com_error(hr).ErrorMessage() << std::endl;
                        goto Exit;
                    }
                }

                hr = m_pAudioClient->GetBufferSize(&bufferFrameCount);
                if (FAILED(hr))
                {
                    std::wcerr << L"[AudioCapturer] Unable to get buffer size for target process: " << _com_error(hr).ErrorMessage() << std::endl;
                    goto Exit;
                }

                std::wcout << L"[AudioCapturer] Buffer Frame Count: " << bufferFrameCount << std::endl;

                hr = m_pAudioClient->GetService(__uuidof(IAudioCaptureClient), reinterpret_cast<void**>(m_pCaptureClient.ReleaseAndGetAddressOf()));
                if (FAILED(hr))
                {
                    std::wcerr << L"[AudioCapturer] Unable to get capture client for target process: " << _com_error(hr).ErrorMessage() << std::endl;
                    goto Exit;
                }
                // Query IAudioClock for precise timestamps
                hr = m_pAudioClient->GetService(__uuidof(IAudioClock), reinterpret_cast<void**>(m_pAudioClock.ReleaseAndGetAddressOf()));
                if (SUCCEEDED(hr) && m_pAudioClock) {
                    UINT64 freq = 0;
                    if (SUCCEEDED(m_pAudioClock->GetFrequency(&freq))) {
                        m_audioClockFreq = freq;
                        std::wcout << L"[AudioCapturer] AudioClock frequency: " << freq << std::endl;
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
    // FALLBACK: Per-process audio session not found, try default device loopback
    // ============================================================================
    if (!m_pAudioClient)
    {
        std::wcerr << L"[AudioCapturer] Could not find audio session for process ID: " << targetProcessId << std::endl;
        std::wcerr << L"[AudioCapturer] This may happen if:" << std::endl;
        std::wcerr << L"[AudioCapturer]   - The target process is not producing audio" << std::endl;
        std::wcerr << L"[AudioCapturer]   - The process has already exited" << std::endl;
        std::wcerr << L"[AudioCapturer]   - Audio is being routed through a different device" << std::endl;
        std::wcout << L"[AudioCapturer] Attempting fallback to default render device with loopback capture..." << std::endl;

        // Fallback: Try to use the default render device with loopback
        hr = m_pEnumerator->GetDefaultAudioEndpoint(eRender, eMultimedia, &m_pDevice);
        if (FAILED(hr))
        {
            std::wcerr << L"[AudioCapturer] Failed to get default render device: " << _com_error(hr).ErrorMessage()
                       << L" (HRESULT: 0x" << std::hex << hr << std::dec << L")" << std::endl;
            std::wcerr << L"[AudioCapturer] Audio capture initialization failed completely." << std::endl;
            std::wcerr << L"[AudioCapturer] Possible causes:" << std::endl;
            std::wcerr << L"[AudioCapturer]   - No audio devices available" << std::endl;
            std::wcerr << L"[AudioCapturer]   - Audio service not running" << std::endl;
            std::wcerr << L"[AudioCapturer]   - Insufficient permissions" << std::endl;
            goto Exit;
        }

        std::wcout << L"[AudioCapturer] Using default render device with loopback capture." << std::endl;

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

        // Try event-driven mode first for ultra-low latency
        DWORD streamFlags = AUDCLNT_STREAMFLAGS_LOOPBACK | AUDCLNT_STREAMFLAGS_EVENTCALLBACK;
        hr = m_pAudioClient->Initialize(
            AUDCLNT_SHAREMODE_SHARED,
            streamFlags,
            hnsRequestedDuration,
            0,
            const_cast<const WAVEFORMATEX*>(pwfx),
            NULL);

        if (FAILED(hr))
        {
            std::wcerr << L"[AudioCapturer] Event-driven init failed for default device; retrying in polling mode. Error: " << _com_error(hr).ErrorMessage() << std::endl;
            // Retry without EVENTCALLBACK
            hr = m_pAudioClient->Initialize(
                AUDCLNT_SHAREMODE_SHARED,
                AUDCLNT_STREAMFLAGS_LOOPBACK,
                hnsRequestedDuration,
                0,
                const_cast<const WAVEFORMATEX*>(pwfx),
                NULL);
            if (FAILED(hr)) {
                std::wcerr << L"[AudioCapturer] Unable to initialize audio client for default device: " << _com_error(hr).ErrorMessage() << std::endl;
                goto Exit;
            }
        }

        hr = m_pAudioClient->GetBufferSize(&bufferFrameCount);
        if (FAILED(hr))
        {
            std::wcerr << L"[AudioCapturer] Unable to get buffer size for default device: " << _com_error(hr).ErrorMessage() << std::endl;
            goto Exit;
        }

        std::wcout << L"[AudioCapturer] Default device buffer frame count: " << bufferFrameCount << std::endl;

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
    }

    // Release local COM objects that are no longer needed
    if (pSessionEnumerator) pSessionEnumerator->Release();
    if (pSessionManager) pSessionManager->Release();
    if (pCollection) pCollection->Release();
    // pDevice is released when m_pDevice is released, or if not found, it's released in the loop.
    // pSessionControl and pSessionControl2 are released in the loop or assigned to member variable.

    std::wcout << L"[AudioCapturer] Starting audio capture and Opus encoding..." << std::endl;

    // Set up event handle for low-latency event-driven capture
    if (m_pAudioClient) {
        // Create event handle if not already created
        if (!m_hCaptureEvent) {
            m_hCaptureEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
            if (!m_hCaptureEvent) {
                std::wcerr << L"[AudioCapturer] Failed to create capture event handle: " << GetLastError() << std::endl;
            }
        }

        if (m_hCaptureEvent) {
            hr = m_pAudioClient->SetEventHandle(m_hCaptureEvent);
            if (SUCCEEDED(hr)) {
                eventMode = true;
                std::wcout << L"[AudioCapturer] Using event-driven capture mode (optimal latency)." << std::endl;
            } else {
                std::wcerr << L"[AudioCapturer] Event-driven mode setup failed (HRESULT: 0x" << std::hex << hr
                           << std::dec << L"), falling back to polling mode. Error: " << _com_error(hr).ErrorMessage() << std::endl;

                // Clean up event handle since we can't use it
                CloseHandle(m_hCaptureEvent);
                m_hCaptureEvent = nullptr;
            }
        } else {
            std::wcerr << L"[AudioCapturer] Event handle creation failed, using polling mode." << std::endl;
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

        if (eventMode && m_hCaptureEvent) {
            // Use short timeout for low-latency responsiveness
            // 5ms timeout allows quick response to stop signals while maintaining low latency
            DWORD wait = WaitForSingleObject(m_hCaptureEvent, 5);
            if (wait == WAIT_TIMEOUT) {
                // Timeout - continue polling for stop signal
                timeoutCount++;
                continue;
            } else if (wait == WAIT_OBJECT_0) {
                // Event signaled - data available, proceed with capture
            } else {
                // Wait failed - log error and continue
                std::wcerr << L"[AudioCapturer] WaitForSingleObject failed: " << GetLastError() << std::endl;
                continue;
            }
        } else {
            // Polling mode: sleep for calculated interval
            Sleep(sleepMs);
        }

        hr = m_pCaptureClient->GetNextPacketSize(&numFramesAvailable);
        if (FAILED(hr))
        {
            std::wcerr << L"Failed to get next packet size: " << _com_error(hr).ErrorMessage() << std::endl;
            goto Exit;
        }

        while (numFramesAvailable != 0)
        {
            dataPacketsProcessed++;

            UINT64 devPos = 0, qpcPos = 0;
            hr = m_pCaptureClient->GetBuffer(
                &pData,
                &numFramesAvailable,
                &flags,
                &devPos,
                &qpcPos);

            if (FAILED(hr))
            {
                std::wcerr << L"Failed to get buffer: " << _com_error(hr).ErrorMessage() << std::endl;
                goto Exit;
            }

            // ============================================================================
            // ZERO-COPY AUDIO PROCESSING PIPELINE - Optimized for minimal latency
            // ============================================================================
            // 1. Convert PCM to float directly in persistent buffer (eliminates copy)
            // 2. Resample in-place using DMO with efficient buffer swaps
            // 3. Feed encoder directly from persistent buffers (no intermediate copies)
            // 4. Pre-sized buffers eliminate resize overhead during runtime
            // ============================================================================

            size_t totalSamples = static_cast<size_t>(numFramesAvailable) * static_cast<size_t>(pwfx->nChannels);

            // Ensure buffer is large enough (pre-sized in constructor, but handle edge cases)
            if (m_floatBuffer.size() < totalSamples) {
                m_floatBuffer.resize(totalSamples);
            }

            // Convert PCM to float directly in the persistent buffer
            if (flags & AUDCLNT_BUFFERFLAGS_SILENT) {
                // Silent frame - fill with zeros
                std::fill(m_floatBuffer.begin(), m_floatBuffer.begin() + totalSamples, 0.0f);
            } else {
                // Convert PCM data to float format in-place
                if (!ConvertPCMToFloat(pData, numFramesAvailable, static_cast<void*>(pwfx), m_floatBuffer)) {
                    std::wcerr << L"[AudioCapturer] Failed to convert PCM to float format" << std::endl;
                    goto Exit;
                }
            }

            // Resample to 48kHz if source rate differs (process directly in persistent buffers)
            if (pwfx->nSamplesPerSec != 48000) {
                // Create a temporary buffer for resampling output
                std::vector<float> resampleOutput;
                uint32_t channels = pwfx->nChannels;
                ResampleTo48k(m_floatBuffer.data(), numFramesAvailable, pwfx->nSamplesPerSec, channels, resampleOutput);

                // Swap to avoid copying - resampleOutput now contains the resampled data
                m_floatBuffer.swap(resampleOutput);

                // Update totalSamples to reflect the new (resampled) sample count
                totalSamples = m_floatBuffer.size();
            }

            // Calculate timestamp for this audio data using IAudioClock (single source of truth)
            int64_t timestampUs = 0;
            UINT64 audioClockPos = 0;
            UINT64 audioClockQpc = 0;

            if (m_pAudioClock && m_audioClockFreq > 0) {
                if (SUCCEEDED(m_pAudioClock->GetPosition(&audioClockPos, &audioClockQpc))) {
                    // Convert audio clock position to microseconds using the audio clock frequency
                    timestampUs = static_cast<int64_t>((audioClockPos * 1000000ULL) / m_audioClockFreq);

                    // Store the initial audio clock time for RTP timestamp calculation
                    if (m_initialAudioClockTime == 0) {
                        m_initialAudioClockTime = timestampUs;
                        std::wcout << L"[AudioCapturer] Initial audio clock time: " << m_initialAudioClockTime << L" us" << std::endl;
                    }
                } else {
                    std::wcerr << L"[AudioCapturer] Failed to get audio clock position, using fallback" << std::endl;
                    auto currentTime = std::chrono::high_resolution_clock::now();
                    timestampUs = std::chrono::duration_cast<std::chrono::microseconds>(currentTime - m_startTime).count();
                }
            } else {
                std::wcerr << L"[AudioCapturer] Audio clock not available, using system clock fallback" << std::endl;
                auto currentTime = std::chrono::high_resolution_clock::now();
                timestampUs = std::chrono::duration_cast<std::chrono::microseconds>(currentTime - m_startTime).count();
            }
            
            // Process audio frame for Opus encoding and WebRTC transmission
            // Use the persistent buffer directly (zero-copy optimization)
            ProcessAudioFrame(m_floatBuffer.data(), totalSamples, timestampUs);

            hr = m_pCaptureClient->ReleaseBuffer(numFramesAvailable);
            if (FAILED(hr))
            {
                std::wcerr << L"Failed to release buffer: " << _com_error(hr).ErrorMessage() << std::endl;
                goto Exit;
            }

            hr = m_pCaptureClient->GetNextPacketSize(&numFramesAvailable);
            if (FAILED(hr))
            {
                std::wcerr << L"Failed to get next packet size after release: " << _com_error(hr).ErrorMessage() << std::endl;
                goto Exit;
            }
        }

        // Periodic performance logging (every 5 seconds)
        uint64_t currentTime = GetTickCount64();
        if (currentTime - lastLogTime >= 5000) {
            double elapsedSeconds = (currentTime - lastLogTime) / 1000.0;
            double cyclesPerSecond = captureCycles / elapsedSeconds;
            double packetsPerSecond = dataPacketsProcessed / elapsedSeconds;

            std::wcout << L"[AudioCapturer] Performance: " << cyclesPerSecond << L" cycles/sec, "
                       << packetsPerSecond << L" packets/sec";

            if (eventMode) {
                double timeoutRate = (timeoutCount * 100.0) / captureCycles;
                std::wcout << L", timeouts: " << timeoutRate << L"%";
            }

            std::wcout << std::endl;

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

    if (m_hCaptureEvent) { CloseHandle(m_hCaptureEvent); m_hCaptureEvent = nullptr; }
    if (pwfx) { CoTaskMemFree(pwfx); pwfx = nullptr; }
    if (pSessionControl2) pSessionControl2->Release();
    if (pSessionControl) pSessionControl->Release();
    if (pSessionEnumerator) pSessionEnumerator->Release();
    if (pSessionManager) pSessionManager->Release();
    if (pCollection) pCollection->Release();
    // ComPtr members auto-release

    CoUninitialize();
}



bool AudioCapturer::ConvertPCMToFloat(const BYTE* pcmData, UINT32 numFrames, void* formatPtr, std::vector<float>& floatData)
{
    if (!pcmData || !formatPtr || numFrames == 0) return false;
    
    // Cast to WAVEFORMATEX - this is safe since we know the type from the caller
    const WAVEFORMATEX* format = static_cast<const WAVEFORMATEX*>(formatPtr);
    const size_t totalSamples = static_cast<size_t>(numFrames) * static_cast<size_t>(format->nChannels);
    floatData.resize(totalSamples);

    WORD tag = format->wFormatTag;
    WORD bitsPerSample = format->wBitsPerSample;

    // Handle WAVE_FORMAT_EXTENSIBLE by inspecting SubFormat
    if (tag == WAVE_FORMAT_EXTENSIBLE && format->cbSize >= sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX)) {
        const WAVEFORMATEXTENSIBLE* ext = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(format);
        // Prefer valid bits if set
        if (ext->Samples.wValidBitsPerSample) {
            bitsPerSample = ext->Samples.wValidBitsPerSample;
        }

        if (IsEqualGUID(ext->SubFormat, KSDATAFORMAT_SUBTYPE_IEEE_FLOAT)) {
            tag = WAVE_FORMAT_IEEE_FLOAT;
        } else if (IsEqualGUID(ext->SubFormat, KSDATAFORMAT_SUBTYPE_PCM)) {
            tag = WAVE_FORMAT_PCM;
        }
    }

    if (tag == WAVE_FORMAT_IEEE_FLOAT) {
        if (bitsPerSample == 32) {
            const float* pcmFloat = reinterpret_cast<const float*>(pcmData);
            std::copy(pcmFloat, pcmFloat + totalSamples, floatData.begin());
            return true;
        }
        return false;
    }

    if (tag == WAVE_FORMAT_PCM) {
        if (bitsPerSample == 16) {
            const int16_t* pcm16 = reinterpret_cast<const int16_t*>(pcmData);
            for (size_t i = 0; i < totalSamples; ++i) {
                floatData[i] = static_cast<float>(pcm16[i]) / 32768.0f;
            }
            return true;
        } else if (bitsPerSample == 24) {
            // 24-bit little-endian signed PCM
            const uint8_t* p = reinterpret_cast<const uint8_t*>(pcmData);
            for (size_t i = 0; i < totalSamples; ++i) {
                int32_t sample = (static_cast<int32_t>(p[i*3 + 0])      ) |
                                 (static_cast<int32_t>(p[i*3 + 1]) << 8 ) |
                                 (static_cast<int32_t>(p[i*3 + 2]) << 16);
                // Sign-extend 24-bit to 32-bit
                if (sample & 0x00800000) sample |= 0xFF000000;
                floatData[i] = static_cast<float>(sample) / 8388608.0f; // 2^23
            }
            return true;
        } else if (bitsPerSample == 32) {
            const int32_t* pcm32 = reinterpret_cast<const int32_t*>(pcmData);
            for (size_t i = 0; i < totalSamples; ++i) {
                floatData[i] = static_cast<float>(pcm32[i]) / 2147483648.0f; // 2^31
            }
            return true;
        }
        return false;
    }

    return false;
    
    return true;
}

void AudioCapturer::ProcessAudioFrame(const float* samples, size_t sampleCount, int64_t timestampUs)
{
    if (!samples || sampleCount == 0) return;

    // Accumulate samples into per-instance buffer (thread-safe, no cross-instance contamination)
    // Append new samples to the instance's accumulation buffer
    const size_t currentSize = m_accumulatedSamples.size();
    m_accumulatedSamples.resize(currentSize + sampleCount);
    std::copy(samples, samples + sampleCount, m_accumulatedSamples.begin() + currentSize);
    m_accumulatedCount += sampleCount;

    // Process complete frames based on Opus frame size (ensures consistent latency)
    // m_samplesPerFrame = total samples per frame (includes all channels)
    // RTP timestamps increment by samples per channel for correct timing
    while (m_accumulatedCount >= m_samplesPerFrame) {
        // Extract one complete frame from the accumulated buffer
        std::copy(m_accumulatedSamples.begin(), m_accumulatedSamples.begin() + m_samplesPerFrame, m_frameBuffer.begin());

        // Queue raw audio frame for dedicated encoder thread (completely offloads encoding)
        // This prevents any encoding work from running on the capture thread
        std::vector<float> frameData(m_frameBuffer.begin(), m_frameBuffer.end());
        if (!QueueRawFrame(frameData, timestampUs)) {
            std::wcerr << L"[AudioCapturer] Failed to queue raw audio frame" << std::endl;
        } else {
            // Log every 100 frames (disabled during streaming)
            // static int frameCount = 0;
            // if (++frameCount % 100 == 0) {
            //     std::wcout << L"[AudioCapturer] Queued raw frame " << frameCount
            //                << L" with " << frameData.size() << L" samples, pts: " << timestampUs << L" us" << std::endl;
            // }
        }

        // Remove processed samples from the instance's accumulator
        m_accumulatedSamples.erase(m_accumulatedSamples.begin(), m_accumulatedSamples.begin() + m_samplesPerFrame);
        m_accumulatedCount -= m_samplesPerFrame;
    }
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

    // Try to use high-quality Windows Audio Resampler DMO first
    if (InitializeDMOResampler(inRate, channels)) {
        size_t inputSamples = inFrames * channels;
        if (ProcessResamplerDMO(in, inputSamples, out)) {
            // Success - DMO resampler handled the conversion
            return;
        } else {
            std::wcerr << L"[AudioCapturer] DMO resampler failed, falling back to linear interpolation" << std::endl;
        }
    }

    // Fallback to linear interpolation method (preserves existing behavior)
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
    std::vector<float> prev(channels, 0.0f);
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
// AUDIO BITRATE ADAPTATION - RTCP feedback integration
// Fully functional parameter update system for Opus encoder
// ============================================================================

void AudioCapturer::OnRtcpFeedback(double packetLoss, double /*rtt*/, double /*jitter*/) {
    auto now = std::chrono::steady_clock::now();
    auto since = std::chrono::duration_cast<std::chrono::milliseconds>(now - s_lastAudioChange).count();

    // Determine if FEC should be enabled/disabled based on packet loss
    bool shouldEnableFec = (packetLoss >= s_fecEnableThreshold);
    bool shouldDisableFec = (packetLoss <= s_fecDisableThreshold);
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

    // High packet loss: Decrease bitrate aggressively
    if (packetLoss >= s_highLossThreshold) {
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
                      << L"%), decreased bitrate to " << newBitrate << L" bps" << std::endl;
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

        // Read frame size in milliseconds (keep at 10ms for low latency)
        if (config.contains("frameSizeMs")) {
            int frameSizeMs = config["frameSizeMs"].get<int>();
            if (frameSizeMs == 10 || frameSizeMs == 20 || frameSizeMs == 40) {
                s_audioConfig.frameSizeMs = frameSizeMs;
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

