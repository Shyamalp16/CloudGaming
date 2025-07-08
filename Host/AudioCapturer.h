#pragma once

#include <Windows.h>
#include <string>
#include <thread>
#include <atomic>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <audiopolicy.h>

class AudioCapturer
{
public:
    AudioCapturer();
    ~AudioCapturer();

    bool StartCapture(const std::wstring& outputFilePath, DWORD processId);
    void StopCapture();

private:
    void CaptureThread(DWORD processId);
    bool WriteWavHeader(HANDLE hFile, WAVEFORMATEX* pwfx, DWORD dataSize);
    bool FixWavHeader(HANDLE hFile, DWORD dataSize, WORD cbSize);

    std::thread m_captureThread;
    std::atomic<bool> m_stopCapture;
    std::wstring m_outputFilePath;

    // COM interfaces
    IMMDeviceEnumerator* m_pEnumerator = nullptr;
    IMMDevice* m_pDevice = nullptr;
    IAudioClient* m_pAudioClient = nullptr;
    IAudioCaptureClient* m_pCaptureClient = nullptr;
    IAudioSessionControl2* m_pSessionControl2 = nullptr;
};
