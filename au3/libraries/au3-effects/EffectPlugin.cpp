/*!********************************************************************

  Audacity: A Digital Audio Editor

  @file EffectPlugin.cpp

  Paul Licameli split from EffectInterface.cpp

**********************************************************************/
#include "EffectPlugin.h"

EffectPlugin::~EffectPlugin() = default;

std::shared_ptr<EffectInstance> EffectPlugin::MakeProcessingInstance(std::shared_ptr<EffectInstance> current) const
{
    auto instance = ProcessingInstanceFactory::Call(*this);
    if (instance) {
        return instance;
    }
    return current ? current : MakeInstance();
}

const wxString EffectPlugin::kUserPresetIdent = wxT("User Preset:");
const wxString EffectPlugin::kFactoryPresetIdent = wxT("Factory Preset:");
const wxString EffectPlugin::kCurrentSettingsIdent = wxT("<Current Settings>");
const wxString EffectPlugin::kFactoryDefaultsIdent = wxT("<Factory Defaults>");

EffectInstanceEx::~EffectInstanceEx() = default;

bool EffectInstanceEx::Init()
{
    return true;
}
