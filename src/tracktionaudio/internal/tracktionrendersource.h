/*
* Audacity: A Digital Audio Editor
*/
#pragma once

#include <functional>
#include <memory>

namespace juce {
template<typename SampleType> class AudioBuffer;
}

namespace au::tracktionaudio {
class TracktionSession;

class TracktionRenderSource
{
public:
    struct Options {
        double startTime = 0.0;
        double endTime = 0.0;
        double sampleRate = 44100.0;
        int channels = 2;
        int blockSize = 512;
    };

    // Create and destroy the source on the main thread. Keep its event loop active during export.
    TracktionRenderSource(TracktionSession& session, Options options);
    ~TracktionRenderSource();

    int process(int maximumFrames);
    const juce::AudioBuffer<float>& buffer() const;
    double currentTime() const;
    void setCancellationCheck(std::function<bool()> check);

private:
    struct Data;
    std::unique_ptr<Data> m_data;
};
}
