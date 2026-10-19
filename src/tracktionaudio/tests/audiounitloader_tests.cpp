/*
 * Audacity: A Digital Audio Editor
 */
#include <tracktion_engine/tracktion_engine.h>

#include <gtest/gtest.h>
#include <QTemporaryDir>

#if AU_TRACKTION_TEST_AUDIO_UNIT
#include <AudioToolbox/AudioUnitParameters.h>
#include "effects/audio_unit/internal/audiouniteffectloader.h"
#include "effects/audio_unit/qml/Audacity/AudioUnit/audiounitviewmodel.h"
#include "effects/effects_base/internal/effectinstancesregister.h"
#include "effects/effects_base/internal/effectsprovider.h"
#include "effects/effects_base/internal/realtimeeffectservice.h"
#include "effects/effects_base/internal/effectsutils.h"
#include "../internal/tracktionpluginscanner.h"
#include "../internal/tracktioneffectinstance.h"
#include "../internal/tracktionruntime.h"
#include "audioplugins/tests/mocks/knownaudiopluginsregistermock.h"
#include "audioplugins/tests/mocks/audiopluginsloadguardmock.h"
#include "global/tests/mocks/globalconfigurationmock.h"
#include "au3-audio-unit/AudioUnitEffectBase.h"
#include "au3-module-manager/ConfigInterface.h"
#include "global/defer.h"
#include "global/modularity/ioc.h"
#include "playback/tests/mocks/playbackmock.h"
#include "playback/tests/mocks/playermock.h"
#include "context/tests/mocks/globalcontextmock.h"

namespace au::tracktionaudio {
namespace {
class AudioUnitLoaderTests : public ::testing::Test
{
protected:
    using Known = muse::audioplugins::IKnownAudioPluginsRegister;
    using Guard = muse::audioplugins::IAudioPluginsLoadGuard;

    void SetUp() override
    {
        ASSERT_TRUE(m_directory.isValid());
        auto* ioc = muse::modularity::globalIoc();
        m_previousKnown = ioc->resolve<Known>("utests");
        m_previousGuard = ioc->resolve<Guard>("utests");
        if (m_previousKnown) {
            ioc->unregister<Known>("utests");
        }
        if (m_previousGuard) {
            ioc->unregister<Guard>("utests");
        }
        ioc->registerExport<Known>("utests", m_known);
        ioc->registerExport<Guard>("utests", m_guard);
        ON_CALL(*m_known, pluginInfoList(testing::_)).WillByDefault(testing::Invoke([this](auto) { return m_plugins; }));
        ON_CALL(*m_guard, beginLoad(testing::_)).WillByDefault(testing::Return(muse::make_ok()));
        m_runtime->init(m_directory.filePath("cache").toStdString(), m_directory.filePath("settings").toStdString());
        m_loader = std::make_unique<effects::AudioUnitEffectLoader>(m_runtime);
        ASSERT_NO_FATAL_FAILURE(select("appl/aufx/lpas/Apple: AULowpass"));
    }

    void TearDown() override
    {
        m_loader.reset();
        m_runtime.reset();
        auto* ioc = muse::modularity::globalIoc();
        ioc->unregister<Known>("utests");
        ioc->unregister<Guard>("utests");
        if (m_previousKnown) {
            ioc->registerExport<Known>("utests", m_previousKnown);
        }
        if (m_previousGuard) {
            ioc->registerExport<Guard>("utests", m_previousGuard);
        }
    }

    void select(const char* path)
    {
        const auto metadata = TracktionPluginMetaReader(PluginFormat::AudioUnit).readMeta(path);
        ASSERT_TRUE(metadata.ret) << metadata.ret.toString();
        ASSERT_EQ(metadata.val.size(), 1u);
        m_plugins = { { metadata.val.front(), muse::io::path_t(path), muse::audioplugins::AudioPluginState::Validated } };
        m_id = muse::String::fromStdString(m_plugins.front().meta.id);
    }

    std::shared_ptr<TracktionHostedEffectInstance> instance(const EffectInstanceFactory& effect)
    {
        return std::dynamic_pointer_cast<TracktionHostedEffectInstance>(effect.MakeInstance());
    }

    static float parameter(AudioUnit unit, AudioUnitParameterID id)
    {
        AudioUnitParameterValue value = 0;
        EXPECT_EQ(AudioUnitGetParameter(unit, id, kAudioUnitScope_Global, 0, &value), noErr);
        return value;
    }

