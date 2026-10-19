/*
* Audacity: A Digital Audio Editor
*/
#include <tracktion_engine/tracktion_engine.h>

#include "tracktionpluginloader.h"

#include <QEventLoop>
#include <QTimer>
#include <stdexcept>

#if JUCE_MAC
#include <CoreFoundation/CoreFoundation.h>
#endif

namespace {
bool waitForPlugin(const std::function<bool()>& ready)
{
    TRACKTION_ASSERT_MESSAGE_THREAD
    if (ready()) {
        return true;
    }
    QEventLoop loop;
    QTimer timer;
    QObject::connect(&timer, &QTimer::timeout, &loop, [&] {
#if JUCE_MAC
        CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.0, true);
#endif
        if (ready()) {
            loop.quit();
        }
    });
    timer.start(5);
    loop.exec(QEventLoop::ExcludeUserInputEvents);
    return ready();
}
}

std::unique_ptr<juce::AudioPluginInstance> au::tracktionaudio::loadTracktionPlugin(juce::AudioPluginFormatManager& formats,
                                                                                   const juce::PluginDescription& description,
                                                                                   juce::String& error)
{
    struct State {
        std::unique_ptr<juce::AudioPluginInstance> result;
        juce::String error;
        bool complete = false;
    };
    const auto state = std::make_shared<State>();
    formats.createPluginInstanceAsync(description, 44100.0, 512, [state](auto instance, const auto& message) {
        state->result = std::move(instance);
        state->error = message;
        state->complete = true;
    });
    if (!waitForPlugin([&] { return state->complete; })) {
        error = "The plug-in load stopped.";
        return {};
    }
    error = state->error;
    return std::move(state->result);
}

void au::tracktionaudio::waitForTracktionPluginLoad(tracktion::ExternalPlugin& plugin)
{
    if (!waitForPlugin([&] { return !plugin.isInitialisingAsync(); })) {
        throw std::runtime_error("The plug-in load stopped.");
    }
    if (!plugin.getAudioPluginInstance()) {
        throw std::runtime_error(plugin.getLoadError().toStdString());
    }
}
