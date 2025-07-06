#include "AudioCapturer.h"
#include <iostream>
#include <fstream>
#include <comdef.h>

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
    m_pCaptureClient(nullptr)
{
}

AudioCapturer::~AudioCapturer()
{
    StopCapture();
}

bool AudioCapturer::StartCapture(const std::wstring& outputFilePath)
{
    m_outputFilePath = outputFilePath;
    m_stopCapture = false;

    m_captureThread = std::thread(&AudioCapturer::CaptureThread, this);

    return true;
}

void AudioCapturer::StopCapture()
{
    m_stopCapture = true;
    if (m_captureThread.joinable())
    {
        m_captureThread.join();
    }
}

void AudioCapturer::CaptureThread()
{
    HRESULT hr;
    REFERENCE_TIME hnsRequestedDuration = REFTIMES_PER_SEC;
    REFERENCE_TIME hnsActualDuration;
    UINT32 bufferFrameCount;
    UINT32 numFramesAvailable;
    BYTE* pData;
    DWORD flags;
    WAVEFORMATEX* pwfx = NULL;
    HANDLE hFile = INVALID_HANDLE_VALUE;
    DWORD dataSize = 0;

    hr = CoInitialize(NULL);

    if (FAILED(hr))
    {
        std::wcerr << L"Unable to initialize COM in render thread: " << _com_error(hr).ErrorMessage() << std::endl;
        return;
    }

    const CLSID CLSID_MMDeviceEnumerator = __uuidof(MMDeviceEnumerator);
    const IID IID_IMMDeviceEnumerator = __uuidof(IMMDeviceEnumerator);
    hr = CoCreateInstance(
        CLSID_MMDeviceEnumerator, NULL,
        CLSCTX_ALL, IID_IMMDeviceEnumerator,
        (void**)&m_pEnumerator);

    if (FAILED(hr))
    {
        std::wcerr << L"Unable to instantiate device enumerator: " << _com_error(hr).ErrorMessage() << std::endl;
        goto Exit;
    }

    hr = m_pEnumerator->GetDefaultAudioEndpoint(
        eRender, eConsole, &m_pDevice);

    if (FAILED(hr))
    {
        std::wcerr << L"Unable to get default audio endpoint: " << _com_error(hr).ErrorMessage() << std::endl;
        goto Exit;
    }

    hr = m_pDevice->Activate(
        __uuidof(IAudioClient), CLSCTX_ALL,
        NULL, (void**)&m_pAudioClient);

    if (FAILED(hr))
    {
        std::wcerr << L"Unable to activate audio client: " << _com_error(hr).ErrorMessage() << std::endl;
        goto Exit;
    }

    hr = m_pAudioClient->GetMixFormat(&pwfx);

    if (FAILED(hr))
    {
        std::wcerr << L"Unable to get mix format: " << _com_error(hr).ErrorMessage() << std::endl;
        goto Exit;
    }

    hr = m_pAudioClient->Initialize(
        AUDCLNT_SHAREMODE_SHARED,
        AUDCLNT_STREAMFLAGS_LOOPBACK,
        hnsRequestedDuration,
        0,
        pwfx,
        NULL);

    if (FAILED(hr))
    {
        std::wcerr << L"Unable to initialize audio client: " << _com_error(hr).ErrorMessage() << std::endl;
        goto Exit;
    }

    // Get the size of the allocated buffer.
    hr = m_pAudioClient->GetBufferSize(&bufferFrameCount);

    if (FAILED(hr))
    {
        std::wcerr << L"Unable to get buffer size: " << _com_error(hr).ErrorMessage() << std::endl;
        goto Exit;
    }

    hr = m_pAudioClient->GetService(
        __uuidof(IAudioCaptureClient),
        (void**)&m_pCaptureClient);

    if (FAILED(hr))
    {
        std::wcerr << L"Unable to get capture client: " << _com_error(hr).ErrorMessage() << std::endl;
        goto Exit;
    }

    hFile = CreateFile(m_outputFilePath.c_str(), GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile == INVALID_HANDLE_VALUE)
    {
        std::wcerr << L"Unable to create output file." << std::endl;
        goto Exit;
    }

    if (!WriteWavHeader(hFile, pwfx, 0))
    {
        std::wcerr << L"Unable to write WAV header." << std::endl;
        goto Exit;
    }

    hnsActualDuration = (double)REFTIMES_PER_SEC * bufferFrameCount / pwfx->nSamplesPerSec;

    hr = m_pAudioClient->Start();  // Start recording.

    if (FAILED(hr))
    {
        std::wcerr << L"Unable to start recording: " << _com_error(hr).ErrorMessage() << std::endl;
        goto Exit;
    }

    while (m_stopCapture == false)
    {
        Sleep(hnsActualDuration / REFTIMES_PER_MILLISEC / 2);

        hr = m_pCaptureClient->GetNextPacketSize(&numFramesAvailable);

        while (numFramesAvailable != 0)
        {
            hr = m_pCaptureClient->GetBuffer(
                &pData,
                &numFramesAvailable,
                &flags,
                NULL,
                NULL);

            if (FAILED(hr))
            {
                std::wcerr << L"Failed to get buffer: " << _com_error(hr).ErrorMessage() << std::endl;
                goto Exit;
            }

            if (flags & AUDCLNT_BUFFERFLAGS_SILENT)
            {
                pData = NULL;  // Tell CopyData to write silence.
            }

            DWORD bytesToWrite = numFramesAvailable * pwfx->nBlockAlign;
            DWORD bytesWritten;
            if (!WriteFile(hFile, pData, bytesToWrite, &bytesWritten, NULL))
            {
                std::wcerr << L"Failed to write to file." << std::endl;
                goto Exit;
            }
            dataSize += bytesWritten;

            hr = m_pCaptureClient->ReleaseBuffer(numFramesAvailable);
            if (FAILED(hr))
            {
                std::wcerr << L"Failed to release buffer: " << _com_error(hr).ErrorMessage() << std::endl;
                goto Exit;
            }

            hr = m_pCaptureClient->GetNextPacketSize(&numFramesAvailable);
        }
    }

    if (!FixWavHeader(hFile, dataSize, pwfx->cbSize))
    {
        std::wcerr << L"Failed to fix WAV header." << std::endl;
    }

Exit:
    if (hFile != INVALID_HANDLE_VALUE)
    {
        CloseHandle(hFile);
    }

    CoTaskMemFree(pwfx);
    if (m_pEnumerator) m_pEnumerator->Release();
    if (m_pDevice) m_pDevice->Release();
    if (m_pAudioClient) m_pAudioClient->Release();
    if (m_pCaptureClient) m_pCaptureClient->Release();

    CoUninitialize();
}

