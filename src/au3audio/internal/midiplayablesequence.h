/*
* Audacity: A Digital Audio Editor
*/
#pragma once

#include <algorithm>
#include <array>
#include <limits>

#include "au3-midi-track/MidiTrack.h"
#include "instrumentprocessor.h"
#include "au3-automation-data/AutomationData.h"

namespace au::au3audio {
class NativeTimeSignature;
class MidiPlayableSequence final : public PlayableSequence
{
public:
    MidiPlayableSequence(std::shared_ptr<const MidiTrack> track, std::shared_ptr<InstrumentProcessor> processor, double sampleRate,
                         bool automation = true, bool playClips = true, int numerator = 4, int denominator = 4, double renderEnd = 0.0,
                         const NativeTimeSignature* liveTimeSignature = nullptr);

    size_t NChannels() const override { return 2; }
    void SetPlaybackLoop(bool cycling, double start, double end) const override;
    float GetChannelVolume(int channel) const override;
    AudioGraph::ChannelType GetChannelType() const override { return AudioGraph::LeftChannel; }
    const ChannelGroup* FindChannelGroup() const override { return m_track.get(); }
    bool GetSolo() const override { return m_track->GetSolo(); }
    bool GetMute() const override { return m_track->GetMute(); }
    double GetStartTime() const override
    { return m_processor->acceptsLiveInput() ? -std::numeric_limits<double>::infinity() : m_track->GetStartTime(); }
    double GetEndTime() const override
    {
        return m_processor->acceptsLiveInput() ? std::numeric_limits<double>::infinity()
               : std::max({ m_track->GetEndTime(), m_renderEnd, m_loopLast / m_sampleRate });
    }

    double GetRate() const override { return m_sampleRate; }
    sampleFormat WidestEffectiveFormat() const override { return floatSample; }
    bool HasTrivialEnvelope() const override { return true; }
    void GetEnvelopeValues(double* buffer, size_t length, double, bool) const override;
    bool DoGet(size_t channel, size_t channels, const samplePtr output[], sampleFormat format, sampleCount start, size_t length,
               bool backwards, fillFormat fill, bool mayThrow, sampleCount* within) const override;

private:
    void render(int64_t sample, size_t count) const;
    int64_t loopPosition(int64_t sample) const;
    void releaseLoop() const;
    std::shared_ptr<const MidiTrack> m_track;
    std::shared_ptr<InstrumentProcessor> m_processor;
    MidiSequence m_sequence;
    bool m_playClips = true;
    AutomationData m_automation;
    std::vector<std::pair<std::string, std::string> > m_parameters;
    double m_sampleRate, m_tempo;
    // Offline renders must keep pulling the instrument after its last clip
    // so note releases and instrument reverb can fill the requested tail.
    double m_renderEnd;
    int m_numerator, m_denominator;
    const NativeTimeSignature* m_liveTimeSignature = nullptr;
    // One bounded rendered packet. Its content position is captured with its
    // own delay; later plugin changes must not retag already-rendered audio.
    mutable int64_t m_renderNext = 0, m_packetStart = 0;
    mutable size_t m_packetFrames = 0, m_packetOffset = 0;
    mutable bool m_cycling = false;
    mutable double m_cycleStart = 0.0, m_cycleEnd = 0.0;
    // Project requests wrap; packet/output positions retain an unwrapped clock
    // so lookahead across several cycles is neither discarded nor replayed.
    mutable int64_t m_loopFirst = 0, m_loopLast = 0, m_outputNext = 0;
    mutable bool m_loopChanged = false;
    mutable std::vector<MidiEvent> m_loopChase, m_loopRelease, m_loopControls;
    mutable int64_t m_nextSample = std::numeric_limits<int64_t>::min();
    mutable std::optional<int64_t> m_processedSamples;
    mutable std::vector<MidiEvent> m_events;
    mutable std::array<std::array<float, 512>, 2> m_buffers {};
};
}
