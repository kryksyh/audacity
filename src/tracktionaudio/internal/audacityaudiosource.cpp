/*
* Audacity: A Digital Audio Editor
*/
#include "audacityaudiosource.h"

#include <algorithm>
#include <atomic>

#include "au3-wave-track/SampleBlock.h"
#include "au3-wave-track/WaveClip.h"
#include "au3-mixer/Envelope.h"

using namespace au::tracktionaudio;

AudacityAudioSource::AudacityAudioSource(const WaveClip& clip)
    : m_sampleRate(clip.GetRate()), m_sampleCount(clip.GetSequence(0)->GetNumSamples().as_long_long()),
    m_envelope(std::make_shared<Envelope>(clip.GetEnvelope()))
{
    m_channels.reserve(clip.NChannels());
    for (size_t channel = 0; channel < clip.NChannels(); ++channel) {
        m_channels.push_back(clip.GetSequence(channel)->GetBlockArray());
    }
}

double AudacityAudioSource::sampleRate() const
{
    return m_sampleRate;
}

unsigned int AudacityAudioSource::channelCount() const
{
    return static_cast<unsigned int>(m_channels.size());
}

int64_t AudacityAudioSource::sampleCount() const
{
    return m_sampleCount;
}

std::shared_ptr<const Envelope> AudacityAudioSource::envelope() const
{
    return std::atomic_load_explicit(&m_envelope, std::memory_order_acquire);
}

void AudacityAudioSource::setEnvelope(const Envelope& envelope)
{
    const std::shared_ptr<const Envelope> next = std::make_shared<Envelope>(envelope);
    m_retiredEnvelopes.push_back(std::atomic_exchange_explicit(&m_envelope, next, std::memory_order_acq_rel));
    // Delete old envelopes on the main thread after the audio threads release them.
    std::erase_if(m_retiredEnvelopes, [](const auto& previous) { return previous.use_count() == 1; });
}

bool AudacityAudioSource::read(unsigned int channel, float* destination, int64_t start, int count) const
{
    std::fill_n(destination, count, 0.0f);

    if (channel >= m_channels.size() || count == 0 || start >= m_sampleCount) {
        return true;
    }

    if (start < 0) {
        const int padding = static_cast<int>(std::min<int64_t>(-start, count));
        destination += padding;
        count -= padding;
        start += padding;
    }

    const auto& blocks = m_channels[channel];
    auto block = std::upper_bound(blocks.begin(), blocks.end(), start, [](int64_t position, const SeqBlock& item) {
        return position < item.start.as_long_long();
    });
    if (block != blocks.begin()) {
        --block;
    }

    int remaining = static_cast<int>(std::min<int64_t>(count, m_sampleCount - start));
    while (remaining > 0 && block != blocks.end()) {
        const auto offset = static_cast<size_t>(start - block->start.as_long_long());
        const size_t available = block->sb->GetSampleCount() - offset;
        const size_t requested = std::min<size_t>(remaining, available);
        const size_t copied = block->sb->GetSamples(reinterpret_cast<samplePtr>(destination), floatSample, offset, requested, false);
        if (copied != requested) {
            return false;
        }
        start += requested;
        destination += requested;
        remaining -= static_cast<int>(requested);
        ++block;
    }

    return remaining == 0;
}
