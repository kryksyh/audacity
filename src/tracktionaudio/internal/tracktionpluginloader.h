/*
* Audacity: A Digital Audio Editor
*/
#pragma once

#include <memory>

namespace juce {
class AudioPluginFormatManager;
class AudioPluginInstance;
class PluginDescription;
class String;
}

namespace tracktion {
inline namespace engine {
class ExternalPlugin;
}
}

namespace au::tracktionaudio {
std::unique_ptr<juce::AudioPluginInstance> loadTracktionPlugin(juce::AudioPluginFormatManager& formats,
                                                               const juce::PluginDescription& description, juce::String& error);
void waitForTracktionPluginLoad(tracktion::ExternalPlugin& plugin);
}
