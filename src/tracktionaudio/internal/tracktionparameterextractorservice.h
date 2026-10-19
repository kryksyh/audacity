/*
 * Audacity: A Digital Audio Editor
 */
#pragma once

#include "effects/effects_base/iparameterextractorservice.h"

#include <unordered_map>
#include <cstdint>

namespace au::effects {
class TracktionParameterExtractorService : public IParameterExtractorService
{
public:
    explicit TracktionParameterExtractorService(EffectFamily family)
        : m_family(family) {}
    EffectFamily family() const override { return m_family; }

    ParameterInfoList extractParameters(EffectInstance* instance, EffectSettingsAccessPtr settingsAccess = nullptr) const override;

    ParameterInfo getParameter(EffectInstance* instance, const muse::String& parameterId) const override;

    double getParameterValue(EffectInstance* instance, const muse::String& parameterId) const override;

    bool setParameterValue(EffectInstance* instance, const muse::String& parameterId, double fullRangeValue,
                           EffectSettingsAccessPtr settingsAccess = nullptr) override;

    muse::String getParameterValueString(EffectInstance* instance, const muse::String& parameterId, double value) const override;

    void beginParameterGesture(EffectInstance* instance, const muse::String& parameterId, EffectSettingsAccessPtr settingsAccess) override;

    void endParameterGesture(EffectInstance* instance, const muse::String& parameterId) override;

    void onInstanceDestroyed(EffectInstance* instance) override;

    void beginParameterEditing(EffectInstance* instance, EffectSettingsAccessPtr settingsAccess) override;

    void endParameterEditing(EffectInstance* instance) override;

private:
    const EffectFamily m_family;
    mutable std::unordered_map<EffectInstance*, uint64_t> m_settingsCounters;
    std::unordered_map<EffectInstance*, EffectSettingsAccessPtr> m_gestureSettings;
    std::unordered_map<EffectInstance*, EffectSettingsAccessPtr> m_editSettings;
};
}
