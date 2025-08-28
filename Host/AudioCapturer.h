#pragma once

#include <Windows.h>
#include <string>
#include <thread>
#include <atomic>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <audiopolicy.h>
#include <avrt.h>  // MMCSS (Multimedia Class Scheduler Service)
#include <vector>
#include <memory>
#include <chrono>
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
    
    std::thread m_captureThread;
    std::atomic<bool> m_stopCapture;
    
    // Opus encoder
    std::unique_ptr<OpusEncoderWrapper> m_opusEncoder;
    std::vector<float> m_frameBuffer;
    size_t m_samplesPerFrame;

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

    // Persistent audio float buffer to avoid per-packet allocations
    std::vector<float> m_floatBuffer;

    // Simple resampler state for linear interpolation between callbacks
    std::vector<float> m_resampleRemainder; // per-channel remainder sample
    double m_resamplePhase = 0.0;
    uint32_t m_lastInputRate = 0;
};
