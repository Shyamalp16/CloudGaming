#include "AudioHelper.h"
#include <iostream>
#include <stdexcept>

DWORD GetProcessIdFromHWND(HWND hwnd) {
	DWORD processID = 0;
	if (hwnd) {
		::GetWindowThreadProcessId(hwnd, &processID);
	}
	else {
		std::wcerr << L"[GetProcessIdFromHWND] Invalid HWND.\n";
	}
	return processID;
}

winrt::Windows::Media::Audio::AudioGraph audioGraph{ nullptr };
winrt::Windows::Media::Audio::AudioDeviceInputNode deviceInputNode{ nullptr };
winrt::Windows::Media::Audio::AudioFrameOutputNode frameOutputNode{ nullptr };
winrt::Windows::Media::Audio::AudioNodeEmitter emitter{ nullptr };
std::atomic<bool> isAudioCapturing{ false };
std::thread audioThread;

//setup audio capture pipeline
winrt::Windows::Foundation::IAsyncAction SetupAudioCaptureAsync(HWND targetHWND) {
	winrt::Windows::Media::Audio::AudioGraphSettings settings(winrt::Windows::Media::Render::AudioRenderCategory::GameMedia);
	settings.EncodingProperties(winrt::Windows::Media::MediaProperties::AudioEncodingProperties::CreatePcm(48000, 2, 32)); // 48kHz, stereo, 32-bit
	settings.QuantumSizeSelectionMode(winrt::Windows::Media::Audio::QuantumSizeSelectionMode::LowestLatency);

	winrt::Windows::Media::Audio::CreateAudioGraphResult result = co_await winrt::Windows::Media::Audio::AudioGraph::CreateAsync(settings);
	if (!audioGraph) {
		std::wcerr << L"[SetupAudioCapture] Failed to create AudioGraph. HRESULT=0x" << std::hex << result.ExtendedError() << std::endl;
		co_return;
	}
	audioGraph = result.Graph();
	std::wcout << L"[SetupAudioCapture] AudioGraph created successfully." << std::endl;

	frameOutputNode = audioGraph.CreateFrameOutputNode();
	if (!frameOutputNode) {
		std::wcerr << L"[SetupAudioCapture] Failed to create FrameOutputNode." << std::endl;
		co_return;
	}
	std::wcout << L"[SetupAudioCapture] FrameOutputNode created successfully." << std::endl;

	
	//fetch default audio device
	winrt::hstring deviceId = winrt::Windows::Media::Devices::MediaDevice::GetDefaultAudioRenderId(winrt::Windows::Media::Devices::AudioDeviceRole::Default);
	if (deviceId.empty()) {
		std::wcerr << L"[SetupAudioCapture] No default audio device found." << std::endl;
		co_return;
	}

	winrt::Windows::Media::Audio::CreateAudioDeviceInputNodeResult inputNodeResult = co_await audioGraph.CreateDeviceInputNodeAsync(
		winrt::Windows::Media::Capture::MediaCategory::Media,
		audioGraph.EncodingProperties(),
		co_await winrt::Windows::Devices::Enumeration::DeviceInformation::CreateFromIdAsync(deviceId)
	);

	if (inputNodeResult.Status() != winrt::Windows::Media::Audio::AudioDeviceNodeCreationStatus::Success) {
		std::wcerr << L"[SetupAudioCapture] Failed to create DeviceInputNode. Status: " << static_cast<int>(inputNodeResult.Status()) << std::endl;
		co_return;
	}
	
	deviceInputNode = inputNodeResult.DeviceInputNode();
	std::wcout << L"[SetupAudioCapture] DeviceInputNode created successfully." << std::endl;

	DWORD targetPid = GetProcessIdFromHWND(targetHWND);
	if (targetPid == 0) {
		std::wcerr << L"[SetupAudioCapture] Invalid target process ID." << std::endl;
		co_return;
	}

	winrt::Windows::Media::Audio::AudioNodeEmitter emitter(winrt::Windows::Media::Audio::AudioNodeEmitterShape::CreateOmnidirectional(),
														   winrt::Windows::Media::Audio::AudioNodeEmitterDecayModel::CreateNatural(1.0, 100.0, 0.0, 1.0),
														   winrt::Windows::Media::Audio::AudioNodeEmitterSettings::None);

	emitter.SpatialAudioModel(winrt::Windows::Media::Audio::SpatialAudioModel::ObjectBased);
	emitter.AddProcessToTrack(targetPid);
		
}