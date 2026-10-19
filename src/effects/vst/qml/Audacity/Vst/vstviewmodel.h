/*
* Audacity: A Digital Audio Editor
*/
#pragma once

#include <QObject>
#include <QTimer>
#include <QtQml/qqmlregistration.h>

#include "modularity/ioc.h"
#include "trackedit/iprojecthistory.h"
#include "effects/effects_base/ieffectinstancesregister.h"
#include "effects/effects_base/ieffectexecutionscenario.h"
#include "effects/effects_base/irealtimeeffectservice.h"
#include "effects/effects_base/effectstypes.h"
#include "effects/effects_base/view/abstracteffectviewmodel.h"

namespace au::tracktionaudio {
class TracktionHostedEffectInstance;
}
class EffectSettingsAccess;
namespace au::effects {
class VstViewModel : public AbstractEffectViewModel
{
    Q_OBJECT

public:
    muse::GlobalInject<IEffectInstancesRegister> instancesRegister;

    muse::ContextInject<IEffectExecutionScenario> executionScenario{ this };
    muse::ContextInject<IRealtimeEffectService> realtimeEffectService{ this };
    muse::ContextInject<trackedit::IProjectHistory> projectHistory{ this };

public:
    VstViewModel(QObject* parent, int instanceId);
    ~VstViewModel() override;

private:
    void doInit() override;
    void doStartPreview() override;
    void doStopPreview() override;

    std::shared_ptr<EffectSettingsAccess> settingsAccess() const;
    void settingsToView();
    bool settingsFromView(bool complete = true);
    void checkSettingChangesFromUiWhileIdle();
    void checkSettingChangesFromUi(bool forceCommitting);

    std::shared_ptr<tracktionaudio::TracktionHostedEffectInstance> m_instance;
    std::shared_ptr<EffectSettingsAccess> m_settingsAccess;
    bool m_settingsLoaded = false;
    QTimer m_settingUpdateTimer;
};

class VstViewModelFactory : public EffectViewModelFactory<VstViewModel>
{
    Q_OBJECT
    QML_ELEMENT;
    QML_SINGLETON;
};
}
