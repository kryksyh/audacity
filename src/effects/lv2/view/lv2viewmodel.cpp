/*
 * Audacity: A Digital Audio Editor
 */
#include <tracktion_engine/tracktion_engine.h>

#include "lv2viewmodel.h"

#include <QPointer>

#include "au3-effects/Effect.h"
#include "au3-realtime-effects/RealtimeEffectState.h"
#include "tracktionaudio/internal/tracktioneffectinstance.h"
#include "tracktionaudio/internal/tracktionlv2ui.h"
#include "playback/iaudiooutput.h"

#if JUCE_LINUX || JUCE_BSD
#include <X11/Xlib.h>
#endif

using namespace au::effects;

Lv2ViewModel::Lv2ViewModel(QObject* parent, int instanceId, const QString& effectState)
    : AbstractEffectViewModel(parent, instanceId),
    m_effectState(!effectState.isEmpty() ? reinterpret_cast<RealtimeEffectState*>(effectState.toULongLong())->shared_from_this() : nullptr)
{}

Lv2ViewModel::~Lv2ViewModel() { deinit(); }

void Lv2ViewModel::doInit()
{
    m_instance = std::dynamic_pointer_cast<tracktionaudio::TracktionHostedEffectInstance>(
        instancesRegister()->instanceById(instanceId()));
    m_settingsAccess = instancesRegister()->settingsAccessById(instanceId());
    if (!m_instance || !m_settingsAccess || !m_instance->Init()) {
        emit vendorUiFailed();
        return;
    }
    auto* processor = m_instance->tracktionPlugin().getAudioPluginInstance();
    if (!processor || !processor->getLV2Client()) {
        emit vendorUiFailed();
        return;
    }
    instancesRegister()->settingsChanged(instanceId()).onNotify(this, [this]() { settingsToView(); });
    instancesRegister()->updateSettingsRequested(instanceId()).onNotify(this, [this]() { checkSettingChangesFromUi(true); });
    realtimeEffectService()->effectSettingsChanged().onNotify(this, [this]() { settingsToView(); });
    settingsToView();
    if (!m_settingsLoaded) {
        return;
    }
    if (!m_instance->prepareLv2Editor(playback()->audioOutput()->sampleRate())) {
        emit vendorUiFailed();
        return;
    }
    const auto* effect = effectsProvider()->effect(instancesRegister()->effectIdByInstanceId(instanceId()));
    if (effect) {
        setTitle(effect->GetSymbol().Msgid().translated());
    }
    const QPointer<Lv2ViewModel> view(this);
    m_pluginUi = std::make_shared<tracktionaudio::TracktionLv2Ui>(*processor, tracktionaudio::TracktionLv2Ui::Callbacks {
        {}, [view]() {
            if (view) {
                QTimer::singleShot(0, view, [view]() {
                    if (view) {
                        emit view->vendorUiFailed();
                    }
                });
            }
        }, [](void* window) {
#if JUCE_LINUX || JUCE_BSD
            auto* display = XOpenDisplay(nullptr);
            if (display) {
                XMapRaised(display, reinterpret_cast<Window>(window));
                XFlush(display);
                XCloseDisplay(display);
            }
#else
            (void)window;
#endif
        }
    });
    if (!m_pluginUi->open()) {
        m_pluginUi.reset();
        emit vendorUiFailed();
        return;
    }
    connect(&m_uiTimer, &QTimer::timeout, this, &Lv2ViewModel::onIdle);
    m_uiTimer.start(30);
    if (m_effectState) {
        connect(&m_settingsTimer, &QTimer::timeout, this, &Lv2ViewModel::makeDirtyIfSettingsChanged);
        m_settingsTimer.start(1000);
    }
}

void Lv2ViewModel::deinit()
{
    m_uiTimer.stop();
    m_settingsTimer.stop();
    checkSettingChangesFromUi(true);
    makeDirtyIfSettingsChanged();
    if (m_pluginUi) {
        m_pluginUi->close();
        m_pluginUi.reset();
    }
    m_settingsLoaded = false;
    m_settingsAccess.reset();
    m_instance.reset();
}

void Lv2ViewModel::settingsToView()
{
    if (m_instance && m_settingsAccess) {
        m_settingsLoaded = m_instance->loadSettings(m_settingsAccess->Get(), true);
        if (!m_settingsLoaded) {
            emit vendorUiFailed();
        }
    }
}

bool Lv2ViewModel::settingsFromView(bool complete)
{
    if (!m_instance || !m_settingsAccess || !m_settingsLoaded) {
        return false;
    }
    bool saved = false;
    m_settingsAccess->ModifySettings([this, complete, &saved](EffectSettings& settings) {
        saved = complete ? m_instance->saveSettings(settings) : m_instance->savePlaybackSettings(settings);
        return nullptr;
    });
    if (saved) {
        m_settingsAccess->Flush();
    }
    return saved;
}

void Lv2ViewModel::checkSettingChangesFromUi(bool complete)
{
    if (!m_instance || !m_settingsLoaded) {
        return;
    }
    const bool changed = m_instance->takeSettingsChanges();
    if ((changed || complete) && settingsFromView(complete)) {
        m_settingsChanged = m_settingsChanged || changed;
        if (changed && !m_effectState && isPreviewing()) {
            stopPreview();
        }
    }
}

void Lv2ViewModel::makeDirtyIfSettingsChanged()
{
    if (m_settingsChanged && m_effectState && globalContext()->currentProject()) {
        m_settingsChanged = false;
        projectHistory()->modifyState();
        projectHistory()->markUnsaved();
    }
}

void Lv2ViewModel::onIdle()
{
    const auto instance = m_instance;
    const auto ui = m_pluginUi;
    const QPointer<Lv2ViewModel> view(this);
    if (instance && m_settingsLoaded) {
        instance->processEditorIdle();
    }
    if (!view) {
        return;
    }
    if (ui && !ui->idle()) {
        if (view) {
            emit externalUiClosed();
        }
        return;
    }
    if (view) {
        checkSettingChangesFromUi();
    }
}

void Lv2ViewModel::doStartPreview()
{
    if (!settingsFromView(true)) {
        return;
    }
    m_settingsAccess->ModifySettings([this](EffectSettings& settings) {
        executionScenario()->previewEffect(instanceId(), settings);
        return nullptr;
    });
}

void Lv2ViewModel::doStopPreview() { executionScenario()->stopPreview(); }
QString Lv2ViewModel::unsupportedUiReason() const { return {}; }
QString Lv2ViewModel::title() const { return m_title; }
void Lv2ViewModel::setTitle(const QString& title)
{
    if (m_title != title) {
        m_title = title;
        emit titleChanged();
    }
}
