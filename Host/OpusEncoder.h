#pragma once

#include <cstdint>
#include <vector>
#include <memory>

// Minimal Opus encoder wrapper for 48 kHz, 20 ms frames.
// Encodes interleaved float PCM ([-1.0, 1.0]) into Opus RTP payloads.
class OpusEncoderWrapper {
public:
    struct Settings {
        int sampleRate = 48000;            // Hz
        int channels = 2;                  // 1 or 2
        int frameSize = 960;               // samples per channel (20 ms at 48 kHz)
        int bitrate = 128000;              // bps
        int complexity = 8;                // 0..10
        bool useVbr = true;                // enable VBR
        bool constrainedVbr = true;        // constrain VBR peaks
        bool enableFec = true;             // in-band FEC
        int expectedLossPerc = 10;         // 0..100
        bool enableDtx = false;            // DTX (mainly for VOIP)
        int application = 2049;            // OPUS_APPLICATION_AUDIO (2049) by default
    };

    OpusEncoderWrapper();
    ~OpusEncoderWrapper();

    bool initialize(const Settings& settings);
    void shutdown();

    // Encode a single 20 ms frame of interleaved float samples.
    // pcmInterleaved size must be frameSize * channels.
    // Returns true and fills encodedData on success.
    bool encodeFrame(const float* pcmInterleaved, std::vector<uint8_t>& encodedData);

    // Encode a single frame using a pre-allocated buffer (optimization for buffer reuse).
    // pcmInterleaved size must be frameSize * channels.
    // buffer must be large enough for the maximum encoded size.
    // Returns the actual encoded size on success, -1 on failure.
    int encodeFrameToBuffer(const float* pcmInterleaved, uint8_t* buffer, size_t bufferSize);

    int channels() const { return m_channels; }
    int frameSize() const { return m_frameSize; }
    int sampleRate() const { return m_sampleRate; }

private:
    void* m_encoder = nullptr; // OpusEncoder*
    int m_sampleRate = 48000;
    int m_channels = 2;
    int m_frameSize = 960;
};


