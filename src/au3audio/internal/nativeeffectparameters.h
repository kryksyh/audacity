/*
* Audacity: A Digital Audio Editor
*/
#pragma once
#include <functional>
#include "audio/iinstrumenteditor.h"

class EffectInstanceFactory;
class EffectInstance;
struct EffectSettings;

namespace au::au3audio {
void registerNativeEffectAutomation();
std::vector<audio::InstrumentParameter> nativeEffectParameters(const EffectInstanceFactory& effect, const EffectSettings& settings);
struct NativeEffectParameterBinding {
    std::function<void(double)> set;
    bool beforeProcessStart = false;
};
NativeEffectParameterBinding bindNativeEffectParameter(const EffectInstanceFactory& effect, EffectSettings& settings,
                                                       EffectInstance& instance, const std::string& id, double sampleRate);
}
