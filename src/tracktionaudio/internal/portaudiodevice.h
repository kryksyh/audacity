/*
* Audacity: A Digital Audio Editor
*/
#pragma once

#include <memory>
#include <vector>
#include <chrono>

#include <juce_audio_devices/juce_audio_devices.h>

typedef void PaStream;

namespace au::tracktionaudio {
class AudioDeviceCallback
{
public:
    using TimePoint = std::chrono::steady_clock::time_point;

    virtual ~AudioDeviceCallback() = default;
    virtual void prepare(double sampleRate, int blockSize, int inputChannels, int outputChannels) = 0;
    virtual void processInput(const float* const* channels, int numChannels, int numSamples, TimePoint adcTime) = 0;
    virtual void processOutput(float* const* channels, int numChannels, int numSamples, TimePoint dacTime) = 0;
};

std::vector<std::unique_ptr<juce::AudioIODeviceType> > createPortAudioDeviceTypes(const juce::CriticalSection& callbackLock,
                                                                                  std::shared_ptr<AudioDeviceCallback> callback = {});
PaStream* portAudioStream(juce::AudioIODevice* device);
}
