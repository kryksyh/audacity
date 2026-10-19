/*
* Audacity: A Digital Audio Editor
*/
#include <tracktion_engine/tracktion_engine.h>

#include <gtest/gtest.h>
#include <QTemporaryDir>

#if AU_TRACKTION_TEST_VST3
#include "effects/vst/internal/vst3effectloader.h"
#include "effects/vst/qml/Audacity/Vst/vstviewmodel.h"
#include "effects/effects_base/internal/effectinstancesregister.h"
#include "effects/effects_base/internal/realtimeeffectservice.h"
#include "effects/effects_base/internal/effectsutils.h"
#include "../internal/tracktionpluginscanner.h"
#include "../internal/tracktioneffectinstance.h"
#include "../internal/tracktionruntime.h"
#include "audioplugins/tests/mocks/knownaudiopluginsregistermock.h"
#include "audioplugins/tests/mocks/audiopluginsloadguardmock.h"
#include "au3-vst3/VST3EffectBase.h"
#include "au3-vst3/VST3EffectsModule.h"
#include "global/defer.h"
#include "global/modularity/ioc.h"
#include "global/tests/mocks/globalconfigurationmock.h"
#include "playback/tests/mocks/playbackmock.h"
#include "playback/tests/mocks/playermock.h"
#include "trackedit/tests/mocks/projecthistorymock.h"

namespace au::tracktionaudio {
namespace {
class VstLoaderTests : public ::testing::Test
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
        const auto metadata = TracktionPluginMetaReader(PluginFormat::VST3).readMeta(AU_TRACKTION_TEST_VST3_PATH);
        ASSERT_TRUE(metadata.ret) << metadata.ret.toString();
        ASSERT_EQ(metadata.val.size(), 1u);
        m_plugins.push_back({ metadata.val.front(), muse::io::path_t(AU_TRACKTION_TEST_VST3_PATH),
                              muse::audioplugins::AudioPluginState::Validated });
        m_id = muse::String::fromStdString(m_plugins.front().meta.id);
        m_runtime->init(m_directory.filePath("cache").toStdString(), m_directory.filePath("settings").toStdString());
        m_loader = std::make_unique<effects::Vst3EffectLoader>(m_runtime);
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

    static void checkAudio(EffectInstanceEx& instance, EffectSettings& settings, float gain)
    {
        ASSERT_EQ(instance.SetBlockSize(128), 128u);
        ASSERT_TRUE(instance.ProcessInitialize(settings, 48000.0, nullptr));
        DEFER { instance.ProcessFinalize();
        };
        std::vector<float> input(128, 0.4f), left(128), right(128);
        const float* inputs[] { input.data(), input.data() };
        float* outputs[] { left.data(), right.data() };
        ASSERT_EQ(instance.ProcessBlock(settings, inputs, outputs, 128), 128u);
        for (int frame = 33; frame < 128; ++frame) {
            EXPECT_NEAR(left[frame], 0.4f * gain, 1e-6f);
            EXPECT_NEAR(right[frame], 0.4f * gain, 1e-6f);
        }
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
    std::unique_ptr<effects::Vst3EffectLoader> m_loader;
};

class VstPreviewScenario : public effects::IEffectExecutionScenario
{
public:
    MOCK_METHOD(muse::Ret, performEffect, (const effects::EffectId&), (override));
    MOCK_METHOD(muse::Ret, performEffect, (const effects::EffectId&, const std::string&), (override));
    MOCK_METHOD(bool, lastProcessorIsAvailable, (), (const, override));
    MOCK_METHOD(muse::async::Notification, lastProcessorIsNowAvailable, (), (const, override));
    MOCK_METHOD(muse::async::Channel<effects::EffectId>, lastProcessorIdChanged, (), (const, override));
    MOCK_METHOD(muse::Ret, repeatLastProcessor, (), (override));
    MOCK_METHOD(void, stopPreview, (), (override));

    muse::Ret previewEffect(const effects::EffectInstanceId&, EffectSettings&) override
    {
        ++calls;
        return muse::make_ok();
    }

