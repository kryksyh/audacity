/*
* Audacity: A Digital Audio Editor
*/
#pragma once

// Processing-thread snapshot. Time refers to the input audio's project time,
// including negative pre-roll and upstream delay compensation.
struct EffectProcessingContext {
    double time = 0.0;
    double sampleRate = 44100.0;
    double tempo = 120.0;
    int numerator = 4;
    int denominator = 4;
    double speed = 1.0;
    bool playing = true;
    bool cycling = false;
    // Project seconds; hosts convert these to musical beats for native APIs.
    double cycleStart = 0.0;
    double cycleEnd = 0.0;
};
