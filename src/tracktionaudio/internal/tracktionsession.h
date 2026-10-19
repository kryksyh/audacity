/*
* Audacity: A Digital Audio Editor
*/
#pragma once

#include <memory>
#include <vector>
#include <optional>

class TrackList;
class WaveTrack;
class MidiTrack;
class Track;
struct TransportSequences;
namespace MixerOptions {
class Downmix;
}

namespace tracktion {
inline namespace engine {
class Edit;
class AudioTrack;
}
}

namespace au::tracktionaudio {
class TracktionRuntime;
class TracktionTrackMeters;
struct AudacityPlaybackControls;

class TracktionSession
{
public:
    enum class Role {
        Playback, Render
    };
    enum class Selection {
        All, Selected
    };
    enum class PlaybackLoopUpdate {
        Start, RegionChanged
    };

    TracktionSession(TracktionRuntime& runtime, TrackList& tracks, Role role, Selection selection = Selection::All);
    TracktionSession(TracktionRuntime& runtime, TrackList& tracks, const TransportSequences& sequences, Role role);
    TracktionSession(TracktionRuntime& runtime, TrackList& tracks, const std::vector<const WaveTrack*>& sources, Role role,
                     bool trackEffects, bool masterEffects, const std::vector<const MidiTrack*>& midiSources = {}, bool automation = true,
                     bool routing = true);
    ~TracktionSession();

    tracktion::Edit& edit() const;
    bool hasRouting() const;
    tracktion::AudioTrack* instrumentTrack(const MidiTrack& source) const;
    void configureRender(unsigned channels, const MixerOptions::Downmix* channelMap, bool averageChannels);
    bool startPlayback();
    void setPlaybackEnd(std::optional<double> time);
    bool updatePlaybackLoop(bool enabled, double startTime, double endTime, PlaybackLoopUpdate update);
    void setPlaybackVolume(float volume);
    void setPlaybackSpeed(double ratio);
    void setLeadInEnd(const TransportSequences& sequences, double time);
    std::optional<int> playbackEndSampleOffset() const;
    std::shared_ptr<const AudacityPlaybackControls> playbackControls() const;
    std::shared_ptr<TracktionTrackMeters> trackMeters() const;

private:
    void initialise(TracktionRuntime& runtime, TrackList& tracks, Role role, const std::vector<const WaveTrack*>& sources,
                    bool trackEffects = true, bool masterEffects = true, const std::vector<const MidiTrack*>& midiSources = {},
                    bool automation = true, bool routing = true);

    struct Data;
    std::unique_ptr<Data> m_data;
};
}
