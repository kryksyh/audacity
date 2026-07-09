/*
 * Audacity: A Digital Audio Editor
 */
#pragma once

#include <QtQml/qqmlregistration.h>
#include <QStringList>

#include "effects/effects_base/effectstypes.h"
#include "effects/effects_base/ieffectsconfiguration.h"
#include "effects/effects_base/ieffectsprovider.h"
#include "pluginhost/ipluginhostservice.h"
#include "async/asyncable.h"
#include "modularity/ioc.h"

#include <QObject>
#include <QVariantList>

namespace au::appshell {
class PluginPreferencesModel : public QObject, public muse::async::Asyncable
{
    Q_OBJECT
    QML_ELEMENT

    Q_PROPERTY(effects::EffectMenuOrganization effectMenuOrganization READ effectMenuOrganization NOTIFY effectMenuOrganizationChanged)

    Q_PROPERTY(QStringList lv2CustomPaths READ lv2CustomPaths NOTIFY lv2CustomPathsChanged)
    Q_PROPERTY(QStringList vst3CustomPaths READ vst3CustomPaths NOTIFY vst3CustomPathsChanged)

    Q_PROPERTY(bool lv2Supported READ lv2Supported CONSTANT)
    Q_PROPERTY(bool vst3Supported READ vst3Supported CONSTANT)

    muse::GlobalInject<effects::IEffectsConfiguration> effectsConfiguration;
    muse::GlobalInject<effects::IEffectsProvider> effectsProvider;
    muse::GlobalInject<au::pluginhost::IPluginHostService> pluginHostService;

public:
    explicit PluginPreferencesModel(QObject* parent = nullptr);

    effects::EffectMenuOrganization effectMenuOrganization() const;
    Q_INVOKABLE void setEffectMenuOrganization(effects::EffectMenuOrganization);

    QStringList lv2CustomPaths() const;
    QStringList vst3CustomPaths() const;

    bool lv2Supported() const;
    bool vst3Supported() const;

    Q_INVOKABLE void addLv2Path();
    Q_INVOKABLE void setLv2Path(int index, const QString& path);
    Q_INVOKABLE void removeLv2Path(int index);

    Q_INVOKABLE void addVst3Path();
    Q_INVOKABLE void setVst3Path(int index, const QString& path);
    Q_INVOKABLE void removeVst3Path(int index);

    Q_INVOKABLE bool pathExists(const QString& path) const;

    //! { id, name } for each loaded plugin that ships a plugin-level
    //! (not per-effect) configuration view.
    Q_INVOKABLE QVariantList pluginConfigViews() const;

    Q_INVOKABLE void init();

signals:
    void effectMenuOrganizationChanged();
    void lv2CustomPathsChanged();
    void vst3CustomPathsChanged();
};
}
