/*
* Audacity: A Digital Audio Editor
*/
#pragma once

#include <optional>
#include <string>

#include "audioplugins/audiopluginstypes.h"

namespace juce {
class AudioPluginFormat;
class PluginDescription;
class String;
}

namespace tracktion {
inline namespace engine {
class Engine;
class PluginManager;
}
}

namespace au::tracktionaudio {
inline const muse::String TRACKTION_PLUGIN_DESCRIPTION_ATTRIBUTE = u"tracktionDescription";

struct Lv2PluginIdentifier {
    std::string pluginUri;
    std::string bundleUri;

    std::string serialise() const;
    static std::optional<Lv2PluginIdentifier> parse(const std::string& identifier);
};

void restoreTracktionPluginDescriptions(tracktion::Engine& engine, const muse::audioplugins::AudioPluginInfoList& plugins);
std::optional<juce::PluginDescription> findTracktionPluginDescription(tracktion::PluginManager& manager, juce::AudioPluginFormat& format,
                                                                      const juce::String& identifier, const juce::String& classID);
}
