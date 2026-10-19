/**********************************************************************

  Audacity: A Digital Audio Editor

  ExportUtils.cpp

  Dominic Mazzoni

  Vitaly Sverchinsky split from ExportPlugin.h

**********************************************************************/

#include "ExportUtils.h"

#include <algorithm>
#include <vector>

#include "au3-track/Track.h"
#include "au3-time-frequency-selection/ViewInfo.h"
#include "au3-wave-track/WaveTrack.h"
#include "au3-midi-track/MidiTrack.h"
#include "au3-playable-track/TrackRouting.h"

//TODO: used in many places in anticipation that Exporter yields same result, fix that
TrackIterRange<const WaveTrack> ExportUtils::FindExportWaveTracks(const TrackList& tracks, bool selectedOnly)
{
    const auto sources = TrackRouting::AudibleSources(tracks, selectedOnly);
    return tracks.Any<const WaveTrack>() + [sources](const WaveTrack* track) {
        return std::find(sources.begin(), sources.end(), track) != sources.end();
    };
}

TrackIterRange<const MidiTrack> ExportUtils::FindExportMidiTracks(const TrackList& tracks, bool selectedOnly)
{
    const auto sources = TrackRouting::AudibleSources(tracks, selectedOnly);
    return tracks.Any<const MidiTrack>() + [sources](const MidiTrack* track) {
        return std::find(sources.begin(), sources.end(), track) != sources.end();
    };
}

TrackIterRange<const PlayableTrack> ExportUtils::FindExportPlayableTracks(const TrackList& tracks, bool selectedOnly)
{
    const auto sources = TrackRouting::AudibleSources(tracks, selectedOnly);
    return tracks.Any<const PlayableTrack>() + [sources](const PlayableTrack* track) {
        return std::find(sources.begin(), sources.end(), track) != sources.end();
    };
}

bool ExportUtils::HasSelectedAudio(const AudacityProject& project)
{
    return !FindExportPlayableTracks(TrackList::Get(project), true).empty()
           && !ViewInfo::Get(project).selectedRegion.isPoint();
}

ExportProcessor::Parameters ExportUtils::ParametersFromEditor(const ExportOptionsEditor& editor)
{
    ExportProcessor::Parameters parameters;
    for (int i = 0, count = editor.GetOptionsCount(); i < count; ++i) {
        ExportOption option;
        ExportValue value;
        if (editor.GetOption(i, option) && editor.GetValue(option.id, value)) {
            parameters.emplace_back(option.id, value);
        }
    }
    return parameters;
}

namespace {
struct ExportHookElement final
{
    ExportUtils::ExportHook hook;
    ExportUtils::Priority priority;
};

auto& ExportHooks()
{
    static std::vector<ExportHookElement> hooks;
    return hooks;
}
}

void ExportUtils::RegisterExportHook(ExportHook hook, Priority priority)
{
    auto& hooks = ExportHooks();
    hooks.insert(
        std::upper_bound(
            hooks.begin(), hooks.end(), priority,
            [](Priority priority, const ExportHookElement& element)
    { return priority > element.priority; }),
        { hook, priority });
}

void ExportUtils::PerformInteractiveExport(
    AudacityProject& project, const FileExtension& format, AudiocomTrace trace,
    bool selectedOnly)
{
    auto& hooks = ExportHooks();
    for (auto& hook : hooks) {
        if (
            hook.hook(project, format, trace, selectedOnly)
            != ExportHookResult::Continue) {
            return;
        }
    }
}
