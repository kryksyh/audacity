/*
* Audacity: A Digital Audio Editor
*/
#include "au3clipsinteraction.h"
#include "au3midicliputils.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <set>

#include "au3wrap/internal/domconverter.h"
#include "trackediterrors.h"

using namespace au::trackedit;
using namespace au::au3;

namespace {
void publish(const ITrackeditProjectPtr& project, MidiTrack& track, const std::vector<MidiClipData>& previous)
{
    for (const auto& clip : previous) {
        if (!midi::clip(track, clip.id)) {
            project->notifyAboutClipRemoved(DomConverter::clip(&track, &clip));
        }
    }
    for (const auto& clip : track.GetClips()) {
        const auto old = std::find_if(previous.begin(), previous.end(), [&](const auto& item) { return item.id == clip.id; });
        if (old == previous.end()) {
            project->notifyAboutClipAdded(DomConverter::clip(&track, &clip));
        } else {
            project->notifyAboutClipChanged(DomConverter::clip(&track, &clip));
        }
    }
    project->notifyAboutTrackChanged(DomConverter::track(&track));
}
}

void Au3ClipsInteraction::notifyMidiClipChanged(const ClipKey& key)
{
    if (auto* track = midi::track(projectRef(), key.trackId)) {
        if (const auto* clip = midi::clip(*track, key.itemId)) {
            globalContext()->currentTrackeditProject()->notifyAboutClipChanged(DomConverter::clip(track, clip));
        }
    }
}

bool Au3ClipsInteraction::editMidiClipBoundaries(const ClipKeyList& keys, secs_t delta, secs_t minimum,
                                                 bool completed, bool left, bool stretch)
{
    if (keys.empty() || !std::isfinite(delta.to_double())) {
        return false;
    }
    double adjusted = delta.to_double();
    for (const auto& key : keys) {
        const auto clip = globalContext()->currentTrackeditProject()->clip(key);
        if (!clip.isValid()) {
            return false;
        }
        adjusted = std::min(adjusted, clip.endTime - clip.startTime - minimum.to_double());
        if (left) {
            adjusted = std::max(adjusted, -clip.startTime);
        }
    }
    for (const auto& key : keys) {
        auto* track = midi::track(projectRef(), key.trackId);
        if (!track) {
            return false;
        }
        const double beats = adjusted * track->GetTempo() / 60.0;
        const bool ok = midi::edit(*track, key.itemId, [&](auto& clip) {
            if (stretch) {
                const auto start = clip.visibleStart() + (left ? beats : 0.0);
                const auto ratio = (clip.visibleLength() - beats) / clip.visibleLength();
                clip.length *= ratio;
                clip.trimLeft *= ratio;
                clip.trimRight *= ratio;
                clip.start = start - clip.trimLeft;
                for (auto& note : clip.notes) {
                    note.start *= ratio;
                    note.length *= ratio;
                }
                for (auto& event : clip.controllers) {
                    event.beat *= ratio;
                }
            } else if (left) {
                clip.trimLeft += beats;
                if (clip.trimLeft < 0.0) {
                    // Extending a MIDI clip can add empty space beyond its
                    // original source. Hidden notes retain their timeline times.
                    clip.start += clip.trimLeft;
                    clip.length -= clip.trimLeft;
                    for (auto& note : clip.notes) {
                        note.start -= clip.trimLeft;
                    }
                    for (auto& event : clip.controllers) {
                        event.beat -= clip.trimLeft;
                    }
                    clip.trimLeft = 0.0;
                }
            } else {
                clip.trimRight += beats;
                if (clip.trimRight < 0.0) {
                    clip.length -= clip.trimRight;
                    clip.trimRight = 0.0;
                }
            }
        });
        if (!ok) {
            return false;
        }
        notifyMidiClipChanged(key);
    }
    if (completed) {
        for (const auto& key : keys) {
            if (!makeRoomForMidiClip(key)) {
                return false;
            }
        }
    }
    return true;
}

