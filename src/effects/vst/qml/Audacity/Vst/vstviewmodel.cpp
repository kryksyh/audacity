/*
* Audacity: A Digital Audio Editor
*/
#include "vstviewmodel.h"

#include "au3-components/EffectInterface.h"
#include "tracktionaudio/internal/tracktioneffectinstance.h"

#include "effects/effects_base/effectstypes.h"

#include "log.h"

using namespace au::effects;

VstViewModel::VstViewModel(QObject* parent, int instanceId)
    : AbstractEffectViewModel(parent, instanceId)
{}

VstViewModel::~VstViewModel()
{
    m_settingUpdateTimer.stop();
    if (m_instance && m_settingsAccess) {
        checkSettingChangesFromUi(true);
    }
}

void VstViewModel::doInit()
{
    EffectInstanceId id = this->instanceId();
    IF_ASSERT_FAILED(id != 0) {
        return;
    }

    std::shared_ptr<EffectInstance> instance = instancesRegister()->instanceById(this->instanceId());
    m_instance = std::dynamic_pointer_cast<tracktionaudio::TracktionHostedEffectInstance>(instance);
    IF_ASSERT_FAILED(m_instance) {
        return;
    }

    m_settingsAccess = instancesRegister()->settingsAccessById(id);
    IF_ASSERT_FAILED(m_settingsAccess) {
        return;
    }

    instancesRegister()->settingsChanged(id).onNotify(this, [this]() {
        settingsToView();
    });

    instancesRegister()->updateSettingsRequested(id).onNotify(this, [this]() {
        settingsFromView();
    });

    realtimeEffectService()->effectSettingsChanged().onNotify(this, [this]() {
        settingsToView();
    });

    settingsToView();

    QObject::connect(&m_settingUpdateTimer, &QTimer::timeout, this, &VstViewModel::checkSettingChangesFromUiWhileIdle);

    m_settingUpdateTimer.start(20);
}

void VstViewModel::checkSettingChangesFromUiWhileIdle()
{
    checkSettingChangesFromUi(false);
}

void VstViewModel::checkSettingChangesFromUi(bool forceCommitting)
{
    if (!m_instance || !m_settingsAccess || !m_settingsLoaded) {
        return;
    }
    const bool hasChanges = m_instance->takeSettingsChanges();
    if ((hasChanges || forceCommitting) && settingsFromView(forceCommitting)) {
        m_settingsAccess->Flush();
        if (hasChanges) {
            projectHistory()->modifyState();
            projectHistory()->markUnsaved();
        }
    }
}

void VstViewModel::settingsToView()
{
    IF_ASSERT_FAILED(m_instance && m_settingsAccess) {
        return;
    }

    m_settingsLoaded = m_instance->loadSettings(m_settingsAccess->Get(), true);
    if (!m_settingsLoaded) {
        LOGE() << "Cannot restore VST3 effect settings.";
    }
}

bool VstViewModel::settingsFromView(bool complete)
{
    if (!m_instance || !m_settingsAccess || !m_settingsLoaded) {
        return false;
    }

    bool saved = false;
    m_settingsAccess->ModifySettings([this, complete, &saved](EffectSettings& settings) {
        saved = complete ? m_instance->saveSettings(settings) : m_instance->savePlaybackSettings(settings);
        return nullptr;
    });
    return saved;
}

void VstViewModel::doStartPreview()
{
    IF_ASSERT_FAILED(m_settingsAccess) {
        return;
    }

    if (!settingsFromView()) {
        return;
    }

    m_settingsAccess->ModifySettings([this](EffectSettings& settings) {
        executionScenario()->previewEffect(instanceId(), settings);
        return nullptr;
    });
}

void VstViewModel::doStopPreview()
{
    executionScenario()->stopPreview();
}
