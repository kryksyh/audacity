/*
* Audacity: A Digital Audio Editor
*/
#pragma once

#include <memory>
#include <vector>

#include "effects/effects_base/effectstypes.h"

class CommandParameters;
struct EffectSettings;

namespace juce {
class AudioPluginInstance;
class MemoryBlock;
}

namespace au::tracktionaudio {
struct TracktionLv2Settings {
    struct Layout;
    using State = std::vector<unsigned char>;
    std::shared_ptr<const Layout> layout;
    std::vector<float> values;
    std::shared_ptr<const State> state;
};

EffectSettings makeTracktionLv2Settings(juce::AudioPluginInstance& processor, const juce::MemoryBlock* state = nullptr);
bool saveTracktionLv2Settings(const EffectSettings& settings, CommandParameters& parameters);
bool loadTracktionLv2Settings(const CommandParameters& parameters, EffectSettings& settings);
bool applyTracktionLv2Settings(juce::AudioPluginInstance& processor, const EffectSettings& settings, bool restoreState);
bool captureTracktionLv2Settings(juce::AudioPluginInstance& processor, EffectSettings& settings, const juce::MemoryBlock* state = nullptr);
bool sameTracktionLv2State(const EffectSettings& first, const EffectSettings& second);
effects::ParameterInfo tracktionLv2Parameter(const juce::AudioPluginInstance& processor, const EffectSettings& layout,
                                             const muse::String& symbol);
effects::ParameterInfoList tracktionLv2Parameters(const juce::AudioPluginInstance& processor, const EffectSettings& layout);
bool setTracktionLv2Parameter(juce::AudioPluginInstance& processor, const EffectSettings& layout, const muse::String& symbol, double value);
muse::String tracktionLv2ParameterValueString(const juce::AudioPluginInstance& processor, const EffectSettings& layout,
                                              const muse::String& symbol, double value);
}
