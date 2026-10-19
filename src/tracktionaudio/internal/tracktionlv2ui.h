/*
 * Audacity: A Digital Audio Editor
 */
#pragma once

#include <functional>
#include <memory>

namespace juce {
class AudioPluginInstance;
}

namespace au::tracktionaudio {
class TracktionLv2Ui
{
public:
    struct Callbacks {
        std::function<int(int, int)> resize;
        std::function<void()> failed;
        std::function<void(void*)> x11WindowReady;
    };

    static bool isSupported(const juce::AudioPluginInstance& processor);
    TracktionLv2Ui(juce::AudioPluginInstance& processor, Callbacks callbacks);
    ~TracktionLv2Ui();

    bool open();
    void close();
    bool idle();

private:
    struct Data;
    std::unique_ptr<Data> m_data;
};
}
