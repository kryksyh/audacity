#include "framework/global/log.h"
#include "global/types/translatablestring.h"

#include "au3wrap/internal/domaccessor.h"

#include "au3-wave-track/WaveTrack.h"
#include "au3-midi-track/MidiTrack.h"

#include "au3trackplaybackcontrol.h"

using namespace au::playback;
using namespace au::au3;

Au3Project& Au3TrackPlaybackControl::projectRef() const
{
    Au3Project* project = reinterpret_cast<Au3Project*>(globalContext()->currentProject()->au3ProjectPtr());
    return *project;
}

volume_dbfs_t Au3TrackPlaybackControl::volume(long trackId) const
{
    const auto* track = DomAccessor::findTrack(projectRef(), Au3TrackId(trackId));
    const auto* midi = dynamic_cast<const MidiTrack*>(track);
    if (midi) {
        return LINEAR_TO_DB(midi->GetGain());
    }
    const auto* wave = dynamic_cast<const Au3WaveTrack*>(track);
    return wave ? LINEAR_TO_DB(wave->GetVolume()) : 0.0;
}

void Au3TrackPlaybackControl::setVolume(long trackId, volume_dbfs_t vol, bool completed)
{
    auto* track = DomAccessor::findTrack(projectRef(), Au3TrackId(trackId));
    const double gain = vol > -60 ? DB_TO_LINEAR(vol) : 0;
    auto* midi = dynamic_cast<MidiTrack*>(track);
    auto* wave = dynamic_cast<Au3WaveTrack*>(track);
    if (midi) {
        midi->SetGain(gain);
    } else if (wave) {
        wave->SetVolume(gain);
    } else {
        return;
    }

    if (completed) {
        projectHistory()->pushHistoryState(muse::trc("playback", "Moved volume slider"),
                                           muse::trc("playback", "Volume"),
                                           trackedit::UndoPushType::CONSOLIDATE);
    }
    return;
}

pan_t Au3TrackPlaybackControl::pan(long trackId) const
{
    const auto* track = DomAccessor::findTrack(projectRef(), Au3TrackId(trackId));
    const auto* midi = dynamic_cast<const MidiTrack*>(track);
    if (midi) {
        return midi->GetPan();
    }
    const auto* wave = dynamic_cast<const Au3WaveTrack*>(track);
    return wave ? wave->GetPan() : 0.0;
}

void Au3TrackPlaybackControl::setPan(long trackId, au::audio::pan_t pan, bool completed)
{
    auto* track = DomAccessor::findTrack(projectRef(), Au3TrackId(trackId));
    auto* midi = dynamic_cast<MidiTrack*>(track);
    auto* wave = dynamic_cast<Au3WaveTrack*>(track);
    if (midi) {
        midi->SetPan(pan);
    } else if (wave) {
        wave->SetPan(pan);
    } else {
        return;
    }

    if (completed) {
        projectHistory()->pushHistoryState(muse::trc("playback", "Moved pan dial"),
                                           //: Undo history entry name; shown after Undo and Redo in the Edit menu
                                           muse::trc("playback", "Pan"),
                                           trackedit::UndoPushType::CONSOLIDATE);
    }
}

bool Au3TrackPlaybackControl::setMuteOrSolo(long trackId, bool value, MuteOrSolo which, bool exclusive)
{
    auto* track = dynamic_cast<PlayableTrack*>(DomAccessor::findTrack(projectRef(), Au3TrackId(trackId)));
    if (!track) {
        return false;
    }

    auto& tracks = TrackList::Get(projectRef());

    auto get = [which](auto* t) {
        return which == MuteOrSolo::Solo ? t->GetSolo() : t->GetMute();
    };
    auto set = [which](auto* t, bool v) {
        which == MuteOrSolo::Solo ? t->SetSolo(v) : t->SetMute(v);
    };

    bool changed = false;
    if (exclusive) {
        value = true;

        for (auto playable : tracks.Any<PlayableTrack>().Excluding(track)) {
            if (get(playable)) {
                set(playable, false);
                changed = true;
            }
        }
    }

    if (get(track) != value) {
        set(track, value);
        changed = true;
    }

    return changed;
}

void Au3TrackPlaybackControl::setSolo(long trackId, bool solo, bool exclusive)
{
    if (setMuteOrSolo(trackId, solo, MuteOrSolo::Solo, exclusive)) {
        onMuteOrSoloChanged();
    }
}

bool Au3TrackPlaybackControl::solo(long trackId) const
{
    auto* track = dynamic_cast<PlayableTrack*>(DomAccessor::findTrack(projectRef(), Au3TrackId(trackId)));
    if (!track) {
        return false;
    }

    return track->GetSolo();
}

void Au3TrackPlaybackControl::setMuted(long trackId, bool mute, bool exclusive)
{
    if (setMuteOrSolo(trackId, mute, MuteOrSolo::Mute, exclusive)) {
        onMuteOrSoloChanged();
    }
}

void Au3TrackPlaybackControl::setMuted(const trackedit::TrackIdList& trackIds, bool mute)
{
    // Apply to every track first (no short-circuiting), then notify and modify the history once.
    bool changed = false;
    for (const trackedit::TrackId trackId : trackIds) {
        if (setMuteOrSolo(trackId, mute, MuteOrSolo::Mute, false)) {
            changed = true;
        }
    }

    if (changed) {
        onMuteOrSoloChanged();
    }
}

bool Au3TrackPlaybackControl::muted(long trackId) const
{
    auto* track = dynamic_cast<PlayableTrack*>(DomAccessor::findTrack(projectRef(), Au3TrackId(trackId)));
    if (!track) {
        return false;
    }

    return track->GetMute();
}

void Au3TrackPlaybackControl::onMuteOrSoloChanged()
{
    for (const auto& playable : TrackList::Get(projectRef()).Any<PlayableTrack>()) {
        m_muteOrSoloChanged.send(playable->GetId());
    }

    projectHistory()->modifyState();
    projectHistory()->markUnsaved();
}

muse::async::Channel<long> Au3TrackPlaybackControl::muteOrSoloChanged() const
{
    return m_muteOrSoloChanged;
}