    QTemporaryDir m_directory;
    std::shared_ptr<Known> m_previousKnown;
    std::shared_ptr<Guard> m_previousGuard;
    std::shared_ptr<testing::NiceMock<muse::audioplugins::KnownAudioPluginsRegisterMock> > m_known
        = std::make_shared<testing::NiceMock<muse::audioplugins::KnownAudioPluginsRegisterMock> >();
    std::shared_ptr<testing::NiceMock<muse::audioplugins::AudioPluginsLoadGuardMock> > m_guard
        = std::make_shared<testing::NiceMock<muse::audioplugins::AudioPluginsLoadGuardMock> >();
    muse::audioplugins::AudioPluginInfoList m_plugins;
    effects::EffectId m_id;
    std::shared_ptr<TracktionRuntime> m_runtime = std::make_shared<TracktionRuntime>();
    std::unique_ptr<effects::AudioUnitEffectLoader> m_loader;
};

class EditorEffectsProvider : public effects::EffectsProvider
{
public:
    Effect* effect(const effects::EffectId&) const override { return current; }
    Effect* current = nullptr;
};

class EditorPreviewScenario : public effects::IEffectExecutionScenario
{
public:
    MOCK_METHOD(muse::Ret, performEffect, (const effects::EffectId&), (override));
    MOCK_METHOD(muse::Ret, performEffect, (const effects::EffectId&, const std::string&), (override));
    MOCK_METHOD(bool, lastProcessorIsAvailable, (), (const, override));
    MOCK_METHOD(muse::async::Notification, lastProcessorIsNowAvailable, (), (const, override));
    MOCK_METHOD(muse::async::Channel<effects::EffectId>, lastProcessorIdChanged, (), (const, override));
    MOCK_METHOD(muse::Ret, repeatLastProcessor, (), (override));
    MOCK_METHOD(void, stopPreview, (), (override));

    muse::Ret previewEffect(const effects::EffectInstanceId&, EffectSettings& settings) override
    {
        ++calls;
        cutoff = AudioUnitWrapper::GetSettings(settings).values.at(kLowPassParam_CutoffFrequency)->second;
        return muse::make_ok();
    }

    int calls = 0;
    float cutoff = 0.0f;
};

class AudioUnitEditorTests : public AudioUnitLoaderTests
{
protected:
    void SetUp() override
    {
        ASSERT_NO_FATAL_FAILURE(AudioUnitLoaderTests::SetUp());
        ASSERT_TRUE(m_loader->ensurePluginIsLoaded(m_id));
        m_provider->current = m_loader->effect(m_id);
        ASSERT_TRUE(m_provider->current);
        auto* global = muse::modularity::globalIoc();
        m_previousProvider = global->resolve<effects::IEffectsProvider>("utests");
        m_previousInstances = global->resolve<effects::IEffectInstancesRegister>("utests");
        if (m_previousProvider) {
            global->unregister<effects::IEffectsProvider>("utests");
        }
        if (m_previousInstances) {
            global->unregister<effects::IEffectInstancesRegister>("utests");
        }
        global->registerExport<effects::IEffectsProvider>("utests", m_provider);
        global->registerExport<effects::IEffectInstancesRegister>("utests", m_instances);
        auto* contextIoc = muse::modularity::ioc(m_context);
        const auto playbackService = std::make_shared<testing::NiceMock<playback::PlaybackMock> >();
        const auto player = std::make_shared<testing::NiceMock<playback::PlayerMock> >();
        ON_CALL(*playbackService, player(testing::_)).WillByDefault(testing::Return(player));
        contextIoc->registerExport<playback::IPlayback>("utests", playbackService);
        contextIoc->registerExport<effects::IRealtimeEffectService>("utests", std::make_shared<effects::RealtimeEffectService>(m_context));
        contextIoc->registerExport<effects::IEffectExecutionScenario>("utests", m_preview);
        contextIoc->registerExport<context::IGlobalContext>("utests", std::make_shared<testing::NiceMock<context::GlobalContextMock> >());
    }

    void TearDown() override
    {
        muse::modularity::removeIoC(m_context);
        auto* global = muse::modularity::globalIoc();
        global->unregister<effects::IEffectInstancesRegister>("utests");
        global->unregister<effects::IEffectsProvider>("utests");
        m_instances.reset();
        if (m_previousInstances) {
            global->registerExport<effects::IEffectInstancesRegister>("utests", m_previousInstances);
        }
        if (m_previousProvider) {
            global->registerExport<effects::IEffectsProvider>("utests", m_previousProvider);
        }
        AudioUnitLoaderTests::TearDown();
    }

