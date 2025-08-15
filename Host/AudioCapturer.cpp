#include "AudioCapturer.h"
#include <iostream>
#include <fstream>
#include <comdef.h>
#include <vector>

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

bool AudioCapturer::StartCapture(const std::wstring& outputFilePath, DWORD processId)
{
    m_outputFilePath = outputFilePath;
    m_stopCapture = false;
    // Store the process ID for the capture thread to use
    // You'll need to add a member variable for this in AudioCapturer.h
    // For now, let's assume you have a member `m_targetProcessId`
    // m_targetProcessId = processId; 
    // For now, I'll pass it as an argument to the thread function
    m_captureThread = std::thread(&AudioCapturer::CaptureThread, this, processId);

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

void AudioCapturer::CaptureThread(DWORD targetProcessId)
{
    HRESULT hr;
    REFERENCE_TIME hnsRequestedDuration = REFTIMES_PER_SEC;
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
        (void**)&m_pEnumerator);

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
                m_pDevice = pDevice; // Keep a reference to the device
                m_pDevice->AddRef(); // Increment ref count as we're keeping it
                m_pSessionControl2 = pSessionControl2; // Keep a reference to the session control
                m_pSessionControl2->AddRef(); // Increment ref count as we're keeping it
                
                hr = m_pDevice->Activate(__uuidof(IAudioClient), CLSCTX_ALL, NULL, (void**)&m_pAudioClient);
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

                hr = m_pAudioClient->Initialize(
                    AUDCLNT_SHAREMODE_SHARED,
                    AUDCLNT_STREAMFLAGS_LOOPBACK, // capture render (what-you-hear)
                    hnsRequestedDuration,
                    0,
                    pwfx,
                    NULL);

                if (FAILED(hr))
                {
                    std::wcerr << L"[AudioCapturer] Unable to initialize audio client for target process: " << _com_error(hr).ErrorMessage() << std::endl;
                    goto Exit;
                }

                hr = m_pAudioClient->GetBufferSize(&bufferFrameCount);
                if (FAILED(hr))
                {
                    std::wcerr << L"[AudioCapturer] Unable to get buffer size for target process: " << _com_error(hr).ErrorMessage() << std::endl;
                    goto Exit;
                }

                std::wcout << L"[AudioCapturer] Buffer Frame Count: " << bufferFrameCount << std::endl;

                hr = m_pAudioClient->GetService(__uuidof(IAudioCaptureClient), (void**)&m_pCaptureClient);
                if (FAILED(hr))
                {
                    std::wcerr << L"[AudioCapturer] Unable to get capture client for target process: " << _com_error(hr).ErrorMessage() << std::endl;
                    goto Exit;
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

    hr = m_pAudioClient->Start();  // Start recording.
    if (FAILED(hr))
    {
        std::wcerr << L"Unable to start recording: " << _com_error(hr).ErrorMessage() << std::endl;
        goto Exit;
    }

    // Derive a reasonable polling interval from buffer size (~quarter-buffer)
    DWORD sleepMs = 10;
    if (pwfx && pwfx->nSamplesPerSec) {
        DWORD bufferMs = (bufferFrameCount * 1000u) / pwfx->nSamplesPerSec;
        sleepMs = std::max<DWORD>(1, bufferMs / 4);
    }

    while (m_stopCapture == false)
    {
        Sleep(sleepMs);

        hr = m_pCaptureClient->GetNextPacketSize(&numFramesAvailable);
        if (FAILED(hr))
        {
            std::wcerr << L"Failed to get next packet size: " << _com_error(hr).ErrorMessage() << std::endl;
            goto Exit;
        }

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

            DWORD bytesToWrite = numFramesAvailable * pwfx->nBlockAlign;
            DWORD bytesWritten;
            if (flags & AUDCLNT_BUFFERFLAGS_SILENT) {
                // Write explicit silence when device reports silence
                static std::vector<BYTE> zeroBuf;
                if (zeroBuf.size() < bytesToWrite) zeroBuf.assign(bytesToWrite, 0);
                if (!WriteFile(hFile, zeroBuf.data(), bytesToWrite, &bytesWritten, NULL)) {
                    std::wcerr << L"Failed to write silent buffer to file." << std::endl;
                    goto Exit;
                }
            } else {
                if (!WriteFile(hFile, pData, bytesToWrite, &bytesWritten, NULL))
                {
                    std::wcerr << L"Failed to write to file." << std::endl;
                    goto Exit;
                }
            }
            dataSize += bytesWritten;

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
    if (pSessionControl2) pSessionControl2->Release();
    if (pSessionControl) pSessionControl->Release();
    if (pSessionEnumerator) pSessionEnumerator->Release();
    if (pSessionManager) pSessionManager->Release();
    if (pCollection) pCollection->Release();
    if (m_pEnumerator) m_pEnumerator->Release();
    if (m_pDevice) m_pDevice->Release();
    if (m_pAudioClient) m_pAudioClient->Release();
    if (m_pCaptureClient) m_pCaptureClient->Release();
    if (m_pSessionControl2) m_pSessionControl2->Release();

    CoUninitialize();
}

bool AudioCapturer::WriteWavHeader(HANDLE hFile, WAVEFORMATEX* pwfx, DWORD dataSize)
{
    DWORD bytesWritten;
    DWORD fmtSize = sizeof(WAVEFORMATEX) + pwfx->cbSize; // 16 for PCM, 18+cbSize otherwise
    DWORD dwRiffSize = 20 + fmtSize + dataSize; // 4('WAVE') + 8('fmt '+size) + fmt + 8('data'+size) + data

    // RIFF chunk
    if (!WriteFile(hFile, "RIFF", 4, &bytesWritten, NULL)) return false;
    if (!WriteFile(hFile, &dwRiffSize, 4, &bytesWritten, NULL)) return false;
    if (!WriteFile(hFile, "WAVE", 4, &bytesWritten, NULL)) return false;

    // FORMAT chunk
    if (!WriteFile(hFile, "fmt ", 4, &bytesWritten, NULL)) return false;
    DWORD dwFmtSize = fmtSize;
    if (!WriteFile(hFile, &dwFmtSize, 4, &bytesWritten, NULL)) return false;
    if (!WriteFile(hFile, pwfx, fmtSize, &bytesWritten, NULL)) return false;

    // DATA chunk
    if (!WriteFile(hFile, "data", 4, &bytesWritten, NULL)) return false;
    if (!WriteFile(hFile, &dataSize, 4, &bytesWritten, NULL)) return false;

    return true;
}

bool AudioCapturer::FixWavHeader(HANDLE hFile, DWORD dataSize, WORD cbSize)
{
    DWORD bytesWritten;
    // RIFF size = 4 (WAVE) + 4 (fmt ID) + 4 (fmt size) + (sizeof(WAVEFORMATEX)+cbSize) + 4 (data ID) + 4 (data size) + dataSize
    DWORD fmtSize = sizeof(WAVEFORMATEX) + cbSize; // typically 16 or 18+cbSize
    DWORD dwRiffSize = 20 + fmtSize + dataSize;

    // Update RIFF chunk size
    SetFilePointer(hFile, 4, NULL, FILE_BEGIN);
    if (!WriteFile(hFile, &dwRiffSize, 4, &bytesWritten, NULL)) return false;

    // dataSize offset = 4 (RIFF) + 4 (dwRiffSize) + 4 (WAVE) + 4 (fmt ID) + 4 (fmt size) + fmtSize (WAVEFORMATEX + cbSize) + 4 (data ID)
    DWORD dataSizeOffset = 24 + fmtSize;
    SetFilePointer(hFile, dataSizeOffset, NULL, FILE_BEGIN);
    if (!WriteFile(hFile, &dataSize, 4, &bytesWritten, NULL)) return false;

    return true;
}

