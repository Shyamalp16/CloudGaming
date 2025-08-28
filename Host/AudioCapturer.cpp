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
    m_samplesPerFrame(960), // 20ms at 48kHz (will be updated based on Opus settings)
    m_accumulatedCount(0)   // Initialize accumulation counter
{
    // Initialize Opus encoder with optimized settings for low-latency gaming
    m_opusEncoder = std::make_unique<OpusEncoderWrapper>();

    // Pre-allocate accumulation buffer to reduce reallocations during runtime
    // Reserve space for multiple frames to handle bursty audio data
    m_accumulatedSamples.reserve(m_samplesPerFrame * 4); // Reserve space for 4 frames
}

AudioCapturer::~AudioCapturer()
{
    StopCapture();
}

bool AudioCapturer::StartCapture(DWORD processId)
{
    m_stopCapture = false;
    
    // Initialize Opus encoder with optimized settings for low-latency gaming
    OpusEncoderWrapper::Settings settings;
    settings.sampleRate = 48000;        // 48 kHz
    settings.channels = 2;              // Stereo for game/media audio
    settings.frameSize = 480;           // 10ms frames at 48kHz
    settings.bitrate = 64000;           // 64 kbps for stereo
    settings.complexity = 6;            // Balance between quality and CPU usage
    settings.useVbr = true;             // Variable bitrate
    settings.constrainedVbr = true;     // Constrain VBR peaks
    settings.enableFec = true;          // Forward Error Correction for packet loss
    settings.expectedLossPerc = 10;     // Expect 10% packet loss
    settings.enableDtx = false;         // Disable DTX for continuous game/media audio
    settings.application = 2049;        // OPUS_APPLICATION_AUDIO for music/gaming
    
    if (!m_opusEncoder->initialize(settings)) {
        std::wcerr << L"[AudioCapturer] Failed to initialize Opus encoder" << std::endl;
        return false;
    }
    
    // Initialize timing
    m_startTime = std::chrono::high_resolution_clock::now();
    m_nextFrameTime = 0;
    m_rtpTimestamp = 0;
    m_samplesPerFrame = settings.frameSize * settings.channels;
    m_frameBuffer.resize(m_samplesPerFrame);
    
    std::wcout << L"[AudioCapturer] Initialized Opus encoder: " 
               << settings.sampleRate << L"Hz, " 
               << settings.channels << L" channels, "
               << settings.frameSize << L" samples/frame, "
               << settings.bitrate << L" bps" << std::endl;
    
    m_captureThread = std::thread(&AudioCapturer::CaptureThread, this, processId);
    return true;
}

