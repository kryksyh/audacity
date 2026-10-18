/**********************************************************************

  Audacity: A Digital Audio Editor

  ExportPluginHelpers.cpp

  Dominic Mazzoni

  Vitaly Sverchinsky split from ExportPlugin.h

**********************************************************************/

#include "ExportPluginHelpers.h"

#include "ExportUtils.h"
#include "au3-wave-track/WaveTrack.h"
#include "au3-midi-track/MidiTrack.h"

std::unique_ptr<AudioRenderSource> ExportPluginHelpers::CreateMixer(
    const AudacityProject& project, bool selectionOnly, double startTime,
    double stopTime, unsigned numOutChannels, size_t outBufferSize,
    bool outInterleaved, double outRate, sampleFormat outFormat,
    MixerOptions::Downmix* mixerSpec)
{
    AudioRenderSource::Tracks tracks;
    for (const auto* track : ExportUtils::FindExportWaveTracks(TrackList::Get(project), selectionOnly)) {
        tracks.push_back(std::static_pointer_cast<const WaveTrack>(track->shared_from_this()));
    }
    AudioRenderSource::Options options {
        startTime, stopTime, numOutChannels, outBufferSize, outInterleaved, outRate, outFormat, mixerSpec
    };
    options.channelMode = mixerSpec ? AudioRenderSource::ChannelMode::MapChannels : AudioRenderSource::ChannelMode::Mixdown;
    options.masterEffects = true;
    for (const auto* track : ExportUtils::FindExportMidiTracks(TrackList::Get(project), selectionOnly)) {
        options.midiTracks.push_back(std::static_pointer_cast<const MidiTrack>(track->shared_from_this()));
    }
    return AudioRenderSource::Create(project, tracks, options);
}

namespace {
double EvalExportProgress(AudioRenderSource& mixer, double t0, double t1)
{
    const auto duration = t1 - t0;
    if (duration > 0) {
        return std::clamp(mixer.MixGetCurrentTime() - t0, .0, duration) / duration;
    }
    return .0;
}
}

ExportResult ExportPluginHelpers::UpdateProgress(ExportProcessorDelegate& delegate, AudioRenderSource& mixer, double t0, double t1)
{
    delegate.OnProgress(EvalExportProgress(mixer, t0, t1));
    if (delegate.IsStopped()) {
        return ExportResult::Stopped;
    }
    if (delegate.IsCancelled()) {
        return ExportResult::Cancelled;
    }
    return ExportResult::Success;
}
