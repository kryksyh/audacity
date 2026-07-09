/*
* Audacity: A Digital Audio Editor
*/
#pragma once

#include "effects/effects_base/iparameterextractorservice.h"

namespace au::effects {
//! Parameter extraction for effects of dynamically loaded plugins.
//! Works for any AU3 stateful effect that implements VisitSettings
//! (the standard EffectParameter / CapturedParameters pattern).
class PluginParameterExtractorService : public IParameterExtractorService
{
public:
    EffectFamily family() const override { return EffectFamily::Plugin; }

    ParameterInfoList extractParameters(EffectInstance* instance, EffectSettingsAccessPtr settingsAccess = nullptr) const override;

    ParameterInfo getParameter(EffectInstance* instance, const muse::String& parameterId) const override;

    double getParameterValue(EffectInstance* instance, const muse::String& parameterId) const override;

    bool setParameterValue(EffectInstance* instance, const muse::String& parameterId, double fullRangeValue,
                           EffectSettingsAccessPtr settingsAccess = nullptr) override;

    bool setParameterStringValue(EffectInstance* instance, const muse::String& parameterId, const muse::String& stringValue,
                                 EffectSettingsAccessPtr settingsAccess = nullptr) override;

    muse::String getParameterValueString(EffectInstance* instance, const muse::String& parameterId, double value) const override;
};
}
