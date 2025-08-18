#pragma once

#include <Windows.h>
#include <string>
#include <thread>
#include <atomic>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <audiopolicy.h>
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
    
    std::thread m_captureThread;
    std::atomic<bool> m_stopCapture;
    
    // Opus encoder
    std::unique_ptr<OpusEncoderWrapper> m_opusEncoder;
    std::vector<float> m_frameBuffer;
    size_t m_samplesPerFrame;
    
    // Timing for 20ms frames
    std::chrono::high_resolution_clock::time_point m_startTime;
    int64_t m_nextFrameTime;
    uint32_t m_rtpTimestamp;
    
    // COM interfaces (smart pointers)
    Microsoft::WRL::ComPtr<IMMDeviceEnumerator> m_pEnumerator;
    Microsoft::WRL::ComPtr<IMMDevice> m_pDevice;
    Microsoft::WRL::ComPtr<IAudioClient> m_pAudioClient;
    Microsoft::WRL::ComPtr<IAudioCaptureClient> m_pCaptureClient;
    Microsoft::WRL::ComPtr<IAudioSessionControl2> m_pSessionControl2;

    // Event-driven capture
    HANDLE m_hCaptureEvent = nullptr;
};
