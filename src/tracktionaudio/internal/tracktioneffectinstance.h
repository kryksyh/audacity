/*
* Audacity: A Digital Audio Editor
*/
#pragma once

#include <memory>
#include <functional>

#include "au3-effects/PerTrackEffect.h"
#include "effects/effects_base/effectstypes.h"

class EffectInstance;
class EffectInstanceFactory;
struct ComponentInstanceRecord;

namespace juce {
class MemoryBlock;
}

namespace tracktion {
inline namespace engine {
class ExternalPlugin;
}
}

namespace Steinberg::Vst {
class IComponent;
class IEditController;
}

namespace au::tracktionaudio {
class TracktionRuntime;

class TracktionHostedEffectInstance : public PerTrackEffect::Instance
{
public:
    using PerTrackEffect::Instance::Instance;

    virtual const std::string& name() const = 0;
    virtual Steinberg::Vst::IComponent* vstComponent() const = 0;
    virtual Steinberg::Vst::IEditController* vstController() const = 0;
    virtual ComponentInstanceRecord* audioUnit() const = 0;
    virtual bool openAudioUnitEditor() = 0;
    virtual void closeAudioUnitEditor() = 0;
    virtual bool prepareLv2Editor(double sampleRate) = 0;
    virtual bool loadSettings(const EffectSettings& settings, bool defaults) = 0;
    virtual bool loadParameterSettings(const EffectSettings& settings) = 0;
    virtual bool loadVst3PresetFile(const std::string& path) = 0;
    virtual bool saveSettings(EffectSettings& settings) = 0;
    virtual bool savePlaybackSettings(EffectSettings& settings) = 0;
    virtual void beginPlayback(std::function<void()> editorIdleService = {}) = 0;
    virtual void endPlayback() = 0;
    virtual void processEditorIdle() = 0;
    virtual bool takeSettingsChanges() = 0;
    virtual bool hasSettingsChanges() const = 0;
    virtual bool settingsMatchProcessor(const EffectSettings& settings) const = 0;
    virtual uint64_t stateRevision() const = 0;
    virtual tracktion::ExternalPlugin& tracktionPlugin() const = 0;
    virtual const juce::MemoryBlock& defaultState() const = 0;
    virtual effects::ParameterInfoList parameters() const = 0;
    virtual effects::ParameterInfo parameter(const muse::String& id) const = 0;
    virtual double parameterValue(const muse::String& id) const = 0;
    virtual bool setParameterValue(const muse::String& id, double value) = 0;
    virtual muse::String parameterValueString(const muse::String& id, double normalized) const = 0;
    virtual void beginParameterGesture(const muse::String& id) = 0;
    virtual void endParameterGesture(const muse::String& id) = 0;
};

std::shared_ptr<EffectInstance> createTracktionEffectInstance(TracktionRuntime& runtime, const EffectInstanceFactory& effect);
}
