/*
* Audacity: A Digital Audio Editor
*/
#pragma once

#include <span>
#include "MidiTrack.h"

// Standard MIDI Files (SMF 0/1). Parsing is independent of the project, so a
// truncated or unsupported file cannot leave partially imported tracks behind.
struct MIDI_TRACK_API MidiFileData {
    bool success = false;
    wxString error;
    double tempo = 120.0;
    int numerator = 4;
    int denominator = 4;
    double duration = 0.0;
    bool hasSystemExclusive = false;
    std::vector<std::shared_ptr<MidiTrack> > tracks;
};

MIDI_TRACK_API MidiFileData ReadMidiFile(std::span<const uint8_t> bytes, double projectTempo = 0.0);
