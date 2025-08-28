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
#include <wrl/client.h>
#include "OpusEncoder.h"

class AudioCapturer
{
public:
    AudioCapturer();
    ~AudioCapturer();

    bool StartCapture(DWORD processId);
    void StopCapture();

private:
    void CaptureThread(DWORD processId);
    bool ConvertPCMToFloat(const BYTE* pcmData, UINT32 numFrames, void* format, std::vector<float>& floatData);
    void ProcessAudioFrame(const float* samples, size_t sampleCount, int64_t timestampUs);
    void ResampleTo48k(const float* in, size_t inFrames, uint32_t inRate, uint32_t channels, std::vector<float>& out);

    // DMO Resampler methods for high-quality audio resampling
    bool InitializeDMOResampler(uint32_t inputSampleRate, uint32_t inputChannels);
    bool ProcessResamplerDMO(const float* inputData, size_t inputSamples, std::vector<float>& outputData);
    void CleanupDMOResampler();

    // Test and validation methods
    void TestResamplerQuality(uint32_t testSampleRate, uint32_t testChannels);

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

    // Per-instance audio frame accumulation (replaces static variables)
    std::vector<float> m_accumulatedSamples;
    size_t m_accumulatedCount = 0;
    
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

    // MMCSS (Multimedia Class Scheduler Service) for thread prioritization
    HANDLE m_hMmcssTask = nullptr;
    DWORD m_mmcssTaskIndex = 0;

    // Windows Audio Resampler DMO for high-quality resampling
    Microsoft::WRL::ComPtr<IMediaObject> m_audioResamplerDMO;
    Microsoft::WRL::ComPtr<IMediaBuffer> m_inputBuffer;
    Microsoft::WRL::ComPtr<IMediaBuffer> m_outputBuffer;
    DMO_MEDIA_TYPE m_inputMediaType = {};
    DMO_MEDIA_TYPE m_outputMediaType = {};
    bool m_resamplerInitialized = false;
    uint32_t m_currentInputSampleRate = 0;
    uint32_t m_currentInputChannels = 0;

    // Persistent audio float buffer to avoid per-packet allocations
    std::vector<float> m_floatBuffer;

    // Simple resampler state for linear interpolation between callbacks
    std::vector<float> m_resampleRemainder; // per-channel remainder sample
    double m_resamplePhase = 0.0;
    uint32_t m_lastInputRate = 0;
};
