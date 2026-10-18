/*
* Audacity: A Digital Audio Editor
*/
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

class MidiTrack;

struct MidiEvent {
    int64_t sample = 0;
    uint8_t status = 0;
    uint8_t data1 = 0;
    uint8_t data2 = 0;
    bool operator==(const MidiEvent&) const = default;
};

class MIDI_TRACK_API MidiSequence final
{
public:
    MidiSequence(const MidiTrack& track, double sampleRate, bool withEvents = true);
    const std::vector<MidiEvent>& Events() const { return mEvents; }
    void AppendEvents(int64_t start, size_t frames, std::vector<MidiEvent>& output) const;
    void Chase(int64_t start, std::vector<MidiEvent>& output) const;

private:
    std::vector<MidiEvent> mEvents;
};
