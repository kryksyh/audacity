/*
 * Audacity: A Digital Audio Editor
 */
#pragma once

#include <QObject>
#include <QTimer>
#include <QtQml/qqmlregistration.h>

#include "modularity/ioc.h"
#include "context/iglobalcontext.h"
#include "trackedit/iprojecthistory.h"

#include "effects/effects_base/view/abstracteffectviewmodel.h"
#include "effects/effects_base/ieffectinstancesregister.h"
#include "effects/effects_base/ieffectexecutionscenario.h"
#include "effects/effects_base/ieffectsprovider.h"
#include "effects/effects_base/irealtimeeffectservice.h"

class EffectSettingsAccess;

namespace au::tracktionaudio {
class TracktionHostedEffectInstance;
}

namespace au::effects {
class AudioUnitViewModel : public AbstractEffectViewModel
{
    Q_OBJECT
    Q_PROPERTY(QString title READ title NOTIFY titleChanged FINAL)

    muse::GlobalInject<IEffectInstancesRegister> instancesRegister;
    muse::GlobalInject<IEffectsProvider> effectsProvider;

    muse::ContextInject<au::context::IGlobalContext> globalContext{ this };
    muse::ContextInject<IEffectExecutionScenario> executionScenario{ this };
    muse::ContextInject<IRealtimeEffectService> realtimeEffectService{ this };
    muse::ContextInject<trackedit::IProjectHistory> projectHistory{ this };

public:
    AudioUnitViewModel(QObject* parent, int instanceId);
    ~AudioUnitViewModel() override;

    Q_INVOKABLE void deinit();

    QString title() const;
    void setTitle(const QString& newTitle);

signals:
    void titleChanged();

private:
    void doInit() override;
    void doStartPreview() override;
    void doStopPreview() override;

    void settingsToView();
    bool settingsFromView(bool complete = true);
    void checkSettingChangesFromUi(bool complete = false);

    std::shared_ptr<tracktionaudio::TracktionHostedEffectInstance> m_instance;
    std::shared_ptr<EffectSettingsAccess> m_settingsAccess;
    bool m_settingsLoaded = false;

    QString m_title;
    QTimer m_settingsTimer;
};

class AudioUnitViewModelFactory : public EffectViewModelFactory<AudioUnitViewModel>
{
    Q_OBJECT
    QML_ELEMENT;
    QML_SINGLETON;
};
}
