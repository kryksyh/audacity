#[[
MIDI tracks in the Audacity document.
]]
set(TARGET au3-midi-track)
set(SOURCES MidiTrack.cpp MidiTrack.h MidiSequence.cpp MidiSequence.h MidiController.h MidiFile.cpp MidiFile.h)
set(LIBRARIES
PUBLIC
    au3-playable-track
    au3-mixer
PRIVATE
    au3-project
    au3-realtime-effects
    au3-stretching-sequence
    au3-strings
    wxBase
)
audacity_library(${TARGET} "${SOURCES}" "${LIBRARIES}" "" "")