    int calls = 0;
};

class VstEditorTests : public VstLoaderTests
{
protected:
    void SetUp() override
    {
        ASSERT_NO_FATAL_FAILURE(VstLoaderTests::SetUp());
        ASSERT_TRUE(m_loader->ensurePluginIsLoaded(m_id));
        auto* global = muse::modularity::globalIoc();
        m_previousInstances = global->resolve<effects::IEffectInstancesRegister>("utests");
        if (m_previousInstances) {
            global->unregister<effects::IEffectInstancesRegister>("utests");
        }
        global->registerExport<effects::IEffectInstancesRegister>("utests", m_instances);
        auto* context = muse::modularity::ioc(m_context);
        const auto playbackService = std::make_shared<testing::NiceMock<playback::PlaybackMock> >();
        const auto player = std::make_shared<testing::NiceMock<playback::PlayerMock> >();
        ON_CALL(*playbackService, player(testing::_)).WillByDefault(testing::Return(player));
        context->registerExport<playback::IPlayback>("utests", playbackService);
        context->registerExport<effects::IRealtimeEffectService>("utests", std::make_shared<effects::RealtimeEffectService>(m_context));
        context->registerExport<effects::IEffectExecutionScenario>("utests", m_preview);
        context->registerExport<trackedit::IProjectHistory>("utests",
                                                            std::make_shared<testing::NiceMock<trackedit::ProjectHistoryMock> >());
    }

    void TearDown() override
    {
        muse::modularity::removeIoC(m_context);
        auto* global = muse::modularity::globalIoc();
        global->unregister<effects::IEffectInstancesRegister>("utests");
        m_instances.reset();
        if (m_previousInstances) {
            global->registerExport<effects::IEffectInstancesRegister>("utests", m_previousInstances);
        }
        VstLoaderTests::TearDown();
    }

