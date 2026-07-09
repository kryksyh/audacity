/*
* Audacity: A Digital Audio Editor
*/
#pragma once

#include <QObject>

#include "modularity/ioc.h"

#include "effects/effects_base/ieffectinstancesregister.h"
#include "effects/effects_base/iparameterextractorregistry.h"

namespace au::effects {
//! Exposed to plugin-shipped QML views as the `effect` context object:
//! generic key-based access to the effect instance's parameter model.
class PluginEffectViewBridge : public QObject
{
    Q_OBJECT

    muse::GlobalInject<IEffectInstancesRegister> instancesRegister;
    muse::GlobalInject<IParameterExtractorRegistry> extractorRegistry;

public:
    explicit PluginEffectViewBridge(int instanceId, QObject* parent = nullptr);

    Q_INVOKABLE double value(const QString& key) const;
    Q_INVOKABLE void setValue(const QString& key, double value);

    Q_INVOKABLE QString stringValue(const QString& key) const;
    Q_INVOKABLE void setStringValue(const QString& key, const QString& value);

    //! Choice labels of an enum parameter
    Q_INVOKABLE QStringList choices(const QString& key) const;

    //! Generator duration (seconds), stored in the effect settings
    Q_INVOKABLE double duration() const;
    Q_INVOKABLE void setDuration(double value);

signals:
    void valuesChanged();

private:
    IParameterExtractorService* extractor() const;

    int m_instanceId = 0;
};
}
