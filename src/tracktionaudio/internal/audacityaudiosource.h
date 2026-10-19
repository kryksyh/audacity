/*
* Audacity: A Digital Audio Editor
*/
#pragma once

#include <cstdint>
#include <memory>
#include <vector>

#include "au3-wave-track/Sequence.h"

class WaveClip;
class Envelope;

namespace au::tracktionaudio {
class AudacityAudioSource
{
public:
    explicit AudacityAudioSource(const WaveClip& clip);

    double sampleRate() const;
    unsigned int channelCount() const;
    int64_t sampleCount() const;
    std::shared_ptr<const Envelope> envelope() const;
    void setEnvelope(const Envelope& envelope);

    bool read(unsigned int channel, float* destination, int64_t start, int count) const;

private:
    double m_sampleRate = 0.0;
    int64_t m_sampleCount = 0;
    std::vector<BlockArray> m_channels;
    std::shared_ptr<const Envelope> m_envelope;
    std::vector<std::shared_ptr<const Envelope> > m_retiredEnvelopes;
};
}
