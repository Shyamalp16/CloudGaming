package main

import "testing"

func TestMediaRuntimeCanRestartAfterClose(t *testing.T) {
	closeGo()
	startMediaRuntime()

	mediaRuntimeMutex.Lock()
	running := mediaRuntimeRunning
	audioReady := audioSendQueue != nil && audioSendStop != nil
	videoReady := videoSendQueue != nil && videoSendStop != nil
	mediaRuntimeMutex.Unlock()

	if !running || !audioReady || !videoReady {
		t.Fatalf("media runtime did not restart: running=%v audio=%v video=%v",
			running, audioReady, videoReady)
	}

	closeGo()
}