    muse::modularity::ContextPtr m_context = std::make_shared<muse::modularity::Context>(418);
    std::shared_ptr<effects::IEffectInstancesRegister> m_previousInstances;
    std::shared_ptr<effects::EffectInstancesRegister> m_instances = std::make_shared<effects::EffectInstancesRegister>();
    std::shared_ptr<VstPreviewScenario> m_preview = std::make_shared<VstPreviewScenario>();
};
}

TEST_F(VstEditorTests, RejectedStateIsPreservedDuringUpdatesPreviewAndClose)
{
    const auto* effect = m_loader->effect(m_id);
    ASSERT_TRUE(effect);
    for (const bool requestUpdate : { false, true }) {
        SCOPED_TRACE(requestUpdate);
        auto settings = effect->MakeSettings();
        CommandParameters parameters;
        parameters.Write("ProcessorState", "");
        ASSERT_TRUE(effect->LoadSettings(parameters, settings));
        const auto serialized = [&]() {
            CommandParameters saved;
            EXPECT_TRUE(effect->SaveSettings(settings, saved));
            wxString result;
            saved.GetParameters(result);
            return result;
        };
        const auto rejected = serialized();
        const auto editor = std::dynamic_pointer_cast<TracktionHostedEffectInstance>(effect->MakeInstance());
        ASSERT_TRUE(editor);
        ASSERT_FALSE(editor->loadSettings(settings, true));
        const auto access = std::make_shared<SimpleEffectSettingsAccess>(settings);
        const auto id = m_instances->regInstance(m_id, editor, access);
        {
            effects::VstViewModel model(nullptr, id);
            model.setContext(m_context);
            model.init();
            if (requestUpdate) {
                m_instances->requestUpdateSettings(id);
                EXPECT_EQ(serialized(), rejected);
                model.startPreview();
                EXPECT_EQ(m_preview->calls, 0);
            }
        }
        EXPECT_EQ(serialized(), rejected);
        m_instances->unregInstance(id);
    }
}

TEST_F(VstEditorTests, AValidStateRestoresEditorUpdatesAndPreviewAfterRejection)
{
    const auto* effect = m_loader->effect(m_id);
    ASSERT_TRUE(effect);
    auto settings = effect->MakeSettings();
    CommandParameters parameters;
    parameters.Write("ProcessorState", "");
    ASSERT_TRUE(effect->LoadSettings(parameters, settings));
    const auto editor = std::dynamic_pointer_cast<TracktionHostedEffectInstance>(effect->MakeInstance());
    ASSERT_TRUE(editor);
    const auto access = std::make_shared<SimpleEffectSettingsAccess>(settings);
    const auto id = m_instances->regInstance(m_id, editor, access);
    {
        effects::VstViewModel model(nullptr, id);
        model.setContext(m_context);
        model.init();
        auto valid = effect->MakeSettings();
        const auto source = std::dynamic_pointer_cast<TracktionHostedEffectInstance>(effect->MakeInstance());
        ASSERT_TRUE(source);
        ASSERT_TRUE(source->loadSettings(valid, true));
        const auto info = source->parameters();
        const auto gain = std::find_if(info.begin(), info.end(), [](const auto& value) { return value.name == u"Gain"; });
        ASSERT_NE(gain, info.end());
        ASSERT_TRUE(source->setParameterValue(gain->id, 0.25));
        ASSERT_TRUE(source->saveSettings(valid));
        access->Set(std::move(valid), nullptr);
        m_instances->notifyAboutSettingsChanged(id);
        EXPECT_NEAR(editor->parameterValue(gain->id), 0.25, 1e-6);
        ASSERT_TRUE(editor->setParameterValue(gain->id, 0.75));
        m_instances->requestUpdateSettings(id);
        ASSERT_TRUE(source->loadSettings(settings, true));
        EXPECT_NEAR(source->parameterValue(gain->id), 0.75, 1e-6);
        model.startPreview();
        EXPECT_EQ(m_preview->calls, 1);
    }
    m_instances->unregInstance(id);
}

TEST_F(VstLoaderTests, NativeLoaderKeepsIdentifiersAndCreatesTracktionInstances)
{
    EXPECT_EQ(m_loader->family(), effects::EffectFamily::VST3);
    EXPECT_CALL(*m_guard, beginLoad(m_id.toStdString())).Times(1);
    EXPECT_CALL(*m_guard, endLoad(m_id.toStdString())).Times(1);
    ASSERT_TRUE(m_loader->ensurePluginIsLoaded(m_id));
    const auto* effect = m_loader->effect(m_id);
    ASSERT_TRUE(effect);
    EXPECT_FALSE(dynamic_cast<const VST3EffectBase*>(effect));
    EXPECT_EQ(effects::utils::effectId(effect), m_id);
    EXPECT_TRUE(m_loader->ensurePluginIsLoaded(m_id));
    testing::Mock::VerifyAndClearExpectations(m_guard.get());
    const auto instance = std::dynamic_pointer_cast<TracktionHostedEffectInstance>(effect->MakeInstance());
    ASSERT_TRUE(instance);
    ASSERT_TRUE(instance->Init());
    EXPECT_TRUE(instance->vstController());
    auto settings = effect->MakeSettings();
    ASSERT_NO_FATAL_FAILURE(checkAudio(*instance, settings, 0.5f));
    m_loader->deinit();
    m_runtime.reset();
    const auto next = std::dynamic_pointer_cast<TracktionHostedEffectInstance>(effect->MakeInstance());
    ASSERT_TRUE(next);
    EXPECT_TRUE(next->Init());
}

TEST_F(VstLoaderTests, LoaderCreatesItsOwnHostWithoutOpeningAnAudioDevice)
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
    m_loader = std::make_unique<effects::Vst3EffectLoader>();
    ASSERT_TRUE(m_loader->ensurePluginIsLoaded(m_id));
    const auto* effect = m_loader->effect(m_id);
    ASSERT_TRUE(effect);
    const auto instance = std::dynamic_pointer_cast<TracktionHostedEffectInstance>(effect->MakeInstance());
    ASSERT_TRUE(instance);
    ASSERT_TRUE(instance->Init());
    EXPECT_FALSE(instance->parameters().empty());
    EXPECT_FALSE(instance->tracktionPlugin().edit.engine.getDeviceManager().deviceManager.getCurrentAudioDevice());
}

