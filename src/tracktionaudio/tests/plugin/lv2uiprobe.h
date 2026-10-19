/*
 * Audacity: A Digital Audio Editor
 */
#pragma once

struct Lv2UiProbe {
    int opened = 0;
    int cleaned = 0;
    int shown = 0;
    int hidden = 0;
    int portEvents = 0;
    int atomEvents = 0;
    bool validFeatures = false;
    float gain = 0.0f;
    float atomValue = 0.0f;
    float sampleRate = 0.0f;
    void* instance = nullptr;
};
