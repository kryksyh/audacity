/*
* Audacity: A Digital Audio Editor
*/
#pragma once

#include <memory>
#include <string>

namespace tracktion {
inline namespace engine {
class Engine;
class WaveInputDevice;
}
}

namespace au::tracktionaudio {
class AudioSourceRegistry;
class AudacityEffectRegistry;
class AudioDeviceCallback;

class TracktionRuntime
{
public:
    TracktionRuntime();
    ~TracktionRuntime();

    void init(const std::string& cacheDirectory, const std::string& settingsDirectory);
    void deinit();

    std::string openAudioDevice(const std::string& host, const std::string& inputName, const std::string& outputName, int inputChannels,
                                int outputChannels, double sampleRate, std::shared_ptr<AudioDeviceCallback> callback = {});
    void closeAudioDevice();
    bool isAudioDeviceRunning() const;
    static void setInputGain(tracktion::WaveInputDevice& input, float gain);

    tracktion::Engine& engine() const;
    std::shared_ptr<AudioSourceRegistry> sources() const;
    std::shared_ptr<AudacityEffectRegistry> effects() const;
    std::shared_ptr<void> lifetime() const;

private:
    struct Data;
    std::shared_ptr<Data> m_data;
};
}