TEST_F(VstLoaderTests, RejectedProcessorStatePreservesTheEditorAndStopsOfflineProcessing)
{
    ASSERT_TRUE(m_loader->ensurePluginIsLoaded(m_id));
    const auto* effect = m_loader->effect(m_id);
    ASSERT_TRUE(effect);
    const auto editor = std::dynamic_pointer_cast<TracktionHostedEffectInstance>(effect->MakeInstance());
    ASSERT_TRUE(editor);
    auto settings = effect->MakeSettings();
    ASSERT_TRUE(editor->loadSettings(settings, true));
    const auto parameters = editor->parameters();
    const auto gain = std::find_if(parameters.begin(), parameters.end(), [](const auto& value) { return value.name == u"Gain"; });
    ASSERT_NE(gain, parameters.end());
    ASSERT_TRUE(editor->setParameterValue(gain->id, 0.25));
    ASSERT_TRUE(editor->saveSettings(settings));
    CommandParameters saved;
    ASSERT_TRUE(effect->SaveSettings(settings, saved));
    for (const auto* state : { "", "%%%" }) {
        SCOPED_TRACE(state);
        auto rejected = settings;
        CommandParameters bad;
        bad.Write("ProcessorState", wxString::FromUTF8(state));
        bad.Write("Parameters", wxString::FromUTF8(gain->id.toStdString()) + "=0.9;");
        ASSERT_TRUE(effect->LoadSettings(bad, rejected));
        EXPECT_FALSE(editor->loadSettings(rejected, true));
        EXPECT_NEAR(editor->parameterValue(gain->id), 0.25, 1e-6);
        EXPECT_FALSE(editor->settingsMatchProcessor(rejected));
        auto restored = effect->MakeSettings();
        ASSERT_TRUE(editor->saveSettings(restored));
        CommandParameters current;
        ASSERT_TRUE(effect->SaveSettings(restored, current));
        EXPECT_EQ(current.Read("ProcessorState"), saved.Read("ProcessorState"));
        const auto offline = std::dynamic_pointer_cast<TracktionHostedEffectInstance>(effect->MakeInstance());
        ASSERT_TRUE(offline);
        EXPECT_FALSE(offline->ProcessInitialize(rejected, 48000.0, nullptr));
        EXPECT_FALSE(offline->GetLastError().empty());
        offline->ProcessFinalize();
    }
}

TEST_F(VstLoaderTests, NativeFactoryProgramsKeepAudacityAndLegacySettings)
{
    ASSERT_TRUE(m_loader->ensurePluginIsLoaded(m_id));
    const auto* effect = m_loader->effect(m_id);
    ASSERT_TRUE(effect);
    const auto names = effect->GetFactoryPresets();
    const auto quarter = std::find(names.begin(), names.end(), wxT("Quarter"));
    ASSERT_NE(quarter, names.end());
    EXPECT_NE(std::find(names.begin(), names.end(), wxT("Half")), names.end());
    auto settings = effect->MakeSettings();
    ASSERT_TRUE(effect->LoadFactoryPreset(static_cast<int>(quarter - names.begin()), settings));
    const auto instance = std::dynamic_pointer_cast<TracktionHostedEffectInstance>(effect->MakeInstance());
    ASSERT_TRUE(instance);
    ASSERT_NO_FATAL_FAILURE(checkAudio(*instance, settings, 0.25f));
    CommandParameters saved;
    ASSERT_TRUE(effect->SaveSettings(settings, saved));
    EXPECT_TRUE(saved.HasEntry("ProcessorState"));
    VST3EffectsModule legacy;
    ASSERT_TRUE(legacy.Initialize());
    const auto component = legacy.LoadPlugin(effect->GetPath());
    const auto* oldEffect = dynamic_cast<const VST3EffectBase*>(component.get());
    ASSERT_TRUE(oldEffect);
    EXPECT_EQ(effect->GetSymbol(), oldEffect->GetSymbol());
    EXPECT_EQ(effect->GetVendor(), oldEffect->GetVendor());
    EXPECT_EQ(effect->GetVersion(), oldEffect->GetVersion());
    EXPECT_EQ(effect->GetDescription(), oldEffect->GetDescription());
    EXPECT_EQ(effect->GetType(), oldEffect->GetType());
    EXPECT_EQ(effect->GetFamily(), oldEffect->GetFamily());
    EXPECT_EQ(effect->SupportsRealtime(), oldEffect->SupportsRealtime());
    EXPECT_EQ(effect->ParamsAreInputAgnostic(), oldEffect->ParamsAreInputAgnostic());
    auto oldSettings = oldEffect->MakeSettings();
    ASSERT_TRUE(oldEffect->LoadSettings(saved, oldSettings));
    const auto oldInstance = std::dynamic_pointer_cast<EffectInstanceEx>(oldEffect->MakeInstance());
    ASSERT_TRUE(oldInstance);
    ASSERT_NO_FATAL_FAILURE(checkAudio(*oldInstance, oldSettings, 0.25f));
    EXPECT_FALSE(effect->LoadFactoryPreset(-1, settings));
    EXPECT_FALSE(effect->LoadFactoryPreset(static_cast<int>(names.size()), settings));
}

