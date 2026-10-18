/*
* Audacity: A Digital Audio Editor
*/
#pragma once

#include "au3-midi-track/MidiTrack.h"
#include "au3wrap/internal/domaccessor.h"

namespace au::trackedit::midi {
inline MidiTrack* track(au3::Au3Project& project, TrackId id)
{
    return dynamic_cast<MidiTrack*>(au3::DomAccessor::findTrack(project, au3::Au3TrackId(id)));
}

inline const MidiClipData* clip(const MidiTrack& track, int64_t id)
{
    for (const auto& clip : track.GetClips()) {
        if (clip.id == id) {
            return &clip;
        }
    }
    return nullptr;
}

template<typename Edit>
bool edit(MidiTrack& track, int64_t id, Edit&& edit)
{
    auto clips = track.GetClips();
    for (auto& clip : clips) {
        if (clip.id == id) {
            edit(clip);
            return track.SetClips(std::move(clips));
        }
    }
    return false;
}
}
