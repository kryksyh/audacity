/*
* Audacity: A Digital Audio Editor
*/
#pragma once
#include "au3-audio-io/AudioPlaybackProcessor.h"

namespace au::au3audio {
class NativePlaybackProcessor final : public AudioPlaybackProcessor
{
public:
    NativePlaybackProcessor(AudacityProject&, const Sequences&, double rate, unsigned channels, size_t capacity, bool playing = true);
    ~NativePlaybackProcessor() override;
    unsigned ProcessingChannels(size_t sequence) const override;
    size_t Capacity() const override;
    void Reset() override;
    void SetPlaybackLoop(bool cycling, double start, double end) override;
    // Call only while processing is detached/stopped.
    void SetTransportContext(double tempo, int numerator, int denominator);
    void SetRealtimeMode();
    bool NeedsRebuild() const;
    size_t Process(RealtimeEffects::ProcessingScope&, std::span<const float* const>, std::span<const double>,
                   std::optional<double> nextTime = std::nullopt) override;
    const float* Output(unsigned channel) const override;
    std::span<const double> OutputTimes() const override;
    const float* Meter(size_t sequence, unsigned channel) const override;
private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};
void registerNativePlaybackProcessor();
}
