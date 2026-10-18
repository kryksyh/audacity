/*
* Audacity: A Digital Audio Editor
*/
#pragma once

// MIDI Association RP-015: https://midi.org/response-to-reset-all-controllers
// -1 means that Reset All Controllers must retain this value. Parameter
// transaction data/increment messages must never be replayed as zero values.
constexpr int MidiControllerResetValue(int control) noexcept
{
    if (control < 0 || control >= 120 || control == 0 || control == 32
        || control == 7 || control == 39 || control == 10 || control == 42
        || control == 6 || control == 38 || control == 96 || control == 97
        || (control >= 70 && control <= 79) || (control >= 91 && control <= 95)) return -1;
    return control == 11 || (control >= 98 && control <= 101) ? 127 : 0;
}

constexpr bool MidiControllerResetRequired(int control) noexcept
{
    return control == 1 || control == 11 || (control >= 64 && control <= 67)
           || (control >= 98 && control <= 101);
}
