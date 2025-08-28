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
    m_samplesPerFrame(960) // 20ms at 48kHz
{
    // Initialize Opus encoder with optimized settings for low-latency gaming
    m_opusEncoder = std::make_unique<OpusEncoderWrapper>();
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

    hr = CoInitialize(NULL);
    if (FAILED(hr))
    {
        std::wcerr << L"Unable to initialize COM in render thread: " << _com_error(hr).ErrorMessage() << std::endl;
        return;
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

    if (!m_pAudioClient)
    {
        std::wcerr << L"[AudioCapturer] Could not find audio session for process ID: " << targetProcessId << std::endl;
        goto Exit;
    }

    // Release local COM objects that are no longer needed
    if (pSessionEnumerator) pSessionEnumerator->Release();
    if (pSessionManager) pSessionManager->Release();
    if (pCollection) pCollection->Release();
    // pDevice is released when m_pDevice is released, or if not found, it's released in the loop.
    // pSessionControl and pSessionControl2 are released in the loop or assigned to member variable.

    std::wcout << L"[AudioCapturer] Starting audio capture and Opus encoding..." << std::endl;

    // Set up event handle if event-driven mode was enabled
    if (m_pAudioClient) {
        // If the client was initialized with EVENTCALLBACK, SetEventHandle will succeed
        if (!m_hCaptureEvent) {
            m_hCaptureEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
        }
        if (m_hCaptureEvent) {
            hr = m_pAudioClient->SetEventHandle(m_hCaptureEvent);
            if (SUCCEEDED(hr)) {
                eventMode = true;
                std::wcout << L"[AudioCapturer] Using event-driven capture." << std::endl;
            }
        }
    }

    hr = m_pAudioClient->Start();  // Start recording.
    if (FAILED(hr))
    {
        std::wcerr << L"Unable to start recording: " << _com_error(hr).ErrorMessage() << std::endl;
        goto Exit;
    }

    // Derive a reasonable polling interval from buffer size (~quarter-buffer)
    if (pwfx && pwfx->nSamplesPerSec) {
        DWORD bufferMs = (bufferFrameCount * 1000u) / pwfx->nSamplesPerSec;
        sleepMs = std::max<DWORD>(1, bufferMs / 4);
    }

    while (m_stopCapture == false)
    {
        if (eventMode && m_hCaptureEvent) {
            DWORD wait = WaitForSingleObject(m_hCaptureEvent, 50); // short timeout to be responsive to stop
            if (wait != WAIT_OBJECT_0) {
                continue; // timeout or error; loop back
            }
        } else {
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

            // Calculate timestamp for this audio data using IAudioClock if available
            int64_t timestampUs = 0;
            if (m_pAudioClock && m_audioClockFreq > 0) {
                UINT64 pos = 0;
                UINT64 qpc = 0;
                if (SUCCEEDED(m_pAudioClock->GetPosition(&pos, &qpc))) {
                    timestampUs = static_cast<int64_t>((pos * 1000000ULL) / m_audioClockFreq);
                }
            }
            if (timestampUs == 0) {
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
    }

    // Cleanup and shutdown

Exit:
    std::wcout << L"[AudioCapturer] Audio capture stopped." << std::endl;

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
    
    // Accumulate samples into frame buffer
    static std::vector<float> accumulatedSamples;
    static size_t accumulatedCount = 0;
    
    // Append new samples
    const size_t currentSize = accumulatedSamples.size();
    accumulatedSamples.resize(currentSize + sampleCount);
    std::copy(samples, samples + sampleCount, accumulatedSamples.begin() + currentSize);
    accumulatedCount += sampleCount;
    
    // Process complete 20ms frames (960 samples for mono at 48kHz)
    while (accumulatedCount >= m_samplesPerFrame) {
        // Extract one frame
        std::copy(accumulatedSamples.begin(), accumulatedSamples.begin() + m_samplesPerFrame, m_frameBuffer.begin());
        
        // Encode with Opus
        std::vector<uint8_t> encodedData;
        if (m_opusEncoder->encodeFrame(m_frameBuffer.data(), encodedData)) {
            // Calculate RTP timestamp (48kHz clock, increment by 960 per 20ms frame)
            m_rtpTimestamp += 960;
            
            // Calculate microsecond timestamp for this frame
            auto currentTime = std::chrono::high_resolution_clock::now();
            auto elapsedTime = std::chrono::duration_cast<std::chrono::microseconds>(currentTime - m_startTime);
            int64_t frameTimestampUs = elapsedTime.count();
            
            // Send to WebRTC
            int result = sendAudioPacket(encodedData.data(), static_cast<int>(encodedData.size()), frameTimestampUs);
            if (result != 0) {
                std::wcerr << L"[AudioCapturer] Failed to send audio packet to WebRTC. Error: " << result << std::endl;
            } else {
                // Log every 100 frames (disabled during streaming)
                // static int frameCount = 0;
                // if (++frameCount % 100 == 0) {
                //     std::wcout << L"[AudioCapturer] Sent Opus frame " << frameCount << L", size: " << encodedData.size() 
                //                << L" bytes, RTP timestamp: " << m_rtpTimestamp << std::endl;
                // }
            }
        } else {
            std::wcerr << L"[AudioCapturer] Failed to encode audio frame with Opus" << std::endl;
        }
        
        // Remove processed samples from accumulator
        accumulatedSamples.erase(accumulatedSamples.begin(), accumulatedSamples.begin() + m_samplesPerFrame);
        accumulatedCount -= m_samplesPerFrame;
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

