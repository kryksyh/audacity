/*
 * Audacity: A Digital Audio Editor
 */
#pragma once

#include "effects/effects_base/view/abstracteffectviewmodel.h"
#include "effects/effects_base/ieffectinstancesregister.h"
#include "effects/effects_base/ieffectexecutionscenario.h"
#include "effects/effects_base/ieffectsprovider.h"
#include "effects/effects_base/irealtimeeffectservice.h"
#include "context/iglobalcontext.h"
#include "trackedit/iprojecthistory.h"

#include <QTimer>

namespace au::tracktionaudio {
class TracktionHostedEffectInstance;
class TracktionLv2Ui;
}

namespace au::effects {
class Lv2ViewModel : public AbstractEffectViewModel
{
    Q_OBJECT
    Q_PROPERTY(QString title READ title WRITE setTitle NOTIFY titleChanged FINAL)
    Q_PROPERTY(QString unsupportedUiReason READ unsupportedUiReason NOTIFY unsupportedUiReasonChanged FINAL)

    muse::GlobalInject<IEffectInstancesRegister> instancesRegister;
    muse::GlobalInject<IEffectsProvider> effectsProvider;

    muse::ContextInject<context::IGlobalContext> globalContext { this };
    muse::ContextInject<IEffectExecutionScenario> executionScenario { this };
    muse::ContextInject<IRealtimeEffectService> realtimeEffectService { this };
    muse::ContextInject<trackedit::IProjectHistory> projectHistory { this };

public:
    Lv2ViewModel(QObject* parent, int instanceId, const QString& effectState);
    ~Lv2ViewModel() override;

    Q_INVOKABLE void deinit();
    QString unsupportedUiReason() const;
    QString title() const;
    void setTitle(const QString& title);

signals:
    void titleChanged();
    void externalUiClosed();
    void unsupportedUiReasonChanged();
    void vendorUiFailed();

private:
    void doInit() override;
    void doStartPreview() override;
    void doStopPreview() override;
    void settingsToView();
    bool settingsFromView(bool complete);
    void checkSettingChangesFromUi(bool complete = false);
    void makeDirtyIfSettingsChanged();
    void onIdle();

    QString m_title;
    const RealtimeEffectStatePtr m_effectState;
    std::shared_ptr<tracktionaudio::TracktionHostedEffectInstance> m_instance;
    EffectSettingsAccessPtr m_settingsAccess;
    std::shared_ptr<tracktionaudio::TracktionLv2Ui> m_pluginUi;
    QTimer m_uiTimer;
    QTimer m_settingsTimer;
    bool m_settingsChanged = false;
    bool m_settingsLoaded = false;
};

class Lv2ViewModelFactory : public QObject
{
    Q_OBJECT
public:
    ~Lv2ViewModelFactory() override = default;
    Q_INVOKABLE Lv2ViewModel* createModel(QObject* parent, int instanceId, const QString& effectState) const
    {
        return new Lv2ViewModel(parent, instanceId, effectState);
    }
};
}
