/*
* Audacity: A Digital Audio Editor
*/
#include "plugineffectviewbridge.h"

#include "au3-components/EffectInterface.h"

using namespace au::effects;

PluginEffectViewBridge::PluginEffectViewBridge(int instanceId, QObject* parent)
    : QObject(parent), m_instanceId(instanceId)
{
}

IParameterExtractorService* PluginEffectViewBridge::extractor() const
{
    return extractorRegistry()->extractorForFamily(EffectFamily::Plugin);
}

double PluginEffectViewBridge::value(const QString& key) const
{
    const auto instance = instancesRegister()->instanceById(m_instanceId);
    IParameterExtractorService* service = extractor();
    if (!instance || !service) {
        return 0.0;
    }
    return service->getParameterValue(instance.get(), muse::String::fromQString(key));
}

void PluginEffectViewBridge::setValue(const QString& key, double value)
{
    const auto instance = instancesRegister()->instanceById(m_instanceId);
    IParameterExtractorService* service = extractor();
    if (!instance || !service) {
        return;
    }
    service->setParameterValue(instance.get(), muse::String::fromQString(key), value,
                               instancesRegister()->settingsAccessById(m_instanceId));
    emit valuesChanged();
}

QString PluginEffectViewBridge::stringValue(const QString& key) const
{
    const auto instance = instancesRegister()->instanceById(m_instanceId);
    IParameterExtractorService* service = extractor();
    if (!instance || !service) {
        return {};
    }
    return service->getParameter(instance.get(), muse::String::fromQString(key)).currentValueString.toQString();
}

void PluginEffectViewBridge::setStringValue(const QString& key, const QString& value)
{
    const auto instance = instancesRegister()->instanceById(m_instanceId);
    IParameterExtractorService* service = extractor();
    if (!instance || !service) {
        return;
    }
    service->setParameterStringValue(instance.get(), muse::String::fromQString(key),
                                     muse::String::fromQString(value),
                                     instancesRegister()->settingsAccessById(m_instanceId));
    emit valuesChanged();
}

QStringList PluginEffectViewBridge::choices(const QString& key) const
{
    const auto instance = instancesRegister()->instanceById(m_instanceId);
    IParameterExtractorService* service = extractor();
    if (!instance || !service) {
        return {};
    }
    QStringList result;
    for (const muse::String& label : service->getParameter(instance.get(), muse::String::fromQString(key)).enumValues) {
        result << label.toQString();
    }
    return result;
}

double PluginEffectViewBridge::duration() const
{
    const auto access = instancesRegister()->settingsAccessById(m_instanceId);
    return access ? access->Get().extra.GetDuration() : 0.0;
}

void PluginEffectViewBridge::setDuration(double value)
{
    const auto access = instancesRegister()->settingsAccessById(m_instanceId);
    if (!access || access->Get().extra.GetDuration() == value) {
        return;
    }
    access->ModifySettings([value](::EffectSettings& s) {
        s.extra.SetDuration(value);
        return nullptr;
    });
    emit valuesChanged();
}
