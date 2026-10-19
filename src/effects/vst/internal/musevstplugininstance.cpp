/*
* Audacity: A Digital Audio Editor
*/
#include "musevstplugininstance.h"

#include "tracktionaudio/internal/tracktioneffectinstance.h"

// from muse
#include "vst/vsttypes.h"

#include "log.h"

using namespace au::effects;

MuseVstPluginInstance::MuseVstPluginInstance(const EffectId& effectId,
                                             const EffectInstanceId& instanceId,
                                             std::shared_ptr<tracktionaudio::TracktionHostedEffectInstance> instance)
    : m_resourceId(effectId.toStdString()),
    m_instanceId(instanceId),
    m_instance(std::move(instance))
{
}

const muse::audio::AudioResourceId& MuseVstPluginInstance::resourceId() const
{
    return m_resourceId;
}

const std::string& MuseVstPluginInstance::name() const
{
    return m_instance->name();
}

muse::vst::VstPluginInstanceId MuseVstPluginInstance::id() const
{
    return m_instanceId;
}

bool MuseVstPluginInstance::isLoaded() const
{
    return m_instance->vstController() && m_instance->vstComponent();
}

muse::async::Notification MuseVstPluginInstance::loadingCompleted() const
{
    static muse::async::Notification n;
    return n;
}

muse::vst::PluginViewPtr MuseVstPluginInstance::createView() const
{
    auto controller = this->controller();
    if (!controller) {
        return nullptr;
    }

    return owned(controller->createView(muse::vst::PluginEditorViewType::kEditor));
}

muse::vst::PluginControllerPtr MuseVstPluginInstance::controller() const
{
    return m_instance->vstController();
}

muse::vst::PluginComponentPtr MuseVstPluginInstance::component() const
{
    return m_instance->vstComponent();
}

muse::vst::PluginMidiMappingPtr MuseVstPluginInstance::midiMapping() const
{
    NOT_IMPLEMENTED;
    return nullptr;
}

void MuseVstPluginInstance::updatePluginConfig(const muse::audio::AudioUnitConfig& config)
{
    UNUSED(config);
}

void MuseVstPluginInstance::refreshConfig()
{
}

muse::async::Channel<muse::audio::AudioUnitConfig> MuseVstPluginInstance::pluginSettingsChanged() const
{
    static muse::async::Channel<muse::audio::AudioUnitConfig> n;
    return n;
}