bool AudioCapturer::WriteWavHeader(HANDLE hFile, WAVEFORMATEX* pwfx, DWORD dataSize)
{
    DWORD bytesWritten;
    DWORD dwRiffSize = dataSize + 36;

    // RIFF chunk
    if (!WriteFile(hFile, "RIFF", 4, &bytesWritten, NULL)) return false;
    if (!WriteFile(hFile, &dwRiffSize, 4, &bytesWritten, NULL)) return false;
    if (!WriteFile(hFile, "WAVE", 4, &bytesWritten, NULL)) return false;

    // FORMAT chunk
    if (!WriteFile(hFile, "fmt ", 4, &bytesWritten, NULL)) return false;
    DWORD dwFmtSize = sizeof(WAVEFORMATEX) + pwfx->cbSize;
    if (!WriteFile(hFile, &dwFmtSize, 4, &bytesWritten, NULL)) return false;
    if (!WriteFile(hFile, pwfx, sizeof(WAVEFORMATEX) + pwfx->cbSize, &bytesWritten, NULL)) return false;

    // DATA chunk
    if (!WriteFile(hFile, "data", 4, &bytesWritten, NULL)) return false;
    if (!WriteFile(hFile, &dataSize, 4, &bytesWritten, NULL)) return false;

    return true;
}

bool AudioCapturer::FixWavHeader(HANDLE hFile, DWORD dataSize, WORD cbSize)
{
    DWORD bytesWritten;
    // dwRiffSize = 4 (WAVE) + 4 (fmt ID) + 4 (fmt size) + (18 + cbSize) (WAVEFORMATEX) + 4 (data ID) + 4 (data size) + dataSize
    DWORD dwRiffSize = 42 + cbSize + dataSize;

    // Update RIFF chunk size
    SetFilePointer(hFile, 4, NULL, FILE_BEGIN);
    if (!WriteFile(hFile, &dwRiffSize, 4, &bytesWritten, NULL)) return false;

    // dataSize offset = 4 (RIFF) + 4 (dwRiffSize) + 4 (WAVE) + 4 (fmt ID) + 4 (fmt size) + (18 + cbSize) (WAVEFORMATEX) + 4 (data ID)
    DWORD dataSizeOffset = 42 + cbSize;
    SetFilePointer(hFile, dataSizeOffset, NULL, FILE_BEGIN);
    if (!WriteFile(hFile, &dataSize, 4, &bytesWritten, NULL)) return false;

    return true;
}

