/*
 * Audacity: A Digital Audio Editor
 */
#include "audiounitviewmodel.h"

#include "au3-effects/Effect.h"
#include "tracktionaudio/internal/tracktioneffectinstance.h"

#include "log.h"

namespace au::effects {
AudioUnitViewModel::AudioUnitViewModel(QObject* parent, int instanceId)
    : AbstractEffectViewModel(parent, instanceId)
{}

AudioUnitViewModel::~AudioUnitViewModel()
{
    deinit();
}

void AudioUnitViewModel::doInit()
{
    IF_ASSERT_FAILED(instanceId() >= 0) {
        return;
    }

    m_instance = std::dynamic_pointer_cast<tracktionaudio::TracktionHostedEffectInstance>(
        instancesRegister()->instanceById(instanceId()));
    IF_ASSERT_FAILED(m_instance && m_instance->audioUnit()) {
        return;
    }

    m_settingsAccess = instancesRegister()->settingsAccessById(instanceId());
    IF_ASSERT_FAILED(m_settingsAccess) {
        return;
    }

    instancesRegister()->settingsChanged(instanceId()).onNotify(this, [this]() {
        settingsToView();
    });
    instancesRegister()->updateSettingsRequested(instanceId()).onNotify(this, [this]() {
        settingsFromView();
    });
    realtimeEffectService()->effectSettingsChanged().onNotify(this, [this]() {
        settingsToView();
    });

    const EffectId id = instancesRegister()->effectIdByInstanceId(instanceId());
    const auto* effect = effectsProvider()->effect(id);
    IF_ASSERT_FAILED(effect) {
        return;
    }

    settingsToView();
    setTitle(effect->GetSymbol().Msgid().translated());
    connect(&m_settingsTimer, &QTimer::timeout, this, [this]() {
        checkSettingChangesFromUi();
    });
    m_settingsTimer.start(100);
}

void AudioUnitViewModel::deinit()
{
    m_settingsTimer.stop();
    checkSettingChangesFromUi(true);
    m_settingsLoaded = false;
    m_settingsAccess.reset();
    m_instance.reset();
}

void AudioUnitViewModel::doStartPreview()
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

void AudioUnitViewModel::doStopPreview()
{
    executionScenario()->stopPreview();
}

void AudioUnitViewModel::settingsToView()
{
    if (m_instance && m_settingsAccess) {
        m_settingsLoaded = m_instance->loadSettings(m_settingsAccess->Get(), true);
        if (!m_settingsLoaded) {
            LOGE() << "Cannot restore Audio Unit effect settings.";
        }
    }
}

bool AudioUnitViewModel::settingsFromView(bool complete)
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

void AudioUnitViewModel::checkSettingChangesFromUi(bool complete)
{
    if (!m_instance || !m_settingsAccess || !m_settingsLoaded) {
        return;
    }

    const bool changed = m_instance->takeSettingsChanges();
    if ((changed || complete) && settingsFromView(complete)) {
        m_settingsAccess->Flush();
        if (changed && globalContext()->currentProject()) {
            projectHistory()->modifyState();
            projectHistory()->markUnsaved();
        }
    }
}

QString AudioUnitViewModel::title() const
{
    return m_title;
}

void AudioUnitViewModel::setTitle(const QString& newTitle)
{
    if (m_title == newTitle) {
        return;
    }
    m_title = newTitle;
    emit titleChanged();
}
}
