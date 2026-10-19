/*
 * Audacity: A Digital Audio Editor
 */
#include "tracktionparameterextractorservice.h"

#include "tracktionaudio/internal/tracktioneffectinstance.h"

using namespace au::effects;
using namespace au::tracktionaudio;

ParameterInfoList TracktionParameterExtractorService::extractParameters(EffectInstance* instance,
                                                                        EffectSettingsAccessPtr settingsAccess) const
{
    auto* hosted = dynamic_cast<TracktionHostedEffectInstance*>(instance);
    if (!hosted) {
        return {};
    }
    if (settingsAccess) {
        const auto counter = settingsAccess->modificationCounter();
        const auto found = m_settingsCounters.find(instance);
        if (found == m_settingsCounters.end() || found->second != counter) {
            hosted->loadSettings(settingsAccess->Get(), false);
            m_settingsCounters[instance] = counter;
        }
    }
    return hosted->parameters();
}

ParameterInfo TracktionParameterExtractorService::getParameter(EffectInstance* instance, const muse::String& parameterId) const
{
    const auto* hosted = dynamic_cast<TracktionHostedEffectInstance*>(instance);
    return hosted ? hosted->parameter(parameterId) : ParameterInfo {};
}

double TracktionParameterExtractorService::getParameterValue(EffectInstance* instance, const muse::String& parameterId) const
{
    const auto* hosted = dynamic_cast<TracktionHostedEffectInstance*>(instance);
    return hosted ? hosted->parameterValue(parameterId) : 0.0;
}

bool TracktionParameterExtractorService::setParameterValue(EffectInstance* instance, const muse::String& parameterId,
                                                           double fullRangeValue, EffectSettingsAccessPtr)
{
    auto* hosted = dynamic_cast<TracktionHostedEffectInstance*>(instance);
    return hosted && hosted->setParameterValue(parameterId, fullRangeValue);
}

muse::String TracktionParameterExtractorService::getParameterValueString(EffectInstance* instance, const muse::String& parameterId,
                                                                         double value) const
{
    const auto* hosted = dynamic_cast<TracktionHostedEffectInstance*>(instance);
    return hosted ? hosted->parameterValueString(parameterId, value) : muse::String {};
}

void TracktionParameterExtractorService::beginParameterGesture(EffectInstance* instance, const muse::String& parameterId,
                                                               EffectSettingsAccessPtr settingsAccess)
{
    auto* hosted = dynamic_cast<TracktionHostedEffectInstance*>(instance);
    if (hosted) {
        m_gestureSettings[instance] = std::move(settingsAccess);
        hosted->beginParameterGesture(parameterId);
    }
}

void TracktionParameterExtractorService::endParameterGesture(EffectInstance* instance, const muse::String& parameterId)
{
    auto* hosted = dynamic_cast<TracktionHostedEffectInstance*>(instance);
    if (!hosted) {
        return;
    }
    hosted->endParameterGesture(parameterId);
    const auto found = m_gestureSettings.find(instance);
    if (found != m_gestureSettings.end()) {
        const auto access = found->second;
        if (access) {
            access->ModifySettings([hosted](EffectSettings& settings) {
                hosted->saveSettings(settings);
                return nullptr;
            });
            access->Flush();
            m_settingsCounters[instance] = access->modificationCounter();
        }
        m_gestureSettings.erase(found);
    }
}

void TracktionParameterExtractorService::onInstanceDestroyed(EffectInstance* instance)
{
    m_settingsCounters.erase(instance);
    m_gestureSettings.erase(instance);
    m_editSettings.erase(instance);
}

void TracktionParameterExtractorService::beginParameterEditing(EffectInstance* instance, EffectSettingsAccessPtr settingsAccess)
{
    auto* hosted = dynamic_cast<TracktionHostedEffectInstance*>(instance);
    if (hosted && settingsAccess) {
        hosted->loadSettings(settingsAccess->Get(), false);
        m_settingsCounters[instance] = settingsAccess->modificationCounter();
        m_editSettings[instance] = std::move(settingsAccess);
    }
}

void TracktionParameterExtractorService::endParameterEditing(EffectInstance* instance)
{
    auto* hosted = dynamic_cast<TracktionHostedEffectInstance*>(instance);
    const auto found = m_editSettings.find(instance);
    if (hosted && found != m_editSettings.end() && hosted->takeSettingsChanges()) {
        const auto access = found->second;
        access->ModifySettings([hosted](EffectSettings& settings) {
            hosted->saveSettings(settings);
            return nullptr;
        });
        access->Flush();
    }
    m_editSettings.erase(instance);
}
