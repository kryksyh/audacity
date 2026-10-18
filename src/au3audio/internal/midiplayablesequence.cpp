/*
* Audacity: A Digital Audio Editor
*/
#include "midiplayablesequence.h"
#include "nativetimesignature.h"

#include <algorithm>
#include <cstring>
#include <cmath>
#include <stdexcept>
#include "au3-math/Dither.h"
#include "au3-midi-track/MidiController.h"
#include "au3-playable-track/TrackAutomation.h"
#include "au3-utility/MemoryX.h"

using namespace au::au3audio;

MidiPlayableSequence::MidiPlayableSequence(std::shared_ptr<const MidiTrack> track,
                                           std::shared_ptr<InstrumentProcessor> processor, double sampleRate, bool automation,
                                           bool playClips,
                                           int numerator, int denominator, double renderEnd, const NativeTimeSignature* liveTimeSignature)
    : m_track(std::move(track)), m_processor(std::move(processor)), m_sequence(*m_track, sampleRate, playClips), m_playClips(playClips),
    m_sampleRate(sampleRate), m_tempo(m_track->GetTempo()), m_renderEnd(renderEnd), m_numerator(numerator), m_denominator(denominator),
    m_liveTimeSignature(liveTimeSignature)
{
    m_processor->prepare(sampleRate, 512);
    m_events.reserve(m_sequence.Events().size() + 4096);
    if (automation) {
        m_automation = TrackAutomation::Get(*m_track);
    }
    for (const auto& lane : m_automation.Lanes()) {
        constexpr std::string_view prefix = "instrument/";
        if (lane.enabled && !lane.points.empty() && lane.parameter.starts_with(prefix)) {
            m_parameters.emplace_back(lane.parameter, lane.parameter.substr(prefix.size()));
        }
    }
}

void MidiPlayableSequence::render(int64_t sample, size_t count) const
{
    for (const auto& [lane, parameter] : m_parameters) {
        m_processor->setAutomatedParameter(parameter, m_automation.ValueAt(lane, sample / m_sampleRate, 0.0));
    }
    float* output[] { m_buffers[0].data(), m_buffers[1].data() };
    const auto meter = m_liveTimeSignature ? m_liveTimeSignature->meter() : NativeTimeSignature::Meter { m_numerator, m_denominator };
    InstrumentProcessContext context { sample, m_tempo, meter.numerator, meter.denominator, true, m_cycling, m_cycleStart, m_cycleEnd };
    if (!m_processedSamples) {
        m_processedSamples = sample;
    }
    context.continuousSample = m_processedSamples;
    m_processor->process(output, 2, count, m_events, context);
    *m_processedSamples += static_cast<int64_t>(count);
}

void MidiPlayableSequence::SetPlaybackLoop(bool cycling, double start, double end) const
{
    if (m_cycling == cycling && m_cycleStart == start && m_cycleEnd == end) {
        return;
    }
    // Recording uses DBL_MAX for its open-ended range. Inactive loop bounds
    // remain seconds in the plugin context and are never converted to samples.
    if (!std::isfinite(start) || !std::isfinite(end)
        || (cycling && (static_cast<long double>(start) * m_sampleRate <= std::numeric_limits<int64_t>::min()
                        || static_cast<long double>(end) * m_sampleRate <= std::numeric_limits<int64_t>::min()
                        || (static_cast<long double>(end) - start) * m_sampleRate >= std::numeric_limits<int64_t>::max()
                        || static_cast<long double>(end) * m_sampleRate >= std::numeric_limits<int64_t>::max()
                        || static_cast<long double>(start) * m_sampleRate >= std::numeric_limits<int64_t>::max()))) {
        throw std::out_of_range("MIDI playback loop is out of range.");
    }
    m_cycling = cycling;
    m_cycleStart = start;
    m_cycleEnd = end;
    m_loopFirst = m_loopLast = 0;
    m_loopChanged = true;
    m_loopChase.clear();
    m_loopRelease.clear();
    m_loopControls.clear();
    if (!cycling || end <= start) {
        return;
    }
    m_loopFirst = static_cast<int64_t>(std::llround(static_cast<long double>(start) * m_sampleRate));
    m_loopLast = static_cast<int64_t>(std::llround(static_cast<long double>(end) * m_sampleRate));
    if (m_loopLast <= m_loopFirst) {
        return;
    }
    if (!m_playClips) {
        return;
    }
    m_sequence.Chase(m_loopFirst, m_loopChase);
    std::vector<MidiEvent> ending;
    m_sequence.Chase(m_loopLast, ending);
    for (uint8_t channel = 0; channel < 16; ++channel) {
        m_loopRelease.push_back({ 0, static_cast<uint8_t>(0xb0 | channel), 64, 0 });
    }
    for (const auto& event : ending) {
        if ((event.status & 0xf0) == 0x90) {
            m_loopRelease.push_back({ 0, static_cast<uint8_t>(0x80 | (event.status & 0xf)), event.data1, 0 });
        } else if ((event.status & 0xf0) == 0xb0 && event.data1 < 120 && event.data1 != 64) {
            m_loopControls.push_back(event);
        }
    }
}