void AudioCapturer::StopCapture()
{
    m_stopCapture = true;
    if (m_captureThread.joinable()) { m_captureThread.join(); }

    // Clear accumulation buffers to ensure clean state for next capture session
    // This prevents any leftover data from affecting subsequent captures
    m_accumulatedSamples.clear();
    m_accumulatedCount = 0;

    // Reset timing state
    m_initialAudioClockTime = 0;
    m_nextFrameTime = 0;
    m_rtpTimestamp = 0;
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
    // Opus uses 10ms frames (480 samples at 48kHz) for low-latency encoding
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
            sleepMs = std::max<DWORD>(1, std::min<DWORD>(bufferMs / 4, OPUS_FRAME_MS));
        } else {
            // Large buffer: use frame-aligned polling interval
            sleepMs = OPUS_FRAME_MS;
        }

        // Ensure we never exceed Opus frame duration for optimal alignment
        sleepMs = std::min<DWORD>(sleepMs, OPUS_FRAME_MS);

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

            // Convert PCM to float and process for Opus encoding (reuse persistent buffer)
            std::vector<float> processedSamples;
            const size_t totalSamples = static_cast<size_t>(numFramesAvailable) * static_cast<size_t>(pwfx->nChannels);
            if (m_floatBuffer.size() < totalSamples) m_floatBuffer.resize(totalSamples);
            if (flags & AUDCLNT_BUFFERFLAGS_SILENT) {
                std::fill(m_floatBuffer.begin(), m_floatBuffer.begin() + totalSamples, 0.0f);
            } else {
                if (!ConvertPCMToFloat(pData, numFramesAvailable, static_cast<void*>(pwfx), m_floatBuffer)) {
                    std::wcerr << L"[AudioCapturer] Failed to convert PCM to float format" << std::endl;
                    goto Exit;
                }
            }
            processedSamples.assign(m_floatBuffer.begin(), m_floatBuffer.begin() + totalSamples);

            // Resample to 48kHz if source rate differs
            if (pwfx->nSamplesPerSec != 48000) {
                std::vector<float> resampled;
                uint32_t channels = pwfx->nChannels;
                ResampleTo48k(processedSamples.data(), numFramesAvailable, pwfx->nSamplesPerSec, channels, resampled);
                processedSamples.swap(resampled);
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
            ProcessAudioFrame(processedSamples.data(), processedSamples.size(), timestampUs);

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
    // m_samplesPerFrame is derived from Opus settings (e.g., 480 for 10ms at 48kHz, 960 for 20ms)
    while (m_accumulatedCount >= m_samplesPerFrame) {
        // Extract one complete frame from the accumulated buffer
        std::copy(m_accumulatedSamples.begin(), m_accumulatedSamples.begin() + m_samplesPerFrame, m_frameBuffer.begin());

        // Encode with Opus using the extracted frame
        std::vector<uint8_t> encodedData;
        if (m_opusEncoder->encodeFrame(m_frameBuffer.data(), encodedData)) {
            // Calculate RTP timestamp using the provided timestamp (48kHz clock)
            // Convert microseconds to RTP timestamp units (48kHz = 48000 ticks per second)
            uint32_t rtpTimestamp = 0;
            if (timestampUs > 0 && m_initialAudioClockTime > 0) {
                // Calculate RTP timestamp relative to initial audio clock time
                int64_t relativeTimeUs = timestampUs - m_initialAudioClockTime;
                rtpTimestamp = static_cast<uint32_t>((relativeTimeUs * 48LL) / 1000LL);
            } else {
                // Fallback: increment RTP timestamp
                m_rtpTimestamp += static_cast<uint32_t>(m_samplesPerFrame);
                rtpTimestamp = m_rtpTimestamp;
            }

            // Send to WebRTC with the original timestamp (pts) for backward compatibility
            // In the future, we could send RTP timestamp directly, but for now we maintain the existing interface
            int result = sendAudioPacket(encodedData.data(), static_cast<int>(encodedData.size()), timestampUs);
            if (result != 0) {
                std::wcerr << L"[AudioCapturer] Failed to send audio packet to WebRTC. Error: " << result << std::endl;
            } else {
                // Log every 100 frames (disabled during streaming)
                // static int frameCount = 0;
                // if (++frameCount % 100 == 0) {
                //     std::wcout << L"[AudioCapturer] Sent Opus frame " << frameCount << L", size: " << encodedData.size()
                //                << L" bytes, RTP timestamp: " << rtpTimestamp << L", pts: " << timestampUs << L" us" << std::endl;
                // }
            }
        } else {
            std::wcerr << L"[AudioCapturer] Failed to encode audio frame with Opus" << std::endl;
        }

        // Remove processed samples from the instance's accumulator
        m_accumulatedSamples.erase(m_accumulatedSamples.begin(), m_accumulatedSamples.begin() + m_samplesPerFrame);
        m_accumulatedCount -= m_samplesPerFrame;
    }
}



void AudioCapturer::ResampleTo48k(const float* in, size_t inFrames, uint32_t inRate, uint32_t channels, std::vector<float>& out)
{
    if (inRate == 0 || channels == 0) { out.clear(); return; }
    if (inRate == 48000) {
        out.assign(in, in + inFrames * channels);
        return;
    }

    // Linear interpolation per channel with persistent phase
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