    muse::modularity::ContextPtr m_context = std::make_shared<muse::modularity::Context>(417);
    std::shared_ptr<effects::IEffectsProvider> m_previousProvider;
    std::shared_ptr<effects::IEffectInstancesRegister> m_previousInstances;
    std::shared_ptr<EditorEffectsProvider> m_provider = std::make_shared<EditorEffectsProvider>();
    std::shared_ptr<effects::EffectInstancesRegister> m_instances = std::make_shared<effects::EffectInstancesRegister>();
    std::shared_ptr<EditorPreviewScenario> m_preview = std::make_shared<EditorPreviewScenario>();
};
}

TEST_F(AudioUnitEditorTests, RejectedStateIsPreservedDuringUpdatesPreviewAndClose)
{
    const auto* effect = m_provider->current;
    for (const bool requestUpdate : { false, true }) {
        SCOPED_TRACE(requestUpdate);
        auto settings = effect->MakeSettings();
        CommandParameters parameters;
        parameters.Write("AudioUnitState", "YmFk");
        ASSERT_TRUE(effect->LoadSettings(parameters, settings));
        const auto rejected = AudioUnitWrapper::GetSettings(settings).mState;
        ASSERT_TRUE(rejected);
        const auto editor = instance(*effect);
        ASSERT_TRUE(editor);
        ASSERT_FALSE(editor->loadSettings(settings, true));
        const auto access = std::make_shared<SimpleEffectSettingsAccess>(settings);
        const auto id = m_instances->regInstance(m_id, editor, access);
        {
            effects::AudioUnitViewModel model(nullptr, id);
            model.setContext(m_context);
            model.init();
            if (requestUpdate) {
                m_instances->requestUpdateSettings(id);
                EXPECT_EQ(AudioUnitWrapper::GetSettings(settings).mState, rejected);
                model.startPreview();
                EXPECT_EQ(m_preview->calls, 0);
            }
            model.deinit();
        }
        EXPECT_EQ(AudioUnitWrapper::GetSettings(settings).mState, rejected);
        m_instances->unregInstance(id);
    }
}

TEST_F(AudioUnitEditorTests, AValidStateRestoresEditorUpdatesAndPreviewAfterRejection)
{
    const auto* effect = m_provider->current;
    auto settings = effect->MakeSettings();
    CommandParameters parameters;
    parameters.Write("AudioUnitState", "YmFk");
    ASSERT_TRUE(effect->LoadSettings(parameters, settings));
    const auto editor = instance(*effect);
    ASSERT_TRUE(editor);
    const auto access = std::make_shared<SimpleEffectSettingsAccess>(settings);
    const auto id = m_instances->regInstance(m_id, editor, access);
    effects::AudioUnitViewModel model(nullptr, id);
    model.setContext(m_context);
    model.init();
    auto valid = effect->MakeSettings();
    const auto source = instance(*effect);
    ASSERT_TRUE(source);
    ASSERT_TRUE(source->loadSettings(valid, true));
    ASSERT_TRUE(source->setParameterValue(u"0", 1234.0));
    ASSERT_TRUE(source->saveSettings(valid));
    access->Set(std::move(valid), nullptr);
    m_instances->notifyAboutSettingsChanged(id);
    EXPECT_NEAR(parameter(editor->audioUnit(), kLowPassParam_CutoffFrequency), 1234.0f, 0.01f);
    ASSERT_TRUE(editor->setParameterValue(u"0", 900.0));
    m_instances->requestUpdateSettings(id);
    EXPECT_NEAR(AudioUnitWrapper::GetSettings(settings).values.at(kLowPassParam_CutoffFrequency)->second, 900.0f, 0.01f);
    model.startPreview();
    EXPECT_EQ(m_preview->calls, 1);
    EXPECT_NEAR(m_preview->cutoff, 900.0f, 0.01f);
    model.deinit();
    EXPECT_TRUE(AudioUnitWrapper::GetSettings(settings).mState);
    m_instances->unregInstance(id);
}

TEST_F(AudioUnitLoaderTests, NativeDefinitionKeepsIdentityAndCreatesTracktionInstances)
{
    EXPECT_EQ(m_loader->family(), effects::EffectFamily::AudioUnit);
    EXPECT_CALL(*m_guard, beginLoad(m_id.toStdString())).Times(1);
    EXPECT_CALL(*m_guard, endLoad(m_id.toStdString())).Times(1);
    ASSERT_TRUE(m_loader->ensurePluginIsLoaded(m_id));
    const auto* effect = m_loader->effect(m_id);
    ASSERT_TRUE(effect);
    EXPECT_FALSE(dynamic_cast<const AudioUnitEffectBase*>(effect));
    EXPECT_EQ(effects::utils::effectId(effect), m_id);
    EXPECT_TRUE(effect->IsInteractive());
    EXPECT_TRUE(effect->SupportsAutomation());
    EXPECT_TRUE(effect->SupportsRealtime());
    ASSERT_TRUE(m_loader->ensurePluginIsLoaded(m_id));
    testing::Mock::VerifyAndClearExpectations(m_guard.get());
    const auto editor = instance(*effect);
    ASSERT_TRUE(editor);
    ASSERT_TRUE(editor->Init());
    ASSERT_TRUE(editor->audioUnit());
    EXPECT_FALSE(editor->vstComponent());
    auto settings = effect->MakeSettings();
    EXPECT_TRUE(AudioUnitWrapper::GetSettings(settings).mState);
    ASSERT_TRUE(editor->loadSettings(settings, true));
    EXPECT_FALSE(editor->tracktionPlugin().edit.engine.getDeviceManager().deviceManager.getCurrentAudioDevice());
    m_loader->deinit();
    m_runtime.reset();
    const auto next = instance(*effect);
    ASSERT_TRUE(next);
    EXPECT_TRUE(next->Init());
}

TEST_F(AudioUnitLoaderTests, LegacyParametersAndCurrentStateUseTheSameCodec)
{
    ASSERT_TRUE(m_loader->ensurePluginIsLoaded(m_id));
    const auto* effect = m_loader->effect(m_id);
    ASSERT_TRUE(effect);
    AudioComponentDescription description { kAudioUnitType_Effect, kAudioUnitSubType_LowPassFilter, kAudioUnitManufacturer_Apple, 0, 0 };
    AudioUnitEffectBase legacy("appl/aufx/lpas/Apple: AULowpass", "Apple: AULowpass", AudioComponentFindNext(nullptr, &description));
    ASSERT_TRUE(legacy.InitializePlugin());
    EXPECT_EQ(effect->GetSymbol(), legacy.GetSymbol());
    EXPECT_EQ(effect->GetVendor(), legacy.GetVendor());
    EXPECT_EQ(effect->GetVersion(), legacy.GetVersion());
    EXPECT_EQ(effect->GetDescription(), legacy.GetDescription());
    EXPECT_EQ(effect->GetFamily(), legacy.GetFamily());
    EXPECT_EQ(effect->GetType(), legacy.GetType());
    EXPECT_EQ(effect->SupportsAutomation(), legacy.SupportsAutomation());
    auto oldSettings = legacy.MakeSettings();
    AudioUnitWrapper::GetSettings(oldSettings).values.at(kLowPassParam_CutoffFrequency)->second = 1234.0f;
    CommandParameters parameters;
    ASSERT_TRUE(legacy.SaveSettings(oldSettings, parameters));
    auto settings = effect->MakeSettings();
    settings.extra.SetActive(false);
    ASSERT_TRUE(effect->LoadSettings(parameters, settings));
    EXPECT_FALSE(settings.extra.GetActive());
    const auto editor = instance(*effect);
    ASSERT_TRUE(editor);
    ASSERT_TRUE(editor->loadSettings(settings, true));
    EXPECT_NEAR(parameter(editor->audioUnit(), kLowPassParam_CutoffFrequency), 1234.0f, 0.01f);
    ASSERT_TRUE(editor->saveSettings(settings));
    CommandParameters saved;
    ASSERT_TRUE(effect->SaveSettings(settings, saved));
    EXPECT_TRUE(saved.HasEntry("AudioUnitState"));
    ASSERT_TRUE(legacy.LoadSettings(saved, oldSettings));
    EXPECT_NEAR(AudioUnitWrapper::GetSettings(oldSettings).values.at(kLowPassParam_CutoffFrequency)->second, 1234.0f, 0.01f);
}

TEST_F(AudioUnitLoaderTests, FactoryPresetsKeepNamesNumbersAndParameterValues)
{
    ASSERT_NO_FATAL_FAILURE(select("appl/aufx/mrev/Apple: AUMatrixReverb"));
    ASSERT_TRUE(m_loader->ensurePluginIsLoaded(m_id));
    const auto* effect = m_loader->effect(m_id);
    ASSERT_TRUE(effect);
    AudioComponentDescription description { kAudioUnitType_Effect, kAudioUnitSubType_MatrixReverb, kAudioUnitManufacturer_Apple, 0, 0 };
    AudioUnitEffectBase legacy("appl/aufx/mrev/Apple: AUMatrixReverb", "Apple: AUMatrixReverb", AudioComponentFindNext(nullptr,
                                                                                                                       &description));
    ASSERT_TRUE(legacy.InitializePlugin());
    const auto names = effect->GetFactoryPresets();
    EXPECT_EQ(names, legacy.GetFactoryPresets());
    ASSERT_GE(names.size(), 2u);
    for (int index = 0; index < 2; ++index) {
        auto settings = effect->MakeSettings();
        settings.extra.SetActive(false);
        ASSERT_TRUE(effect->LoadFactoryPreset(index, settings));
        EXPECT_FALSE(settings.extra.GetActive());
        auto reference = legacy.MakeSettings();
        ASSERT_TRUE(legacy.LoadFactoryPreset(index, reference));
        const auto& actual = AudioUnitWrapper::GetSettings(settings);
        const auto& expected = AudioUnitWrapper::GetSettings(reference);
        EXPECT_EQ(actual.mPresetNumber, expected.mPresetNumber);
        for (const auto& [id, value] : expected.values) {
            if (value) {
                ASSERT_TRUE(actual.values.at(id));
                EXPECT_NEAR(actual.values.at(id)->second, value->second, 1e-5f);
            }
        }
        CommandParameters old;
        ASSERT_TRUE(legacy.SaveSettings(reference, old));
        auto restored = effect->MakeSettings();
        ASSERT_TRUE(effect->LoadSettings(old, restored));
        EXPECT_EQ(AudioUnitWrapper::GetSettings(restored).mPresetNumber, expected.mPresetNumber);
    }
    auto settings = effect->MakeSettings();
    auto defaults = legacy.MakeSettings();
    ASSERT_TRUE(legacy.LoadFactoryDefaults(defaults));
    ASSERT_TRUE(effect->LoadFactoryDefaults(settings));
    const auto& expectedDefaults = AudioUnitWrapper::GetSettings(defaults);
    for (const auto& [id, value] : expectedDefaults.values) {
        if (value) {
            ASSERT_TRUE(AudioUnitWrapper::GetSettings(settings).values.at(id));
            EXPECT_NEAR(AudioUnitWrapper::GetSettings(settings).values.at(id)->second, value->second, 1e-5f);
        }
    }
    EXPECT_FALSE(effect->LoadFactoryPreset(-1, settings));
    EXPECT_FALSE(effect->LoadFactoryPreset(static_cast<int>(names.size()), settings));
}

TEST_F(AudioUnitLoaderTests, UserPresetsKeepBinaryCompatibilityAndDefaults)
{
    ASSERT_TRUE(m_loader->ensurePluginIsLoaded(m_id));
    const auto* effect = m_loader->effect(m_id);
    ASSERT_TRUE(effect);
    const auto editor = instance(*effect);
    ASSERT_TRUE(editor);
    auto settings = effect->MakeSettings();
    ASSERT_TRUE(editor->loadSettings(settings, true));
    ASSERT_TRUE(editor->setParameterValue(u"0", 1234.0));
    ASSERT_TRUE(editor->saveSettings(settings));
    const auto group = UserPresetsGroup(wxT("native-au-loader"));
    ASSERT_TRUE(effect->SaveUserPreset(group, settings));
    auto loaded = effect->MakeSettings();
    loaded.extra.SetActive(false);
    ASSERT_TRUE(effect->LoadUserPreset(group, loaded));
    EXPECT_FALSE(loaded.extra.GetActive());
    ASSERT_TRUE(editor->loadSettings(loaded, true));
    EXPECT_NEAR(parameter(editor->audioUnit(), kLowPassParam_CutoffFrequency), 1234.0f, 0.01f);
    ASSERT_TRUE(RemoveConfig(*effect, PluginSettings::Private, group, wxT("Parameters")));
    ASSERT_TRUE(effect->LoadUserPreset(group, loaded));
    ASSERT_TRUE(editor->loadSettings(loaded, true));
    EXPECT_NEAR(parameter(editor->audioUnit(), kLowPassParam_CutoffFrequency), 1234.0f, 0.01f);
    ASSERT_TRUE(effect->LoadFactoryDefaults(loaded));
    EXPECT_FALSE(loaded.extra.GetActive());
    ASSERT_TRUE(editor->loadSettings(loaded, true));
    const auto initial = effect->MakeSettings();
    const auto& defaults = AudioUnitWrapper::GetSettings(initial);
    EXPECT_NEAR(parameter(editor->audioUnit(), kLowPassParam_CutoffFrequency), defaults.values.at(
                    kLowPassParam_CutoffFrequency)->second, 0.01f);
}

TEST_F(AudioUnitLoaderTests, LoaderCreatesItsHostWithoutOpeningAnAudioDevice)
{
    using Configuration = muse::IGlobalConfiguration;
    auto* ioc = muse::modularity::globalIoc();
    const auto previous = ioc->resolve<Configuration>("utests");
    if (previous) {
        ioc->unregister<Configuration>("utests");
    }
    const auto configuration = std::make_shared<testing::NiceMock<muse::GlobalConfigurationMock> >();
    ioc->registerExport<Configuration>("utests", configuration);
    DEFER {
        ioc->unregister<Configuration>("utests");
        if (previous) {
            ioc->registerExport<Configuration>("utests", previous);
        }
    };
    ON_CALL(*configuration, userAppDataPath()).WillByDefault(testing::Return(muse::io::path_t(m_directory.filePath("data").toStdString())));
    ON_CALL(*configuration, appConfigPath()).WillByDefault(testing::Return(muse::io::path_t(m_directory.filePath("config").toStdString())));
    m_loader = std::make_unique<effects::AudioUnitEffectLoader>();
    ASSERT_TRUE(m_loader->ensurePluginIsLoaded(m_id));
    const auto* effect = m_loader->effect(m_id);
    ASSERT_TRUE(effect);
    const auto editor = instance(*effect);
    ASSERT_TRUE(editor);
    EXPECT_FALSE(editor->tracktionPlugin().edit.engine.getDeviceManager().deviceManager.getCurrentAudioDevice());
}

TEST_F(AudioUnitLoaderTests, CurrentComponentMetadataKeepsTheCachedIdentifier)
{
    const auto version = m_plugins.front().meta.attributeVal(effects::EFFECT_VERSION_ATTRIBUTE);
    m_plugins.front().meta.attributes[effects::EFFECT_VERSION_ATTRIBUTE] = u"0.0.0";
    m_plugins.front().meta.attributes[effects::EFFECT_TYPE_ATTRIBUTE] = effects::utils::effectTypeToString(effects::EffectType::Generator);
    ASSERT_TRUE(m_loader->ensurePluginIsLoaded(m_id));
    const auto* effect = m_loader->effect(m_id);
    ASSERT_TRUE(effect);
    EXPECT_EQ(effects::utils::effectId(effect), m_id);
    EXPECT_EQ(effect->GetVersion(), wxString::FromUTF8(version.toStdString()));
    EXPECT_EQ(effect->GetType(), EffectTypeProcess);
}

TEST_F(AudioUnitLoaderTests, InvalidEntriesAndRejectedGuardsDoNotLoadComponents)
{
    EXPECT_CALL(*m_guard, beginLoad(testing::_)).Times(0);
    m_plugins.front().state = muse::audioplugins::AudioPluginState::Discovered;
    EXPECT_FALSE(m_loader->ensurePluginIsLoaded(m_id));
    m_plugins.front().state = muse::audioplugins::AudioPluginState::Validated;
    m_plugins.front().meta.type = "VST3";
    EXPECT_FALSE(m_loader->ensurePluginIsLoaded(m_id));
    m_plugins.front().meta.type = "AudioUnit";
    m_plugins.front().path = "invalid";
    EXPECT_FALSE(m_loader->ensurePluginIsLoaded(m_id));
    testing::Mock::VerifyAndClearExpectations(m_guard.get());
    m_plugins.front().path = "appl/aufx/lpas/Apple: AULowpass";
    EXPECT_CALL(*m_guard, beginLoad(m_id.toStdString())).WillOnce(testing::Return(muse::make_ret(muse::Ret::Code::InternalError)));
    EXPECT_CALL(*m_guard, endLoad(testing::_)).Times(0);
    EXPECT_FALSE(m_loader->ensurePluginIsLoaded(m_id));
}
}
#endif