int64_t MidiPlayableSequence::loopPosition(int64_t sample) const
{
    if (m_loopLast > m_loopFirst && sample >= m_loopLast) {
        const auto offset = static_cast<uint64_t>(sample) - static_cast<uint64_t>(m_loopLast);
        return m_loopFirst + static_cast<int64_t>(offset % static_cast<uint64_t>(m_loopLast - m_loopFirst));
    }
    return sample;
}

void MidiPlayableSequence::releaseLoop() const
{
    if (!m_playClips) {
        return;
    }
    if (!m_processor->releaseLoopTimeline()) {
        // Private processors own only the document's voices. Release those
        // notes through normal timestamped MIDI; reset()/native deactivation
        // would erase delayed audio, including a fixed-block adapter's queue.
        m_processor->restoreManualParameters();
        m_events.insert(m_events.end(), m_loopRelease.begin(), m_loopRelease.end());
        for (auto event : m_loopControls) {
            if (!m_processor->restoreMidiController(event.status & 0xf, event.data1)) {
                const auto value = MidiControllerResetValue(event.data1);
                if (value >= 0) {
                    event.data2 = static_cast<uint8_t>(value);
                    m_events.push_back(event);
                }
            }
        }
    }
    m_events.insert(m_events.end(), m_loopChase.begin(), m_loopChase.end());
}

float MidiPlayableSequence::GetChannelVolume(int channel) const
{
    const auto pan = m_track->GetPan();
    return static_cast<float>(m_track->GetGain() * (channel == 0 ? std::min(1.0, 1.0 - pan) : std::min(1.0, 1.0 + pan)));
}

void MidiPlayableSequence::GetEnvelopeValues(double* buffer, size_t length, double, bool) const
{
    std::fill_n(buffer, length, 1.0);
}

