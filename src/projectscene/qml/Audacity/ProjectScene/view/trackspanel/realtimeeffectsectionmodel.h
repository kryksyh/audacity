/*
 * Audacity: A Digital Audio Editor
 */
#pragma once

#include <QtQml/qqmlregistration.h>

#include "iprojectsceneconfiguration.h"
#include "actions/iactionsdispatcher.h"
#include "actions/actionable.h"
#include "async/asyncable.h"
#include "modularity/ioc.h"
#include <QObject>

namespace au::projectscene {
class RealtimeEffectSectionModel : public QObject, public muse::actions::Actionable, public muse::async::Asyncable, public muse::Injectable
{
    Q_OBJECT
    QML_ELEMENT

    Q_PROPERTY(bool showEffectsSection READ prop_showEffectsSection WRITE prop_setShowEffectsSection NOTIFY showEffectsSectionChanged)

    muse::GlobalInject<IProjectSceneConfiguration> configuration;

    muse::Inject<muse::actions::IActionsDispatcher> dispatcher{ this };

public:
    explicit RealtimeEffectSectionModel(QObject* parent = nullptr);

    Q_INVOKABLE void load();

    bool prop_showEffectsSection() const;
    void prop_setShowEffectsSection(bool show);

signals:
    void showEffectsSectionChanged();
};
}
