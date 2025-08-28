#include "OpusEncoder.h"
#include <opus/opus.h>
#include <algorithm>

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

    int numSamplesPerChannel = m_frameSize;
    int ret = opus_encode_float(reinterpret_cast<OpusEncoder*>(m_encoder),
                                pcmInterleaved,
                                numSamplesPerChannel,
                                buffer,
                                static_cast<opus_int32>(bufferSize));
    if (ret < 0) {
        return -1; // Error
    }
    return ret; // Return actual encoded size
}