bool MidiPlayableSequence::DoGet(size_t channel, size_t channels, const samplePtr output[], sampleFormat format,
                                 sampleCount start, size_t length, bool backwards, fillFormat, bool, sampleCount* within) const
{
    if (channel + channels > 2) {
        return false;
    }
    if (!length) {
        if (within) {
            *within = 0;
        }
        return true;
    }
    if (length > static_cast<size_t>(std::numeric_limits<int64_t>::max())
        || (backwards && start.as_long_long() < std::numeric_limits<int64_t>::min() + static_cast<int64_t>(length))) {
        throw std::out_of_range("MIDI sample request is out of range.");
    }
    int64_t position = start.as_long_long() - (backwards ? static_cast<int64_t>(length) : 0);
    if (position > std::numeric_limits<int64_t>::max() - static_cast<int64_t>(length)) {
        throw std::out_of_range("MIDI sample request is out of range.");
    }
    const auto requested = position;
    const bool wrap = !backwards && !m_loopChanged && m_loopLast > m_loopFirst
                      && requested == m_loopFirst && m_nextSample == m_loopLast;
    const bool continuous = !m_loopChanged && (requested == m_nextSample || wrap);
    const bool offline = !m_processor->acceptsLiveInput() && !m_liveTimeSignature;
    if (!continuous) {
        m_events.clear();
        m_processor->resetTimeline();
        if (offline) {
            // Discover run-time latency before sending the selection's notes.
            // Restore the continuous clock even when native control fails.
            const auto clock = m_processedSamples;
            const auto restoreClock = finally([&] { m_processedSamples = clock; });
            render(position, m_buffers[0].size());
            settleInstrumentLatency(*m_processor);
            m_processor->resetTimeline();
        }
        if (m_playClips) {
            m_sequence.Chase(position, m_events);
        }
        m_renderNext = position;
        m_packetFrames = m_packetOffset = 0;
        m_loopChanged = false;
    } else {
        position = m_outputNext;
    }
    if (position > std::numeric_limits<int64_t>::max() - static_cast<int64_t>(length)) {
        throw std::out_of_range("MIDI loop output position is out of range.");
    }
    size_t written = 0;
    while (written < length) {
        if (m_packetOffset == m_packetFrames) {
            auto frames = std::min(m_parameters.empty() ? size_t { 512 } : size_t { 128 }, length - written);
            const auto projectPosition = loopPosition(m_renderNext);
            if (m_loopLast > m_loopFirst) {
                frames = std::min<uint64_t>(frames, static_cast<uint64_t>(m_loopLast) - static_cast<uint64_t>(projectPosition));
                if (m_renderNext >= m_loopLast && projectPosition == m_loopFirst) {
                    releaseLoop();
                }
            }
            if (m_renderNext > std::numeric_limits<int64_t>::max() - static_cast<int64_t>(frames)) {
                throw std::out_of_range("MIDI render position is out of range.");
            }
            if (m_playClips) {
                m_sequence.AppendEvents(projectPosition, frames, m_events);
            }
            render(projectPosition, frames);
            m_events.clear();
            auto delay = m_processor->latency();
            // A private export worker can wait for main-thread control. Live
            // sources never do so: AudioIO services their requests while the
            // worker is suspended. Each packet retains its own measured delay.
            if (offline && m_processor->latencyUpdatePending()) {
                delay = settleInstrumentLatency(*m_processor);
            }
            if (delay > static_cast<size_t>(std::numeric_limits<int64_t>::max())
                || m_renderNext < std::numeric_limits<int64_t>::min() + static_cast<int64_t>(delay)) {
                throw std::out_of_range("MIDI instrument latency is out of range.");
            }
            m_packetStart = m_renderNext - static_cast<int64_t>(delay);
            m_renderNext += static_cast<int64_t>(frames);
            m_packetFrames = frames;
            m_packetOffset = 0;
        }
        auto packetPosition = m_packetStart + static_cast<int64_t>(m_packetOffset);
        if (packetPosition < position) {
            // Increasing delay repeats older content. Consume that overlap
            // without moving/replaying the instrument's forward MIDI cursor.
            const auto overlap = static_cast<uint64_t>(position) - static_cast<uint64_t>(packetPosition);
            m_packetOffset += static_cast<size_t>(std::min<uint64_t>(overlap, m_packetFrames - m_packetOffset));
            if (m_packetOffset == m_packetFrames) {
                continue;
            }
            packetPosition = m_packetStart + static_cast<int64_t>(m_packetOffset);
        }
        // Reducing delay can omit native samples. Preserve the timeline with
        // bounded silence instead of shifting all subsequent audio/playhead time.
        const bool gap = packetPosition > position;
        const auto available = gap
                               ? static_cast<uint64_t>(packetPosition) - static_cast<uint64_t>(position)
                               : static_cast<uint64_t>(m_packetFrames - m_packetOffset);
        const auto count = static_cast<size_t>(std::min<uint64_t>(available, length - written));
        for (size_t index = 0; index < channels; ++index) {
            const auto offset = backwards ? length - written - count : written;
            auto* destination = output[index] + offset * SAMPLE_SIZE(format);
            if (gap) {
                std::memset(destination, 0, count * SAMPLE_SIZE(format));
                continue;
            }
            auto& buffer = m_buffers[channel + index];
            if (backwards) {
                std::reverse(buffer.begin() + m_packetOffset, buffer.begin() + m_packetOffset + count);
            }
            CopySamples(reinterpret_cast<constSamplePtr>(buffer.data() + m_packetOffset), floatSample,
                        destination, format, count, DitherType::none);
        }
        if (!gap) {
            m_packetOffset += count;
        }
        written += count;
        position += static_cast<int64_t>(count);
    }
    m_outputNext = position;
    m_nextSample = requested + static_cast<int64_t>(length);
    if (within) {
        *within = length;
    }
    return true;
}