muse::RetVal<ClipKeyList> Au3ClipsInteraction::moveMidiClips(const ClipKeyList& keys, secs_t timeOffset, int trackOffset)
{
    using Result = muse::RetVal<ClipKeyList>;
    if (!std::isfinite(timeOffset.to_double())) {
        return Result::make_ret(make_ret(Err::TrackNotFound));
    }
    double adjusted = timeOffset.to_double();
    std::vector<MidiTrack*> tracks;
    for (auto* track : Au3TrackList::Get(projectRef()).Any<MidiTrack>()) {
        tracks.push_back(track);
    }
    struct Move {
        MidiTrack* source;
        MidiTrack* target;
        MidiClipData clip;
    };
    std::vector<Move> moves;
    std::set<ClipKey> unique;
    for (const auto& key : keys) {
        if (!unique.insert(key).second) {
            return Result::make_ret(make_ret(Err::TrackNotFound));
        }
        auto* source = midi::track(projectRef(), key.trackId);
        const auto* clip = source ? midi::clip(*source, key.itemId) : nullptr;
        const auto position = std::find(tracks.begin(), tracks.end(), source);
        const auto targetIndex = std::distance(tracks.begin(), position) + trackOffset;
        if (!clip || targetIndex < 0 || targetIndex >= static_cast<int64_t>(tracks.size())) {
            return Result::make_ret(make_ret(Err::TrackNotFound));
        }
        adjusted = std::max(adjusted, -clip->visibleStart() * 60.0 / source->GetTempo());
        moves.push_back({ source, tracks[targetIndex], *clip });
    }
    std::map<MidiTrack*, std::vector<MidiClipData> > previous, staged;
    for (const auto& move : moves) {
        for (auto* track : { move.source, move.target }) {
            previous.try_emplace(track, track->GetClips());
            staged.try_emplace(track, track->GetClips());
        }
    }
    for (const auto& move : moves) {
        std::erase_if(staged[move.source], [&](const auto& clip) {
            return clip.id == move.clip.id;
        });
    }
    ClipKeyList result;
    for (auto& move : moves) {
        auto& clip = move.clip;
        auto& destination = staged[move.target];
        const auto ratio = move.target->GetTempo() / move.source->GetTempo();
        clip.start = clip.start * ratio + adjusted * move.target->GetTempo() / 60.0;
        clip.length *= ratio;
        clip.trimLeft *= ratio;
        clip.trimRight *= ratio;
        for (auto& note : clip.notes) {
            note.start *= ratio;
            note.length *= ratio;
        }
        for (auto& event : clip.controllers) {
            event.beat *= ratio;
        }
        if (std::any_of(destination.begin(), destination.end(), [&](const auto& other) { return other.id == clip.id; })) {
            clip.id = 1;
            for (const auto& other : destination) {
                clip.id = std::max(clip.id, other.id + 1);
            }
        }
        result.push_back({ move.target->GetId(), clip.id });
        destination.push_back(std::move(clip));
    }
    for (const auto& [track, clips] : staged) {
        if (!MidiTrack::Create()->SetClips(clips)) {
            return Result::make_ret(make_ret(Err::TrackNotFound));
        }
    }
    const auto project = globalContext()->currentTrackeditProject();
    for (auto& [track, clips] : staged) {
        track->SetClips(std::move(clips));
    }
    for (const auto& [track, clips] : previous) {
        publish(project, *track, clips);
    }
    for (const auto& key : result) {
        const auto ret = makeRoomForMidiClip(key);
        if (!ret) {
            return Result::make_ret(ret);
        }
    }
    return Result::make_ok(result);
}

muse::Ret Au3ClipsInteraction::makeRoomForMidiClip(const ClipKey& key)
{
    auto* track = midi::track(projectRef(), key.trackId);
    const auto* source = track ? midi::clip(*track, key.itemId) : nullptr;
    if (!source) {
        return make_ret(Err::TrackNotFound);
    }
    const auto first = source->visibleStart(), last = source->visibleEnd();
    const auto previous = track->GetClips();
    std::vector<MidiClipData> clips;
    auto next = track->NextClipId();
    for (auto clip : previous) {
        if (clip.id == key.itemId || clip.visibleEnd() <= first || clip.visibleStart() >= last) {
            clips.push_back(std::move(clip));
            continue;
        }
        if (clip.visibleStart() < first) {
            auto left = clip;
            left.trimRight = clip.start + clip.length - first;
            clips.push_back(std::move(left));
        }
        if (clip.visibleEnd() > last) {
            clip.id = clip.visibleStart() < first ? next++ : clip.id;
            clip.trimLeft = last - clip.start;
            clips.push_back(std::move(clip));
        }
    }
    if (!track->SetClips(std::move(clips))) {
        return make_ret(Err::TrackNotFound);
    }
    publish(globalContext()->currentTrackeditProject(), *track, previous);
    return muse::make_ok();
}

bool Au3ClipsInteraction::duplicateMidiClips(const ClipKeyList& keys)
{
    auto& tracks = Au3TrackList::Get(projectRef());
    const auto project = globalContext()->currentTrackeditProject();
    std::vector<std::shared_ptr<MidiTrack> > copies;
    for (const auto* source : tracks.Any<const MidiTrack>()) {
        std::vector<MidiClipData> selected;
        for (auto clip : source->GetClips()) {
            if (std::find(keys.begin(), keys.end(), ClipKey { source->GetId(), clip.id }) != keys.end()) {
                clip.selected = false;
                selected.push_back(std::move(clip));
            }
        }
        if (selected.empty()) {
            continue;
        }
        auto copy = std::static_pointer_cast<MidiTrack>(source->Duplicate(::Track::DuplicateOptions {}.Backup()));
        if (!copy->SetClips(std::move(selected))) {
            return false;
        }
        copies.push_back(std::move(copy));
    }
    std::map<int64_t, size_t> originalCounts, copyCounts;
    for (const auto& trackId : project->trackIdList()) {
        for (const auto& clip : project->clipList(trackId)) {
            if (clip.groupId != -1) {
                ++originalCounts[clip.groupId];
            }
        }
    }
    for (const auto& copy : copies) {
        for (const auto& clip : copy->GetClips()) {
            if (clip.groupId != -1) {
                ++copyCounts[clip.groupId];
            }
        }
    }
    std::map<int64_t, int64_t> groups;
    int64_t searchFrom = 0;
    for (const auto& copy : copies) {
        auto clips = copy->GetClips();
        for (auto& clip : clips) {
            if (clip.groupId == -1) {
                continue;
            }
            const auto old = clip.groupId;
            if (copyCounts[old] != originalCounts[old]) {
                clip.groupId = -1;
                continue;
            }
            auto found = groups.find(old);
            if (found == groups.end()) {
                const auto next = project->createNewGroupID(searchFrom);
                found = groups.emplace(old, next).first;
                searchFrom = next + 1;
            }
            clip.groupId = found->second;
        }
        copy->SetClips(std::move(clips));
    }
    for (const auto& copy : copies) {
        tracks.Add(copy);
        project->notifyAboutTrackAdded(DomConverter::track(copy.get()));
        for (const auto& clip : copy->GetClips()) {
            project->notifyAboutClipAdded(DomConverter::clip(copy.get(), &clip));
        }
    }
    return !copies.empty();
}
