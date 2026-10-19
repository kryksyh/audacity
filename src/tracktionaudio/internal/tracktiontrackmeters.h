/*
* Audacity: A Digital Audio Editor
*/
#pragma once

#include <memory>

#include "au3-audio-devices/IMeterSender.h"

namespace tracktion {
inline namespace engine {
class AudioTrack;
}
}

namespace au::tracktionaudio {
class TracktionTrackMeters
{
public:
    TracktionTrackMeters();
    ~TracktionTrackMeters();

    // Change tracks on the main thread while the audio callback is stopped or locked.
    void addTrack(tracktion::AudioTrack& track, int64_t id, unsigned channels);
    void send(IMeterSender* sender, size_t frames, TimePoint dacTime);
    void clear();

private:
    struct Data;
    std::unique_ptr<Data> m_data;
};
}
