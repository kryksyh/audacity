/*
* Audacity: A Digital Audio Editor
*/
#include <tracktion_engine/tracktion_engine.h>

#include "tracktionmixsource.h"
#include "audacityexternaleffect.h"
#include "tracktionautomation.h"
#include "au3-playable-track/TrackAutomation.h"

#include "au3-components/EffectInterface.h"
#include "au3-math/Dither.h"
#include "au3-project/Project.h"
#include "au3-realtime-effects/RealtimeEffectList.h"
#include "au3-realtime-effects/RealtimeEffectState.h"
#include "au3-wave-track/WaveTrack.h"
#include "au3-midi-track/MidiTrack.h"
#include "au3-wave-track/WaveClip.h"

#include "tracktionrendersource.h"
#include "tracktionsession.h"

using namespace au::tracktionaudio;

namespace {
bool needsEffectDither(const RealtimeEffectList& list)
{
    if (!list.IsActive()) {
        return false;
    }
    for (size_t index = 0; index < list.GetStatesCount(); ++index) {
        const auto state = list.GetStateAt(index);
        const auto* effect = state->GetEffect();
        if (state->IsEnabled() && effect && state->GetSettings().has_value()) {
            if (supportsAudacityExternalEffect(*effect)) {
                return true;
            }
            const auto instance = effect->MakeInstance();
            if (instance && instance->NeedsDither()) {
                return true;
            }
        }
    }
    return false;
}
}

struct TracktionMixSource::Data
{
    Data(TracktionRuntime& runtime, const AudacityProject& project, const Tracks& tracks, Options renderOptions)
        : owner(project.shared_from_this()), options(renderOptions),
        buffers(options.interleaved ? 1 : options.channels)
    {
        std::vector<const WaveTrack*> sources;
        for (const auto& track : tracks) {
            sources.push_back(track.get());
            effectiveFormat = std::max(effectiveFormat, track->WidestEffectiveFormat());
            needsDither |= options.trackAutomation && hasTrackAutomation(TrackAutomation::Get(*track));
            needsDither |= track->GetRate() != options.sampleRate || !track->HasTrivialEnvelope()
                           || track->WidestEffectiveFormat() > options.format
                           || (options.channelMode == ChannelMode::Mixdown && !options.channelMap
                               && options.channels == 1 && track->NChannels() > 1)
                           || (options.trackEffects && needsEffectDither(RealtimeEffectList::Get(*track)));
            for (const auto channel : { 0, 1 }) {
                const auto gain = track->GetChannelVolume(channel);
                needsDither |= gain != 0.0f && gain != 1.0f;
            }
            for (const auto& clip : track->Intervals()) {
                needsDither |= clip->HasPitchOrSpeed();
            }
        }
        std::vector<const MidiTrack*> midiSources;
        for (const auto& midi : options.midiTracks) {
            midiSources.push_back(midi.get());
        }
        needsDither |= !midiSources.empty();
        needsDither |= options.masterEffects && needsEffectDither(RealtimeEffectList::Get(project));
        session = std::make_unique<TracktionSession>(runtime, TrackList::Get(
                                                         const_cast<AudacityProject&>(project)), sources,
                                                     TracktionSession::Role::Render, options.trackEffects, options.masterEffects,
                                                     midiSources, options.trackAutomation, options.trackRouting);
        needsDither |= session->hasRouting();
        if (needsDither) {
            effectiveFormat = options.format;
        }
        session->configureRender(options.channels, options.channelMap, options.channelMode == ChannelMode::Mixdown);
        source = std::make_unique<TracktionRenderSource>(*session, TracktionRenderSource::Options {
            options.startTime, tracks.empty() && midiSources.empty() ? options.startTime : options.stopTime,
            options.sampleRate, static_cast<int>(options.channels), 512
        });
        for (auto& buffer : buffers) {
            buffer.Allocate(options.bufferSize * (options.interleaved ? options.channels : 1), options.format);
        }
    }

    std::shared_ptr<const AudacityProject> owner;
    Options options;
    std::vector<SampleBuffer> buffers;
    bool needsDither = false;
    sampleFormat effectiveFormat = narrowestSampleFormat;
    std::unique_ptr<TracktionSession> session;
    std::unique_ptr<TracktionRenderSource> source;
};

TracktionMixSource::TracktionMixSource(TracktionRuntime& runtime, const AudacityProject& project, const Tracks& tracks, Options options)
    : m_data(std::make_unique<Data>(runtime, project, tracks, options))
{
}

TracktionMixSource::~TracktionMixSource() = default;

size_t TracktionMixSource::Process()
{
    return Process(m_data->options.bufferSize);
}

size_t TracktionMixSource::Process(size_t maximumFrames)
{
    auto& data = *m_data;
    const int frames = data.source->process(static_cast<int>(std::min(maximumFrames, data.options.bufferSize)));
    if (frames == 0) {
        return 0;
    }
    const auto& audio = data.source->buffer();
    const unsigned stride = data.options.interleaved ? data.options.channels : 1;
    const auto dither = data.needsDither ? gHighQualityDither : DitherType::none;
    for (unsigned channel = 0; channel < data.options.channels; ++channel) {
        auto* output = data.options.interleaved ? data.buffers[0].ptr() + channel * SAMPLE_SIZE(data.options.format)
                       : data.buffers[channel].ptr();
        CopySamples(reinterpret_cast<constSamplePtr>(audio.getReadPointer(channel)), floatSample,
                    output, data.options.format, frames, dither, 1, stride);
    }
    return frames;
}

constSamplePtr TracktionMixSource::GetBuffer()
{
    return m_data->buffers[0].ptr();
}

constSamplePtr TracktionMixSource::GetBuffer(int channel)
{
    return m_data->buffers[channel].ptr();
}

double TracktionMixSource::MixGetCurrentTime()
{
    return m_data->source->currentTime();
}

sampleFormat TracktionMixSource::EffectiveFormat() const
{
    return m_data->effectiveFormat;
}