TEST_F(VstLoaderTests, UserPresetsKeepCurrentParametersAndFactoryDefaults)
{
    ASSERT_TRUE(m_loader->ensurePluginIsLoaded(m_id));
    const auto* effect = m_loader->effect(m_id);
    ASSERT_TRUE(effect);
    const auto instance = std::dynamic_pointer_cast<TracktionHostedEffectInstance>(effect->MakeInstance());
    ASSERT_TRUE(instance);
    const auto parameters = instance->parameters();
    const auto gain = std::find_if(parameters.begin(), parameters.end(), [](const auto& info) { return info.name == u"Gain"; });
    ASSERT_NE(gain, parameters.end());
    ASSERT_TRUE(instance->setParameterValue(gain->id, 0.875));
    auto settings = effect->MakeSettings();
    ASSERT_TRUE(instance->saveSettings(settings));
    const auto name = wxString::FromUTF8(juce::Uuid().toString().toStdString());
    ASSERT_TRUE(effect->SaveUserPreset(UserPresetsGroup(name), settings));
    auto restored = effect->MakeSettings();
    ASSERT_TRUE(effect->LoadUserPreset(UserPresetsGroup(name), restored));
    ASSERT_NO_FATAL_FAILURE(checkAudio(*instance, restored, 0.875f));
    ASSERT_TRUE(effect->LoadFactoryDefaults(restored));
    ASSERT_NO_FATAL_FAILURE(checkAudio(*instance, restored, 0.5f));
}

TEST_F(VstLoaderTests, UnvalidatedAndWrongFamilyEntriesDoNotLoadBinaries)
{
    EXPECT_CALL(*m_guard, beginLoad(testing::_)).Times(0);
    m_plugins.front().state = muse::audioplugins::AudioPluginState::Discovered;
    EXPECT_FALSE(m_loader->ensurePluginIsLoaded(m_id));
    m_plugins.front().state = muse::audioplugins::AudioPluginState::Validated;
    m_plugins.front().meta.type = "AudioUnit";
    EXPECT_FALSE(m_loader->ensurePluginIsLoaded(m_id));
    EXPECT_FALSE(m_loader->effect(m_id));
}

TEST_F(VstLoaderTests, AFailedCrashGuardPreventsLoading)
{
    EXPECT_CALL(*m_guard, beginLoad(m_id.toStdString())).WillOnce(testing::Return(muse::make_ret(muse::Ret::Code::InternalError)));
    EXPECT_CALL(*m_guard, endLoad(testing::_)).Times(0);
    EXPECT_FALSE(m_loader->ensurePluginIsLoaded(m_id));
    EXPECT_FALSE(m_loader->effect(m_id));
}

TEST_F(VstLoaderTests, MissingFullClassIdentifierDoesNotLoadAnotherClassFromTheBundle)
{
    m_id = m_id.left(m_id.size() - 32) + u"00000001000000000000000000000000";
    m_plugins.front().meta.id = m_id.toStdString();
    EXPECT_CALL(*m_guard, beginLoad(m_id.toStdString())).Times(1);
    EXPECT_CALL(*m_guard, endLoad(m_id.toStdString())).Times(1);
    EXPECT_FALSE(m_loader->ensurePluginIsLoaded(m_id));
    EXPECT_FALSE(m_loader->effect(m_id));
}
}
#endif
