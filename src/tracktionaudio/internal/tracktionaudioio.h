/*
* Audacity: A Digital Audio Editor
*/
#pragma once

#include <string>

#include "au3-audio-devices/AudioIOBase.h"

namespace au::audio {
class IAudioEngine;
}

namespace au::tracktionaudio {
class TracktionAudioIO final : public AudioIOBase
{
public:
    static std::string init(audio::IAudioEngine& engine);
    static void deinit();
    static TracktionAudioIO& instance();

    ~TracktionAudioIO() override;

    void StopStream() override;
    void StopMonitoring() override;

    void setStream(const std::shared_ptr<AudacityProject>& owner, PaStream* stream, int token, double sampleRate);
    void clearStream();
    void setPaused(bool paused);
    void refreshDevices();

    float inputVolume() const;
    void setInputVolume(float volume);
    float softwareInputGain() const;
    float playbackVolume() const;
    void setPlaybackVolume(float volume);

private:
    explicit TracktionAudioIO(audio::IAudioEngine& engine);
    void closeMixer();

    audio::IAudioEngine& m_engine;
    bool m_initialised = false;
    std::atomic<float> m_inputVolume { 1.0f };
    std::atomic<float> m_playbackVolume { 1.0f };
};
}
