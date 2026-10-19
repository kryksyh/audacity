/*
* Audacity: A Digital Audio Editor
*/
#pragma once

#include <map>
#include <memory>

#include <juce_core/juce_core.h>

class ChannelGroup;
class RealtimeEffectList;
class RealtimeEffectState;

namespace tracktion {
inline namespace engine {
class Engine;
}
}

namespace au::tracktionaudio {
inline constexpr auto AUDACITY_EFFECT_PLUGIN_TYPE = "audacity-effect-state";
inline constexpr auto AUDACITY_EFFECT_STATE_PROPERTY = "audacityEffectState";

class AudacityEffectRegistry
{
public:
    struct Binding;

    juce::String add(const std::shared_ptr<RealtimeEffectState>& state, const RealtimeEffectList& list, const ChannelGroup* group,
                     int channels, bool followListChanges);
    void remove(const juce::String& id);
    std::shared_ptr<Binding> find(const juce::String& id) const;

private:
    std::map<juce::String, std::shared_ptr<Binding> > m_bindings;
};

void registerAudacityEffectPlugins(tracktion::Engine& engine, const std::shared_ptr<AudacityEffectRegistry>& registry);
}
