#include "OpusEncoder.h"
#include <opus/opus.h>
#include <algorithm>
#include <iostream>

OpusEncoderWrapper::OpusEncoderWrapper() = default;
OpusEncoderWrapper::~OpusEncoderWrapper() { shutdown(); }

bool OpusEncoderWrapper::ValidateSettings(const Settings& s, std::string* error)
{
    auto fail = [error](const char* message) { if (error) *error = message; return false; };
    if (s.sampleRate != 8000 && s.sampleRate != 12000 && s.sampleRate != 16000 &&
        s.sampleRate != 24000 && s.sampleRate != 48000) return fail("unsupported Opus sample rate");
    if (s.channels != 1 && s.channels != 2) return fail("Opus channels must be 1 or 2");
    const int validFrames[] = {s.sampleRate / 400, s.sampleRate / 200, s.sampleRate / 100,
                               s.sampleRate / 50, s.sampleRate / 25, 3 * s.sampleRate / 50};
    if (std::find(std::begin(validFrames), std::end(validFrames), s.frameSize) == std::end(validFrames))
        return fail("invalid Opus frame size");
    if (s.bitrate < 6000 || s.bitrate > 510000 * s.channels) return fail("Opus bitrate is out of range");
    if (s.complexity < 0 || s.complexity > 10) return fail("Opus complexity is out of range");
    if (s.expectedLossPerc < 0 || s.expectedLossPerc > 100) return fail("Opus loss percentage is out of range");
    if (s.application != OPUS_APPLICATION_VOIP && s.application != OPUS_APPLICATION_AUDIO &&
        s.application != OPUS_APPLICATION_RESTRICTED_LOWDELAY) return fail("invalid Opus application");
    return true;
}

bool OpusEncoderWrapper::initialize(const Settings& s)
{
    shutdown();

    std::string validationError;
    if (!ValidateSettings(s, &validationError)) {
        std::cerr << "[OpusEncoder] Invalid settings: " << validationError << std::endl;
        return false;
    }

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

    const int controls[] = {
        opus_encoder_ctl(enc, OPUS_SET_BITRATE(s.bitrate)),
        opus_encoder_ctl(enc, OPUS_SET_VBR(s.useVbr ? 1 : 0)),
        opus_encoder_ctl(enc, OPUS_SET_VBR_CONSTRAINT(s.constrainedVbr ? 1 : 0)),
        opus_encoder_ctl(enc, OPUS_SET_COMPLEXITY(s.complexity)),
        opus_encoder_ctl(enc, OPUS_SET_INBAND_FEC(s.enableFec ? 1 : 0)),
        opus_encoder_ctl(enc, OPUS_SET_PACKET_LOSS_PERC(s.expectedLossPerc)),
        opus_encoder_ctl(enc, OPUS_SET_DTX(s.enableDtx ? 1 : 0))
    };
    if (std::any_of(std::begin(controls), std::end(controls), [](int rc) { return rc != OPUS_OK; })) {
        shutdown();
        return false;
    }

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

    return ret; // Return actual encoded size
}


