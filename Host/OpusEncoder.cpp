#include "OpusEncoder.h"
#include <opus/opus.h>
#include <algorithm>
#include <iostream>
#include <chrono>
#include <cfloat>

OpusEncoderWrapper::OpusEncoderWrapper() = default;
OpusEncoderWrapper::~OpusEncoderWrapper() { shutdown(); }

bool OpusEncoderWrapper::initialize(const Settings& s)
{
    shutdown();

    int err = OPUS_OK;
    m_sampleRate = s.sampleRate;
    m_channels = s.channels;
    m_frameSize = s.frameSize;

    OpusEncoder* enc = opus_encoder_create(m_sampleRate, m_channels, s.application, &err);
    if (err != OPUS_OK || enc == nullptr) {
        m_encoder = nullptr;
        return false;
    }
    m_encoder = enc;

    opus_encoder_ctl(enc, OPUS_SET_BITRATE(s.bitrate));
    opus_encoder_ctl(enc, OPUS_SET_VBR(s.useVbr ? 1 : 0));
    opus_encoder_ctl(enc, OPUS_SET_VBR_CONSTRAINT(s.constrainedVbr ? 1 : 0));
    opus_encoder_ctl(enc, OPUS_SET_COMPLEXITY(s.complexity));
    opus_encoder_ctl(enc, OPUS_SET_INBAND_FEC(s.enableFec ? 1 : 0));
    opus_encoder_ctl(enc, OPUS_SET_PACKET_LOSS_PERC(std::clamp(s.expectedLossPerc, 0, 100)));
    opus_encoder_ctl(enc, OPUS_SET_DTX(s.enableDtx ? 1 : 0));

    return true;
}

void OpusEncoderWrapper::shutdown()
{
    if (m_encoder) {
        opus_encoder_destroy(reinterpret_cast<OpusEncoder*>(m_encoder));
        m_encoder = nullptr;
    }
}

bool OpusEncoderWrapper::encodeFrame(const float* pcmInterleaved, std::vector<uint8_t>& encodedData)
{
    if (!m_encoder || !pcmInterleaved) return false;

    // Opus worst-case packet is small; allocate a safe buffer
    // RFC 6716 suggests 1275 bytes for 20 ms at 48 kHz per channel as a safe max
    encodedData.resize(1500);

    int numSamplesPerChannel = m_frameSize;
    int ret = opus_encode_float(reinterpret_cast<OpusEncoder*>(m_encoder),
                                pcmInterleaved,
                                numSamplesPerChannel,
                                encodedData.data(),
                                static_cast<opus_int32>(encodedData.size()));
    if (ret < 0) {
        return false;
    }
    encodedData.resize(static_cast<size_t>(ret));
    return true;
}

int OpusEncoderWrapper::encodeFrameToBuffer(const float* pcmInterleaved, uint8_t* buffer, size_t bufferSize)
{
    if (!m_encoder || !pcmInterleaved || !buffer || bufferSize == 0) {
        return -1;
    }

    // Debug: Check input audio levels before encoding (reduced frequency to avoid log spam)
    static int debugCount = 0;
    debugCount++;
    // FIX: Reduce logging frequency - log first 3 frames, then every 1000 frames (was 100)
    if (debugCount <= 3 || debugCount % 1000 == 0) {
        float maxVal = -FLT_MAX, minVal = FLT_MAX, sumSquares = 0.0f;
        int validSamples = 0;

        for (int i = 0; i < m_frameSize * m_channels; ++i) {
            float sample = pcmInterleaved[i];

            // Check for valid audio range (-1.0 to 1.0)
            if (sample >= -1.0f && sample <= 1.0f) {
                if (sample > maxVal) maxVal = sample;
                if (sample < minVal) minVal = sample;
                sumSquares += sample * sample;
                validSamples++;
            }
        }

        float rmsVal = 0.0f;
        if (validSamples > 0) {
            rmsVal = sqrtf(sumSquares / validSamples);
        }

        std::cout << "[OpusEncoder] Input check - Valid samples: " << validSamples << "/" << (m_frameSize * m_channels)
                  << ", RMS: " << rmsVal << ", Max: " << maxVal << ", Min: " << minVal << std::endl;

        // Warn about invalid samples
        if (validSamples < m_frameSize * m_channels) {
            std::cout << "[OpusEncoder] WARNING: " << ((m_frameSize * m_channels) - validSamples)
                      << " samples are out of valid audio range!" << std::endl;
        }
    }

    int numSamplesPerChannel = m_frameSize;

    // Validate input range; if any samples are outside [-1,1] or non-finite, clamp into a scratch buffer
    bool needsClamp = false;
    int total = m_frameSize * m_channels;
    for (int i = 0; i < total; ++i) {
        float s = pcmInterleaved[i];
        if (!std::isfinite(s) || s > 1.0f || s < -1.0f) { needsClamp = true; break; }
    }

    int ret = 0;
    if (needsClamp) {
        static thread_local std::vector<float> scratch;
        scratch.resize(static_cast<size_t>(total));
        for (int i = 0; i < total; ++i) {
            float s = pcmInterleaved[i];
            if (!std::isfinite(s)) s = 0.0f;
            if (s > 1.0f) s = 1.0f; else if (s < -1.0f) s = -1.0f;
            scratch[static_cast<size_t>(i)] = s;
        }
        ret = opus_encode_float(reinterpret_cast<OpusEncoder*>(m_encoder),
                                scratch.data(),
                                numSamplesPerChannel,
                                buffer,
                                static_cast<opus_int32>(bufferSize));
    } else {
        ret = opus_encode_float(reinterpret_cast<OpusEncoder*>(m_encoder),
                                pcmInterleaved,
                                numSamplesPerChannel,
                                buffer,
                                static_cast<opus_int32>(bufferSize));
    }
    if (ret < 0) {
        std::cout << "[OpusEncoder] Encoding error: " << ret << std::endl;
        return -1; // Error
    }

    // Debug: Check if the encoded data looks like silence (comfort noise)
    if (ret == 3 && debugCount % 50 == 0) {  // 3 bytes is comfort noise
        std::cout << "[OpusEncoder] WARNING: 3-byte comfort noise packet detected!" << std::endl;

        // Check the actual encoded bytes
        if (buffer && ret >= 3) {
            std::cout << "[OpusEncoder] Encoded bytes: ";
            for (int i = 0; i < ret; ++i) {
                std::cout << std::hex << (int)buffer[i] << " ";
            }
            std::cout << std::dec << std::endl;
        }
    }

    // Debug: Log encoding results every 5 seconds
    static int encodeCount = 0;
    static auto lastEncodeLog = std::chrono::steady_clock::now();
    encodeCount++;
    auto now = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - lastEncodeLog).count();

    if (elapsed >= 5000) {  // 5 seconds
        std::cout << "[OpusEncoder] Frames encoded in last 5s: " << encodeCount << ", Last frame size: " << ret << " bytes" << std::endl;
        lastEncodeLog = now;
        encodeCount = 0;  // Reset counter
    }

    return ret; // Return actual encoded size
}


