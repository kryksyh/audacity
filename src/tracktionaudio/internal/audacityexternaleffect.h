/*
* Audacity: A Digital Audio Editor
*/
#pragma once

#include <memory>

class EffectInstanceFactory;
struct EffectSettings;
class RealtimeEffectList;
class RealtimeEffectState;

namespace juce {
class MemoryBlock;
}

namespace tracktion {
inline namespace engine {
class Plugin;
class Edit;
class ExternalPlugin;
struct PluginCreationInfo;
}
}

namespace au::tracktionaudio {
bool supportsAudacityExternalEffect(const EffectInstanceFactory& effect);
tracktion::ExternalPlugin* createTracktionExternalProcessor(tracktion::Edit& edit, const EffectInstanceFactory& effect);
bool applyAudacityExternalEffectSettings(tracktion::ExternalPlugin& plugin, const EffectInstanceFactory& effect,
                                         const EffectSettings& settings);
bool applyAudacityExternalEffectParameters(tracktion::ExternalPlugin& plugin, const EffectInstanceFactory& effect,
                                           const EffectSettings& settings);
bool storeAudacityVst3EffectSettings(tracktion::ExternalPlugin& plugin, const EffectInstanceFactory& effect, EffectSettings& settings,
                                     const juce::MemoryBlock* state = nullptr);
bool storeAudacityAudioUnitEffectSettings(tracktion::ExternalPlugin& plugin, EffectSettings& settings,
                                          const juce::MemoryBlock* state = nullptr);
tracktion::Plugin* createAudacityExternalEffect(tracktion::PluginCreationInfo info, const std::shared_ptr<RealtimeEffectState>& state,
                                                const RealtimeEffectList& list, int channels, bool live);
}
