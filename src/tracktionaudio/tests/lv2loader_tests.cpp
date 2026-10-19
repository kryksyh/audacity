/*
 * Audacity: A Digital Audio Editor
 */
#include <tracktion_engine/tracktion_engine.h>
#include <tracktion_graph/tracktion_graph.h>

#include <gtest/gtest.h>
#include <array>
#include <QTemporaryDir>
#include <QLibrary>
#include <QUrl>
#include <QEventLoop>
#if JUCE_MAC
#include <CoreFoundation/CoreFoundation.h>
#endif

#include "effects/lv2/internal/lv2effectloader.h"
#include "effects/lv2/internal/lv2parameterextractorservice.h"
#include "effects/effects_base/internal/effectsutils.h"
#include "../internal/tracktionpluginscanner.h"
#include "../internal/tracktioneffectinstance.h"
#include "../internal/tracktionlv2settings.h"
#include "../internal/tracktionlv2ui.h"
#include "plugin/lv2uiprobe.h"
#include "../internal/tracktionruntime.h"
#include "../internal/tracktionsession.h"
#include "../internal/tracktionrendersource.h"
#include "au3wrap/au3types.h"
#include "au3wrap/internal/au3project.h"
#include "au3-wave-track/WaveTrack.h"
#include "au3-wave-track/WaveClip.h"
#include "project/tests/testtools.h"
#include "../internal/audacityeffectregistry.h"
#include "au3-realtime-effects/RealtimeEffectState.h"
#include "au3-realtime-effects/RealtimeEffectList.h"
#include "au3-xml/XMLWriter.h"
#include "au3-xml/XMLFileReader.h"
#include "audioplugins/tests/mocks/knownaudiopluginsregistermock.h"
#include "audioplugins/tests/mocks/audiopluginsloadguardmock.h"
#include "au3-module-manager/ConfigInterface.h"
#include "global/defer.h"
#include "global/modularity/ioc.h"

namespace au::tracktionaudio {
namespace {
class Lv2LoaderTests : public ::testing::Test
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
        m_loader = std::make_unique<effects::Lv2EffectLoader>(m_runtime);
        ASSERT_NO_FATAL_FAILURE(select("gain"));
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

    void select(const char* name)
    {
        const auto bundle = QUrl::fromLocalFile(QString::fromUtf8(AU_TRACKTION_TEST_LV2_BUNDLE) + "/").toEncoded()
                            .replace("%25", "%%").toStdString();
        const auto path = std::string("urn:audacity:tracktion-test:") + name + "@" + bundle;
        const auto metadata = TracktionPluginMetaReader(PluginFormat::LV2).readMeta(path);
        ASSERT_TRUE(metadata.ret) << metadata.ret.toString();
        ASSERT_EQ(metadata.val.size(), 1u);
        m_plugins = { { metadata.val.front(), muse::io::path_t(path), muse::audioplugins::AudioPluginState::Validated } };
        m_id = muse::String::fromStdString(m_plugins.front().meta.id);
    }

    const Effect* load()
    {
        return m_loader->ensurePluginIsLoaded(m_id) ? m_loader->effect(m_id) : nullptr;
    }

    static std::shared_ptr<TracktionHostedEffectInstance> instance(const EffectInstanceFactory& effect)
    {
        return std::dynamic_pointer_cast<TracktionHostedEffectInstance>(effect.MakeInstance());
    }

    static float process(EffectInstance& instance, EffectSettings& settings, double rate, float input = 0.8f)
    {
        EXPECT_EQ(instance.SetBlockSize(64), 64u);
        EXPECT_TRUE(instance.ProcessInitialize(settings, rate, nullptr));
        std::vector<float> in(64, input), out(64);
        const float* inputs[] { in.data() };
        float* outputs[] { out.data() };
        EXPECT_EQ(instance.ProcessBlock(settings, inputs, outputs, 64), 64u);
        EXPECT_TRUE(instance.ProcessFinalize());
        return out.back();
    }

    static void pump()
    {
        const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(80);
        while (std::chrono::steady_clock::now() < end) {
            QCoreApplication::processEvents(QEventLoop::AllEvents);
#if JUCE_MAC
            CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.0, true);
#endif
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }

    static wxString encoded(const EffectInstanceFactory& effect, const EffectSettings& settings)
    {
        CommandParameters parameters;
        EXPECT_TRUE(effect.SaveSettings(settings, parameters));
        wxString result;
        EXPECT_TRUE(parameters.GetParameters(result));
        return result;
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
    std::unique_ptr<effects::Lv2EffectLoader> m_loader;
};
}

TEST_F(Lv2LoaderTests, DefinitionKeepsAudacityIdentityAndUsesTheNativeHost)
{
    EXPECT_EQ(m_loader->family(), effects::EffectFamily::LV2);
    EXPECT_CALL(*m_guard, beginLoad(m_id.toStdString())).Times(1);
    EXPECT_CALL(*m_guard, endLoad(m_id.toStdString())).Times(1);
    const auto* effect = load();
    ASSERT_TRUE(effect);
    EXPECT_EQ(effects::utils::effectId(effect), m_id);
    EXPECT_EQ(effect->GetFamily().Internal(), wxT("LV2"));
    EXPECT_EQ(effect->GetVersion(), wxT("1.0"));
    EXPECT_EQ(effect->GetType(), EffectTypeProcess);
    EXPECT_TRUE(effect->IsInteractive());
    EXPECT_TRUE(effect->SupportsAutomation());
    EXPECT_TRUE(effect->SupportsRealtime());
    EXPECT_TRUE(effect->HasOptions());
    EXPECT_FALSE(effect->CanExportPresets());
    EXPECT_TRUE(m_loader->ensurePluginIsLoaded(m_id));
    testing::Mock::VerifyAndClearExpectations(m_guard.get());
    const auto editor = instance(*effect);
    ASSERT_TRUE(editor);
    ASSERT_TRUE(editor->Init());
    EXPECT_TRUE(editor->tracktionPlugin().getAudioPluginInstance()->getLV2Client());
    EXPECT_FALSE(editor->vstComponent());
    EXPECT_FALSE(editor->tracktionPlugin().edit.engine.getDeviceManager().deviceManager.getCurrentAudioDevice());
    m_loader->deinit();
    m_runtime.reset();
    const auto next = instance(*effect);
    ASSERT_TRUE(next);
    EXPECT_TRUE(next->Init());
}

TEST_F(Lv2LoaderTests, LegacyNameParametersLoadAndNewSettingsUseTheRealCommandCodec)
{
    const auto* effect = load();
    ASSERT_TRUE(effect);
    auto settings = effect->MakeSettings();
    settings.extra.SetActive(false);
    CommandParameters legacy(wxT("Gain=\"1.25\""));
    ASSERT_TRUE(effect->LoadSettings(legacy, settings));
    EXPECT_FALSE(settings.extra.GetActive());
    const auto editor = instance(*effect);
    ASSERT_TRUE(editor);
    EXPECT_NEAR(process(*editor, settings, 48000.0), 1.0f, 1e-6f);
    ASSERT_TRUE(editor->setParameterValue(u"gain", 0.75));
    ASSERT_TRUE(editor->saveSettings(settings));
    CommandParameters saved;
    ASSERT_TRUE(saved.SetParameters(encoded(*effect, settings)));
    EXPECT_TRUE(saved.HasEntry("Gain"));
    EXPECT_TRUE(saved.HasEntry("__LV2Ports"));
    EXPECT_TRUE(saved.HasEntry("__LV2State"));
    double gain = 0.0;
    ASSERT_TRUE(saved.Read("Gain", &gain));
    EXPECT_DOUBLE_EQ(gain, 0.75);
    auto restored = effect->MakeSettings();
    ASSERT_TRUE(effect->LoadSettings(saved, restored));
    EXPECT_NEAR(process(*editor, restored, 96000.0), 0.6f, 1e-6f);
    const auto before = encoded(*effect, restored);
    for (const auto* invalid : { "", "Gain=\"3\"", "Gain=\"nan\"", "Gain=\"bad\"", "Gain=\"-1\"" }) {
        CommandParameters parameters(wxString::FromUTF8(invalid));
        EXPECT_FALSE(effect->LoadSettings(parameters, restored));
        EXPECT_EQ(encoded(*effect, restored), before);
    }
}

TEST_F(Lv2LoaderTests, DuplicateAndReservedNamesKeepSeparateValuesAfterSerialization)
{
    ASSERT_NO_FATAL_FAILURE(select("duplicates"));
    const auto* effect = load();
    ASSERT_TRUE(effect);
    const auto editor = instance(*effect);
    ASSERT_TRUE(editor);
    auto settings = effect->MakeSettings();
    ASSERT_TRUE(editor->loadSettings(settings, true));
    ASSERT_TRUE(editor->setParameterValue(u"gain", 0.75));
    ASSERT_TRUE(editor->setParameterValue(u"other", 0.10257736593484879));
    ASSERT_TRUE(editor->setParameterValue(u"state", 1.25));
    ASSERT_TRUE(editor->setParameterValue(u"ports", 1.75));
    ASSERT_TRUE(editor->saveSettings(settings));
    CommandParameters parameters(encoded(*effect, settings));
    EXPECT_TRUE(parameters.HasEntry("__LV2State_"));
    EXPECT_TRUE(parameters.HasEntry("__LV2Ports_"));
    auto restored = effect->MakeSettings();
    ASSERT_TRUE(effect->LoadSettings(parameters, restored));
    ASSERT_TRUE(editor->loadSettings(restored, true));
    EXPECT_DOUBLE_EQ(editor->parameterValue(u"gain"), 0.75);
    EXPECT_FLOAT_EQ(editor->parameterValue(u"other"), 0.10257736593484879f);
    EXPECT_DOUBLE_EQ(editor->parameterValue(u"state"), 1.25);
    EXPECT_DOUBLE_EQ(editor->parameterValue(u"ports"), 1.75);
    ASSERT_TRUE(parameters.Write("Gain", 1.5));
    ASSERT_TRUE(effect->LoadSettings(parameters, restored));
    ASSERT_TRUE(editor->loadSettings(restored, true));
    EXPECT_DOUBLE_EQ(editor->parameterValue(u"gain"), 1.5);
    EXPECT_DOUBLE_EQ(editor->parameterValue(u"other"), 1.5);
    CommandParameters legacy(wxT("Gain=\"0.25\" __LV2State=\"0.5\" __LV2Ports=\"1\""));
    ASSERT_TRUE(effect->LoadSettings(legacy, restored));
    ASSERT_TRUE(editor->loadSettings(restored, true));
    EXPECT_DOUBLE_EQ(editor->parameterValue(u"gain"), 0.25);
    EXPECT_DOUBLE_EQ(editor->parameterValue(u"other"), 0.25);
    EXPECT_DOUBLE_EQ(editor->parameterValue(u"state"), 0.5);
    EXPECT_DOUBLE_EQ(editor->parameterValue(u"ports"), 1.0);
}

TEST_F(Lv2LoaderTests, SampleRateSettingsKeepRelativeValuesAndShowPhysicalValues)
{
    ASSERT_NO_FATAL_FAILURE(select("sample-rate"));
    const auto* effect = load();
    ASSERT_TRUE(effect);
    auto settings = effect->MakeSettings();
    CommandParameters legacy(wxT("Gain=\"0.125\""));
    ASSERT_TRUE(effect->LoadSettings(legacy, settings));
    const auto editor = instance(*effect);
    ASSERT_TRUE(editor);
    EXPECT_NEAR(process(*editor, settings, 48000.0), 0.1f, 1e-6f);
    EXPECT_DOUBLE_EQ(editor->parameterValue(u"gain"), 6000.0);
    ASSERT_TRUE(editor->setParameterValue(u"gain", 18000.0));
    ASSERT_TRUE(editor->saveSettings(settings));
    CommandParameters saved(encoded(*effect, settings));
    double relative = 0.0;
    ASSERT_TRUE(saved.Read("Gain", &relative));
    EXPECT_DOUBLE_EQ(relative, 0.375);
    auto restored = effect->MakeSettings();
    ASSERT_TRUE(effect->LoadSettings(saved, restored));
    EXPECT_NEAR(process(*editor, restored, 96000.0), 0.3f, 1e-6f);
    const auto info = editor->parameter(u"gain");
    EXPECT_DOUBLE_EQ(info.currentValue, 36000.0);
    EXPECT_DOUBLE_EQ(info.maxValue, 48000.0);
    EXPECT_DOUBLE_EQ(info.defaultValue, 24000.0);
}

TEST_F(Lv2LoaderTests, FactoryAndUserPresetsKeepPrivateStateAndPartialControls)
{
    ASSERT_NO_FATAL_FAILURE(select("private"));
    const auto* effect = load();
    ASSERT_TRUE(effect);
    const auto names = effect->GetFactoryPresets();
    ASSERT_EQ(names.size(), 4u);
    const auto index = [&](const wxString& name) { return static_cast<int>(std::find(names.begin(), names.end(), name) - names.begin()); };
    auto settings = effect->MakeSettings();
    settings.extra.SetActive(false);
    ASSERT_TRUE(effect->LoadFactoryPreset(index("Quiet"), settings));
    EXPECT_FALSE(settings.extra.GetActive());
    const auto editor = instance(*effect);
    ASSERT_TRUE(editor);
    EXPECT_NEAR(process(*editor, settings, 48000.0), 0.3f, 1e-6f);
    ASSERT_TRUE(editor->setParameterValue(u"gain", 2.0));
    ASSERT_TRUE(editor->saveSettings(settings));
    ASSERT_TRUE(effect->LoadFactoryPreset(index("Partial"), settings));
    EXPECT_NEAR(process(*editor, settings, 48000.0), 0.8f, 1e-6f);
    const auto group = UserPresetsGroup(wxT("native-lv2-loader"));
    ASSERT_TRUE(effect->SaveUserPreset(group, settings));
    auto restored = effect->MakeSettings();
    ASSERT_TRUE(effect->LoadUserPreset(group, restored));
    EXPECT_NEAR(process(*editor, restored, 48000.0), 0.8f, 1e-6f);
    const auto before = encoded(*effect, restored);
    EXPECT_FALSE(effect->LoadFactoryPreset(index("Bad"), restored));
    EXPECT_FALSE(effect->LoadFactoryPreset(-1, restored));
    EXPECT_FALSE(effect->LoadFactoryPreset(4, restored));
    EXPECT_EQ(encoded(*effect, restored), before);
    ASSERT_TRUE(SetConfig(*effect, PluginSettings::Private, group, wxT("Parameters"), wxString("Gain=\"1.25\"")));
    ASSERT_TRUE(effect->LoadUserPreset(group, restored));
    EXPECT_NEAR(process(*editor, restored, 48000.0), 0.5f, 1e-6f);
    ASSERT_TRUE(effect->LoadFactoryDefaults(restored));
    EXPECT_NEAR(process(*editor, restored, 48000.0), 0.4f, 1e-6f);
}

TEST_F(Lv2LoaderTests, InvalidSerializedStateDoesNotReplaceSettingsOrProcessorState)
{
    ASSERT_NO_FATAL_FAILURE(select("private"));
    const auto* effect = load();
    ASSERT_TRUE(effect);
    auto settings = effect->MakeSettings();
    ASSERT_TRUE(effect->LoadFactoryPreset(2, settings));
    const auto editor = instance(*effect);
    ASSERT_TRUE(editor);
    ASSERT_TRUE(editor->loadSettings(settings, true));
    const auto before = encoded(*effect, settings);
    for (const auto* invalid : { "bad", "", "eA==" }) {
        CommandParameters parameters(before);
        parameters.Write("__LV2State", wxString::FromUTF8(invalid));
        EXPECT_FALSE(effect->LoadSettings(parameters, settings));
        EXPECT_EQ(encoded(*effect, settings), before);
    }
    for (const auto* invalid : { "[]", "{", "{}", "{\"gain\":\"1\"}" }) {
        CommandParameters parameters(before);
        parameters.Write("__LV2Ports", wxString::FromUTF8(QByteArray(invalid).toBase64().toStdString()));
        EXPECT_FALSE(effect->LoadSettings(parameters, settings));
        EXPECT_EQ(encoded(*effect, settings), before);
    }
    auto bad = settings;
    bad.cast<TracktionLv2Settings>()->state = std::make_shared<const TracktionLv2Settings::State>(8, 'x');
    EXPECT_FALSE(editor->loadSettings(bad, true));
    EXPECT_DOUBLE_EQ(editor->parameterValue(u"gain"), 1.5);
    EXPECT_NEAR(process(*editor, settings, 48000.0), 0.3f, 1e-6f);
    auto copy = settings;
    ASSERT_TRUE(editor->saveSettings(copy));
    EXPECT_TRUE(sameTracktionLv2State(copy, settings));
    EXPECT_EQ(copy.cast<TracktionLv2Settings>()->state, settings.cast<TracktionLv2Settings>()->state);
}

TEST_F(Lv2LoaderTests, GenericEditorUsesSymbolsAndKeepsAudacitySettings)
{
    ASSERT_NO_FATAL_FAILURE(select("rich"));
    const auto* effect = load();
    ASSERT_TRUE(effect);
    const auto editor = instance(*effect);
    ASSERT_TRUE(editor);
    auto settings = effect->MakeSettings();
    const auto access = std::make_shared<SimpleEffectSettingsAccess>(settings);
    effects::Lv2ParameterExtractorService extractor;
    extractor.beginParameterEditing(editor.get(), access);
    const auto parameters = extractor.extractParameters(editor.get(), access);
    ASSERT_EQ(parameters.size(), 4u);
    EXPECT_EQ(parameters[0].id, u"gain");
    EXPECT_EQ(parameters[0].group, u"Advanced Étage");
    EXPECT_EQ(parameters[0].units, u"db");
    EXPECT_TRUE(extractor.getParameter(editor.get(), u"frequency").isLogarithmic);
    EXPECT_EQ(extractor.getParameter(editor.get(), u"toggle").type, effects::ParameterType::Toggle);
    const auto mode = extractor.getParameter(editor.get(), u"mode");
    EXPECT_EQ(mode.type, effects::ParameterType::Dropdown);
    EXPECT_EQ(mode.enumValues.back(), u"Élevé");
    EXPECT_TRUE(extractor.getParameter(editor.get(), u"trigger").isHidden);
    EXPECT_FALSE(extractor.getParameter(editor.get(), u"meter").isValid());
    EXPECT_FALSE(extractor.setParameterValue(editor.get(), u"meter", 1.0, access));
    extractor.beginParameterGesture(editor.get(), u"gain", access);
    ASSERT_TRUE(extractor.setParameterValue(editor.get(), u"gain", 1.25, access));
    extractor.endParameterGesture(editor.get(), u"gain");
    CommandParameters saved(encoded(*effect, settings));
    double gain = 0.0;
    ASSERT_TRUE(saved.Read("Gain", &gain));
    EXPECT_DOUBLE_EQ(gain, 1.25);
    ASSERT_TRUE(extractor.setParameterValue(editor.get(), u"mode", 4.0, access));
    EXPECT_EQ(extractor.getParameterValueString(editor.get(), u"mode", 4.0), u"Élevé");
    extractor.endParameterEditing(editor.get());
    EXPECT_FLOAT_EQ(settings.cast<TracktionLv2Settings>()->values[3], 4.0f);
    EXPECT_NEAR(process(*editor, settings, 48000.0), 1.0f, 1e-6f);
    extractor.onInstanceDestroyed(editor.get());
}

TEST_F(Lv2LoaderTests, ActualPortsDetermineTheEffectTypeAndGuardsRejectLoads)
{
    struct Case {
        const char* name;
        ::EffectType type;
    };
    for (const auto& test : { Case { "generator", EffectTypeGenerate }, Case { "analyzer", EffectTypeAnalyze },
                              Case { "tool", EffectTypeTool } }) {
        ASSERT_NO_FATAL_FAILURE(select(test.name));
        m_plugins.front().meta.attributes[effects::EFFECT_TYPE_ATTRIBUTE] = effects::utils::effectTypeToString(
            effects::EffectType::Processor);
        const auto* effect = load();
        ASSERT_TRUE(effect);
        EXPECT_EQ(effect->GetType(), test.type);
        EXPECT_FALSE(effect->SupportsRealtime());
    }
    ASSERT_NO_FATAL_FAILURE(select("gain"));
    EXPECT_CALL(*m_guard, beginLoad(testing::_)).Times(0);
    m_plugins.front().state = muse::audioplugins::AudioPluginState::Discovered;
    EXPECT_FALSE(load());
    m_plugins.front().state = muse::audioplugins::AudioPluginState::Validated;
    m_plugins.front().meta.type = "VST3";
    EXPECT_FALSE(load());
    m_plugins.front().meta.type = "Lv2Plugin";
    m_plugins.front().path = "invalid";
    EXPECT_FALSE(load());
    testing::Mock::VerifyAndClearExpectations(m_guard.get());
    ASSERT_NO_FATAL_FAILURE(select("gain"));
    EXPECT_CALL(*m_guard, beginLoad(m_id.toStdString())).WillOnce(testing::Return(muse::make_ret(muse::Ret::Code::InternalError)));
    EXPECT_CALL(*m_guard, endLoad(testing::_)).Times(0);
    EXPECT_FALSE(load());
    testing::Mock::VerifyAndClearExpectations(m_guard.get());
    const auto* effect = load();
    ASSERT_TRUE(effect);
    EXPECT_CALL(*m_guard, beginLoad(m_id.toStdString())).WillOnce(testing::Return(muse::make_ret(muse::Ret::Code::InternalError)));
    EXPECT_CALL(*m_guard, endLoad(testing::_)).Times(0);
    EXPECT_FALSE(effect->MakeInstance());
}

TEST_F(Lv2LoaderTests, LiveAndRenderGraphsKeepPrivateStateThroughUndoAndProjectXml)
{
    ASSERT_NO_FATAL_FAILURE(select("private"));
    const auto* effect = load();
    ASSERT_TRUE(effect);
    const RealtimeEffectState::EffectFactory::Scope factory([&](const PluginID&) -> const EffectInstanceFactory* { return effect; });
    RealtimeEffectList list;
    const auto state = std::make_shared<RealtimeEffectState>(wxT("native-lv2-document"));
    ASSERT_TRUE(list.AddState(state));
    const auto editor = std::dynamic_pointer_cast<TracktionHostedEffectInstance>(state->GetInstance());
    ASSERT_TRUE(editor);
    const auto access = state->GetAccess();
    auto quiet = effect->MakeSettings();
    ASSERT_TRUE(effect->LoadFactoryPreset(2, quiet));
    ASSERT_FLOAT_EQ(quiet.cast<TracktionLv2Settings>()->values.front(), 1.5f);
    access->ModifySettings([&](EffectSettings& settings) { settings = quiet; return nullptr; });
    access->Flush();
    const auto id = m_runtime->effects()->add(state, list, nullptr, 2, true);
    DEFER { m_runtime->effects()->remove(id);
    };
    tracktion::Edit edit(m_runtime->engine(), tracktion::Edit::forEditing);
    juce::ValueTree value(tracktion::IDs::PLUGIN);
    value.setProperty(tracktion::IDs::type, AUDACITY_EFFECT_PLUGIN_TYPE, nullptr);
    value.setProperty(AUDACITY_EFFECT_STATE_PROPERTY, id, nullptr);
    const auto owned = edit.getPluginCache().createNewPlugin(value);
    const auto plugin = dynamic_cast<tracktion::ExternalPlugin*>(owned.get());
    ASSERT_TRUE(plugin);
    EXPECT_FLOAT_EQ(access->Get().cast<TracktionLv2Settings>()->values.front(), 1.5f);
    EXPECT_FLOAT_EQ(*plugin->getAudioPluginInstance()->getLV2Client()->getControlPortValue(2), 1.5f);
    plugin->baseClassInitialise({ tracktion::TimePosition(), 48000.0, 128 });
    EXPECT_FLOAT_EQ(*plugin->getAudioPluginInstance()->getLV2Client()->getControlPortValue(2), 1.5f);
    DEFER { plugin->baseClassDeinitialise();
    };
    const auto check = [&](tracktion::Plugin& plugin, float expected) {
        juce::AudioBuffer<float> audio(2, 128);
        for (int channel = 0; channel < 2; ++channel) {
            std::fill_n(audio.getWritePointer(channel), 128, 0.8f);
        }
        tracktion::PluginRenderContext context(&audio, 0, 128, nullptr, 0.0,
                                               { tracktion::TimePosition(), tracktion::TimePosition::fromSeconds(128.0 / 48000.0) },
                                               false, false, true, false);
        context.referenceSampleRange = { 0, 128 };
        context.referenceSampleRate = 48000.0;
        plugin.applyToBuffer(context);
        EXPECT_NEAR(audio.getSample(0, 127), expected, 1e-6f);
        EXPECT_NEAR(audio.getSample(1, 127), expected, 1e-6f);
    };
    check(*plugin, 0.3f);
    const auto undo = state->Clone();
    ASSERT_TRUE(editor->setParameterValue(u"gain", 2.0));
    pump();
    check(*plugin, 0.4f);
    access->ModifySettings([&](EffectSettings& settings) { settings = undo->GetAccess()->Get(); return nullptr; });
    access->Flush();
    pump();
    check(*plugin, 0.3f);
    EXPECT_EQ(plugin->getAudioPluginInstance(), editor->tracktionPlugin().getAudioPluginInstance());
    XMLStringWriter xml;
    state->WriteXML(xml);
    const auto reloaded = std::make_shared<RealtimeEffectState>(wxT(""));
    XMLFileReader reader;
    ASSERT_TRUE(reader.ParseString(reloaded.get(), xml));
    const auto reopened = std::dynamic_pointer_cast<TracktionHostedEffectInstance>(reloaded->GetInstance());
    ASSERT_TRUE(reopened);
    auto settings = reloaded->GetAccess()->Get();
    EXPECT_NEAR(process(*reopened, settings, 48000.0), 0.3f, 1e-6f);
    const auto renderId = m_runtime->effects()->add(state, list, nullptr, 2, false);
    DEFER { m_runtime->effects()->remove(renderId);
    };
    tracktion::Edit render(m_runtime->engine(), tracktion::Edit::forRendering);
    value.setProperty(AUDACITY_EFFECT_STATE_PROPERTY, renderId, nullptr);
    const auto independent = render.getPluginCache().createNewPlugin(value);
    const auto other = dynamic_cast<tracktion::ExternalPlugin*>(independent.get());
    ASSERT_TRUE(other);
    EXPECT_NE(other->getAudioPluginInstance(), plugin->getAudioPluginInstance());
    other->baseClassInitialise({ tracktion::TimePosition(), 48000.0, 128 });
    DEFER { other->baseClassDeinitialise();
    };
    check(*other, 0.3f);
}

TEST_F(Lv2LoaderTests, VendorEditorControlsTheLiveInstanceAndEveryMonoProcessor)
{
    const auto* plain = load();
    ASSERT_TRUE(plain);
    EXPECT_FALSE(effects::tracktionLv2VendorUiSupported(*plain));
    ASSERT_NO_FATAL_FAILURE(select("ui-echo"));
    const auto* effect = load();
    ASSERT_TRUE(effect);
    EXPECT_TRUE(effects::tracktionLv2VendorUiSupported(*effect));
    const RealtimeEffectState::EffectFactory::Scope factory([&](const PluginID&) -> const EffectInstanceFactory* { return effect; });
    RealtimeEffectList list;
    const auto state = std::make_shared<RealtimeEffectState>(wxT("native-lv2-vendor"));
    ASSERT_TRUE(list.AddState(state));
    const auto editor = std::dynamic_pointer_cast<TracktionHostedEffectInstance>(state->GetInstance());
    ASSERT_TRUE(editor);
    ASSERT_TRUE(editor->loadSettings(state->GetAccess()->Get(), true));
    auto* processor = editor->tracktionPlugin().getAudioPluginInstance();
    ASSERT_TRUE(processor);
    QLibrary library(QString::fromStdString(processor->getLV2Client()->getUis().front().binaryPath.toStdString()));
    const auto setGain = reinterpret_cast<void (*)(float)>(library.resolve("tracktion_ui_gain"));
    ASSERT_TRUE(setGain);
    TracktionLv2Ui ui(*processor, {});
    ASSERT_TRUE(ui.open());
    setGain(1.25f);
    const auto id = m_runtime->effects()->add(state, list, nullptr, 2, true);
    DEFER { m_runtime->effects()->remove(id);
    };
    tracktion::Edit edit(m_runtime->engine(), tracktion::Edit::forEditing);
    juce::ValueTree value(tracktion::IDs::PLUGIN);
    value.setProperty(tracktion::IDs::type, AUDACITY_EFFECT_PLUGIN_TYPE, nullptr);
    value.setProperty(AUDACITY_EFFECT_STATE_PROPERTY, id, nullptr);
    const auto owned = edit.getPluginCache().createNewPlugin(value);
    const auto plugin = dynamic_cast<tracktion::ExternalPlugin*>(owned.get());
    ASSERT_TRUE(plugin);
    EXPECT_EQ(plugin->getAudioPluginInstance(), processor);
    plugin->baseClassInitialise({ tracktion::TimePosition(), 48000.0, 128 });
    DEFER { plugin->baseClassDeinitialise();
    };
    const auto check = [&](float expected) {
        juce::AudioBuffer<float> audio(2, 128);
        for (int channel = 0; channel < 2; ++channel) {
            std::fill_n(audio.getWritePointer(channel), 128, 0.8f);
        }
        tracktion::PluginRenderContext context(&audio, 0, 128, nullptr, 0.0,
                                               { tracktion::TimePosition(), tracktion::TimePosition::fromSeconds(128.0 / 48000.0) },
                                               false, false, true, false);
        context.referenceSampleRange = { 0, 128 };
        context.referenceSampleRate = 48000.0;
        plugin->applyToBuffer(context);
        EXPECT_NEAR(audio.getSample(0, 127), expected, 1e-6f);
        EXPECT_NEAR(audio.getSample(1, 127), expected, 1e-6f);
    };
    check(1.0f);
    setGain(1.75f);
    pump();
    check(1.4f);
    EXPECT_FLOAT_EQ(state->GetAccess()->Get().cast<TracktionLv2Settings>()->values.front(), 1.75f);
    EXPECT_TRUE(ui.idle());
    ui.close();
}

TEST_F(Lv2LoaderTests, EditorAtomChangesAreSavedBeforePlaybackAndReachOfflineProcessing)
{
    ASSERT_NO_FATAL_FAILURE(select("ui-state"));
    const auto* effect = load();
    ASSERT_TRUE(effect);
    const auto editor = instance(*effect);
    ASSERT_TRUE(editor);
    auto settings = effect->MakeSettings();
    ASSERT_TRUE(editor->loadSettings(settings, true));
    auto* processor = editor->tracktionPlugin().getAudioPluginInstance();
    QLibrary library(QString::fromStdString(processor->getLV2Client()->getUis().front().binaryPath.toStdString()));
    const auto setState = reinterpret_cast<void (*)(float)>(library.resolve("tracktion_ui_atom"));
    ASSERT_TRUE(setState);
    TracktionLv2Ui ui(*processor, {});
    ASSERT_TRUE(ui.open());
    setState(0.25f);
    ASSERT_TRUE(editor->saveSettings(settings));
    const auto preview = instance(*effect);
    ASSERT_TRUE(preview);
    EXPECT_NEAR(process(*preview, settings, 48000.0), 0.1f, 1e-6f);
}

TEST_F(Lv2LoaderTests, EditorIdleReportsPrivateChangesAndKeepsStateAcrossSerialization)
{
    ASSERT_NO_FATAL_FAILURE(select("ui-state"));
    const auto* effect = load();
    ASSERT_TRUE(effect);
    const auto editor = instance(*effect);
    ASSERT_TRUE(editor);
    auto settings = effect->MakeSettings();
    ASSERT_TRUE(editor->loadSettings(settings, true));
    auto* processor = editor->tracktionPlugin().getAudioPluginInstance();
    QLibrary library(QString::fromStdString(processor->getLV2Client()->getUis().front().binaryPath.toStdString()));
    const auto setState = reinterpret_cast<void (*)(float)>(library.resolve("tracktion_ui_atom"));
    ASSERT_TRUE(setState);
    TracktionLv2Ui ui(*processor, {});
    ASSERT_TRUE(ui.open());
    editor->processEditorIdle();
    EXPECT_FALSE(editor->takeSettingsChanges());
    setState(0.25f);
    editor->processEditorIdle();
    EXPECT_TRUE(editor->takeSettingsChanges());
    const auto active = reinterpret_cast<bool (*)(const void*)>(library.resolve("tracktion_dsp_active"));
    const auto probe = reinterpret_cast<const Lv2UiProbe* (*)()>(library.resolve("tracktion_ui_probe"));
    ASSERT_TRUE(active);
    ASSERT_TRUE(probe);
    const auto handle = probe()->instance;
    EXPECT_TRUE(active(handle));
    ASSERT_TRUE(editor->saveSettings(settings));
    CommandParameters parameters(encoded(*effect, settings));
    auto restored = effect->MakeSettings();
    ASSERT_TRUE(effect->LoadSettings(parameters, restored));
    const auto preview = instance(*effect);
    ASSERT_TRUE(preview);
    EXPECT_NEAR(process(*preview, restored, 48000.0), 0.1f, 1e-6f);
    EXPECT_FALSE(editor->tracktionPlugin().edit.engine.getDeviceManager().deviceManager.getCurrentAudioDevice());
    ui.close();
    EXPECT_FALSE(active(handle));
}

TEST_F(Lv2LoaderTests, EditorIdleDoesNotRunDuringLiveProcessingOrDeactivateItsProcessor)
{
    ASSERT_NO_FATAL_FAILURE(select("ui-state"));
    const auto* effect = load();
    ASSERT_TRUE(effect);
    const RealtimeEffectState::EffectFactory::Scope factory([&](const PluginID&) -> const EffectInstanceFactory* { return effect; });
    RealtimeEffectList list;
    const auto state = std::make_shared<RealtimeEffectState>(wxT("lv2-editor-idle-owner"));
    ASSERT_TRUE(list.AddState(state));
    const auto editor = std::dynamic_pointer_cast<TracktionHostedEffectInstance>(state->GetInstance());
    ASSERT_TRUE(editor);
    ASSERT_TRUE(editor->loadSettings(state->GetAccess()->Get(), true));
    auto* processor = editor->tracktionPlugin().getAudioPluginInstance();
    QLibrary library(QString::fromStdString(processor->getLV2Client()->getUis().front().binaryPath.toStdString()));
    const auto setState = reinterpret_cast<void (*)(float)>(library.resolve("tracktion_ui_atom"));
    const auto active = reinterpret_cast<bool (*)(const void*)>(library.resolve("tracktion_dsp_active"));
    const auto probe = reinterpret_cast<const Lv2UiProbe* (*)()>(library.resolve("tracktion_ui_probe"));
    ASSERT_TRUE(setState);
    ASSERT_TRUE(active);
    ASSERT_TRUE(probe);
    TracktionLv2Ui ui(*processor, {});
    ASSERT_TRUE(ui.open());
    editor->processEditorIdle();
    const auto id = m_runtime->effects()->add(state, list, nullptr, 1, true);
    DEFER { m_runtime->effects()->remove(id);
    };
    tracktion::Edit edit(m_runtime->engine(), tracktion::Edit::forEditing);
    juce::ValueTree value(tracktion::IDs::PLUGIN);
    value.setProperty(tracktion::IDs::type, AUDACITY_EFFECT_PLUGIN_TYPE, nullptr);
    value.setProperty(AUDACITY_EFFECT_STATE_PROPERTY, id, nullptr);
    const auto owned = edit.getPluginCache().createNewPlugin(value);
    const auto plugin = dynamic_cast<tracktion::ExternalPlugin*>(owned.get());
    ASSERT_TRUE(plugin);
    plugin->baseClassInitialise({ tracktion::TimePosition(), 48000.0, 64 });
    DEFER { plugin->baseClassDeinitialise();
    };
    setState(0.25f);
    editor->processEditorIdle();
    auto beforeAudio = effect->MakeSettings();
    ASSERT_TRUE(editor->saveSettings(beforeAudio));
    const auto preview = instance(*effect);
    ASSERT_TRUE(preview);
    EXPECT_NEAR(process(*preview, beforeAudio, 48000.0), 0.4f, 1e-6f);
    juce::AudioBuffer<float> audio(1, 64);
    std::fill_n(audio.getWritePointer(0), 64, 0.8f);
    tracktion::PluginRenderContext context(&audio, 0, 64, nullptr, 0.0,
                                           { tracktion::TimePosition(), tracktion::TimePosition::fromSeconds(64.0 / 48000.0) },
                                           false, false, true, false);
    context.referenceSampleRange = { 0, 64 };
    context.referenceSampleRate = 48000.0;
    plugin->applyToBuffer(context);
    EXPECT_NEAR(audio.getSample(0, 63), 0.1f, 1e-6f);
    const auto handle = probe()->instance;
    EXPECT_TRUE(active(handle));
    ui.close();
    EXPECT_TRUE(active(handle));
}

TEST_F(Lv2LoaderTests, OfflineLatencyIncludesTheFirstProcessedBlockAndHonoursTheOption)
{
    ASSERT_NO_FATAL_FAILURE(select("latency-late"));
    const auto* effect = load();
    ASSERT_TRUE(effect);
    bool previousLatency = true;
    GetConfig(*effect, PluginSettings::Shared, wxT("Options"), wxT("UseLatency"), previousLatency, true);
    DEFER { SetConfig(*effect, PluginSettings::Shared, wxT("Options"), wxT("UseLatency"), previousLatency);
    };
    for (const bool useLatency : { false, true }) {
        SCOPED_TRACE(useLatency);
        ASSERT_TRUE(SetConfig(*effect, PluginSettings::Shared, wxT("Options"), wxT("UseLatency"), useLatency));
        const auto processor = instance(*effect);
        ASSERT_TRUE(processor);
        auto settings = effect->MakeSettings();
        ASSERT_EQ(processor->SetBlockSize(64), 64u);
        ASSERT_TRUE(processor->ProcessInitialize(settings, 48000.0, nullptr));
        DEFER { processor->ProcessFinalize();
        };
        std::array<float, 64> input {}, output {};
        input[0] = 1.0f;
        const float* inputs[] { input.data() };
        float* outputs[] { output.data() };
        ASSERT_EQ(processor->ProcessBlock(settings, inputs, outputs, input.size()), input.size());
        EXPECT_FLOAT_EQ(output[0], 0.0f);
        EXPECT_FLOAT_EQ(output[17], 0.5f);
        EXPECT_EQ(processor->GetLatency(settings, 48000.0), useLatency ? 17 : 0);
    }
}

TEST_F(Lv2LoaderTests, LiveAndRenderLatencyAndBypassHonourTheAudacityOption)
{
    ASSERT_NO_FATAL_FAILURE(select("latency"));
    const auto* effect = load();
    ASSERT_TRUE(effect);
    bool previousLatency = true;
    GetConfig(*effect, PluginSettings::Shared, wxT("Options"), wxT("UseLatency"), previousLatency, true);
    DEFER { SetConfig(*effect, PluginSettings::Shared, wxT("Options"), wxT("UseLatency"), previousLatency);
    };
    const RealtimeEffectState::EffectFactory::Scope factory([&](const PluginID&) -> const EffectInstanceFactory* { return effect; });
    for (const bool useLatency : { false, true }) {
        SCOPED_TRACE(useLatency);
        ASSERT_TRUE(SetConfig(*effect, PluginSettings::Shared, wxT("Options"), wxT("UseLatency"), useLatency));
        for (const bool live : { false, true }) {
            SCOPED_TRACE(live);
            for (const bool bypassed : { false, true }) {
                SCOPED_TRACE(bypassed);
                RealtimeEffectList list;
                const auto state = std::make_shared<RealtimeEffectState>(wxT("lv2-latency"));
                ASSERT_TRUE(list.AddState(state));
                state->SetActive(!bypassed);
                const auto id = m_runtime->effects()->add(state, list, nullptr, 1, live);
                DEFER { m_runtime->effects()->remove(id);
                };
                tracktion::Edit edit(m_runtime->engine(), tracktion::Edit::forEditing);
                juce::ValueTree value(tracktion::IDs::PLUGIN);
                value.setProperty(tracktion::IDs::type, AUDACITY_EFFECT_PLUGIN_TYPE, nullptr);
                value.setProperty(AUDACITY_EFFECT_STATE_PROPERTY, id, nullptr);
                const auto owned = edit.getPluginCache().createNewPlugin(value);
                const auto plugin = dynamic_cast<tracktion::ExternalPlugin*>(owned.get());
                ASSERT_TRUE(plugin);
                plugin->baseClassInitialise({ tracktion::TimePosition(), 48000.0, 64 });
                DEFER { plugin->baseClassDeinitialise();
                };
                EXPECT_NEAR(plugin->getLatencySeconds() * 48000.0, useLatency ? 17.0 : 0.0, 1e-6);
                juce::AudioBuffer<float> audio(1, 64);
                audio.clear();
                audio.setSample(0, 0, 1.0f);
                tracktion::PluginRenderContext context(&audio, 0, 64, nullptr, 0.0,
                                                       { tracktion::TimePosition(), tracktion::TimePosition::fromSeconds(64.0 / 48000.0) },
                                                       live, false, !live, false);
                context.referenceSampleRange = { 0, 64 };
                context.referenceSampleRate = 48000.0;
                plugin->applyToBuffer(context);
                const int impulse = !bypassed || useLatency ? 17 : 0;
                for (int frame = 0; frame < 64; ++frame) {
                    EXPECT_FLOAT_EQ(audio.getSample(0, frame), frame == impulse ? (bypassed ? 1.0f : 0.5f) : 0.0f) << frame;
                }
            }
        }
    }
}

TEST_F(Lv2LoaderTests, ExportAlignsTrackAndMasterLatencyAndKeepsTheOptionAndBypass)
{
    for (const char* pluginName : { "latency", "latency-late", "latency-worker" }) {
        SCOPED_TRACE(pluginName);
        ASSERT_NO_FATAL_FAILURE(select(pluginName));
        const auto* effect = load();
        ASSERT_TRUE(effect);
        bool previousLatency = true;
        GetConfig(*effect, PluginSettings::Shared, wxT("Options"), wxT("UseLatency"), previousLatency, true);
        DEFER { SetConfig(*effect, PluginSettings::Shared, wxT("Options"), wxT("UseLatency"), previousLatency);
        };
        const auto project = std::make_shared<au3::Au3ProjectAccessor>(muse::modularity::globalCtx());
        const auto projectPath = m_directory.filePath(QString::fromUtf8(pluginName) + ".aup4").toStdString();
        const auto fixture = std::string(tracktionaudio_tests_DATA_ROOT) + "/../../trackedit/tests/data/empty.aup4";
        ASSERT_TRUE(testtools::copyFile(fixture, projectPath));
        ASSERT_TRUE(project->load(muse::io::path_t(projectPath), false));
        DEFER { project->clearSavedState();
                project->close();
        };
        auto& au3Project = *reinterpret_cast<au3::Au3Project*>(project->au3ProjectPtr());
        auto& tracks = TrackList::Get(au3Project);
        const auto track = WaveTrackFactory::Get(au3Project).Create(2, floatSample, 48000.0);
        std::vector<float> samples(4800);
        samples[100] = samples[2397] = samples[4790] = 0.8f;
        const auto clip = track->CreateClip(0.0, "latency");
        constSamplePtr channels[] { reinterpret_cast<constSamplePtr>(samples.data()), reinterpret_cast<constSamplePtr>(samples.data()) };
        clip->Append(channels, floatSample, samples.size(), 1, floatSample);
        clip->Flush();
        track->InsertInterval(clip, true);
        tracks.Add(track);
        const RealtimeEffectState::EffectFactory::Scope factory([&](const PluginID&) -> const EffectInstanceFactory* { return effect; });
        const auto trackState = std::make_shared<RealtimeEffectState>(wxT("lv2-export-latency"));
        const auto masterState = std::make_shared<RealtimeEffectState>(wxT("lv2-export-latency"));
        ASSERT_TRUE(RealtimeEffectList::Get(*track).AddState(trackState));
        ASSERT_TRUE(RealtimeEffectList::Get(au3Project).AddState(masterState));
        for (const bool useLatency : { false, true }) {
            SCOPED_TRACE(useLatency);
            ASSERT_TRUE(SetConfig(*effect, PluginSettings::Shared, wxT("Options"), wxT("UseLatency"), useLatency));
            for (const bool bypassed : { false, true }) {
                SCOPED_TRACE(bypassed);
                trackState->SetActive(!bypassed);
                masterState->SetActive(!bypassed);
                TracktionSession session(*m_runtime, tracks, TracktionSession::Role::Render);
                TracktionRenderSource::Options options;
                options.endTime = 0.1;
                options.sampleRate = 48000.0;
                options.blockSize = 64;
                TracktionRenderSource source(session, options);
                const int delay = !useLatency && !bypassed ? 34 : 0;
                int offset = 0;
                while (const int frames = source.process(257)) {
                    ASSERT_LE(offset + frames, static_cast<int>(samples.size()));
                    for (int frame = 0; frame < frames; ++frame) {
                        const int input = offset + frame - delay;
                        const float expected = input < 0 ? 0.0f : samples[input] * (bypassed ? 1.0f : 0.25f);
                        for (int channel = 0; channel < 2; ++channel) {
                            ASSERT_NEAR(source.buffer().getSample(channel, frame), expected, 1e-6f) << offset + frame;
                        }
                    }
                    offset += frames;
                }
                EXPECT_EQ(offset, static_cast<int>(samples.size()));
            }
        }
    }
}

TEST_F(Lv2LoaderTests, VendorEditorUsesTheProjectRateBeforePlaybackAndKeepsItsStateWhenTheRateChanges)
{
    ASSERT_NO_FATAL_FAILURE(select("ui-state"));
    const auto* effect = load();
    ASSERT_TRUE(effect);
    const auto editor = instance(*effect);
    ASSERT_TRUE(editor);
    auto settings = effect->MakeSettings();
    ASSERT_TRUE(editor->loadSettings(settings, true));
    ASSERT_TRUE(editor->prepareLv2Editor(48000.0));
    auto* processor = editor->tracktionPlugin().getAudioPluginInstance();
    QLibrary library(QString::fromStdString(processor->getLV2Client()->getUis().front().binaryPath.toStdString()));
    const auto probe = reinterpret_cast<const Lv2UiProbe* (*)()>(library.resolve("tracktion_ui_probe"));
    const auto setState = reinterpret_cast<void (*)(float)>(library.resolve("tracktion_ui_atom"));
    ASSERT_TRUE(probe);
    ASSERT_TRUE(setState);
    TracktionLv2Ui ui(*processor, {});
    ASSERT_TRUE(ui.open());
    EXPECT_FLOAT_EQ(probe()->sampleRate, 48000.0f);
    setState(0.25f);
    editor->processEditorIdle();
    const int opened = probe()->opened;
    ASSERT_TRUE(editor->prepareLv2Editor(96000.0));
    EXPECT_EQ(probe()->opened, opened + 1);
    EXPECT_FLOAT_EQ(probe()->sampleRate, 96000.0f);
    ASSERT_TRUE(editor->saveSettings(settings));
    const auto preview = instance(*effect);
    ASSERT_TRUE(preview);
    EXPECT_NEAR(process(*preview, settings, 96000.0), 0.1f, 1e-6f);
    EXPECT_FALSE(m_runtime->engine().getDeviceManager().deviceManager.getCurrentAudioDevice());
}

TEST_F(Lv2LoaderTests, EditorRateChangesDoNotChangeAnActiveGraphOrTheRateOfAReusedGraph)
{
    ASSERT_NO_FATAL_FAILURE(select("ui-state"));
    const auto* effect = load();
    ASSERT_TRUE(effect);
    const RealtimeEffectState::EffectFactory::Scope factory([&](const PluginID&) -> const EffectInstanceFactory* { return effect; });
    RealtimeEffectList list;
    const auto state = std::make_shared<RealtimeEffectState>(wxT("lv2-editor-rate"));
    ASSERT_TRUE(list.AddState(state));
    const auto editor = std::dynamic_pointer_cast<TracktionHostedEffectInstance>(state->GetInstance());
    ASSERT_TRUE(editor);
    ASSERT_TRUE(editor->loadSettings(state->GetAccess()->Get(), true));
    ASSERT_TRUE(editor->prepareLv2Editor(48000.0));
    auto* processor = editor->tracktionPlugin().getAudioPluginInstance();
    QLibrary library(QString::fromStdString(processor->getLV2Client()->getUis().front().binaryPath.toStdString()));
    const auto probe = reinterpret_cast<const Lv2UiProbe* (*)()>(library.resolve("tracktion_ui_probe"));
    const auto active = reinterpret_cast<bool (*)(const void*)>(library.resolve("tracktion_dsp_active"));
    ASSERT_TRUE(probe);
    ASSERT_TRUE(active);
    TracktionLv2Ui ui(*processor, {});
    ASSERT_TRUE(ui.open());
    const auto id = m_runtime->effects()->add(state, list, nullptr, 1, true);
    DEFER { m_runtime->effects()->remove(id);
    };
    tracktion::Edit edit(m_runtime->engine(), tracktion::Edit::forEditing);
    juce::ValueTree value(tracktion::IDs::PLUGIN);
    value.setProperty(tracktion::IDs::type, AUDACITY_EFFECT_PLUGIN_TYPE, nullptr);
    value.setProperty(AUDACITY_EFFECT_STATE_PROPERTY, id, nullptr);
    const auto owned = edit.getPluginCache().createNewPlugin(value);
    const auto plugin = dynamic_cast<tracktion::ExternalPlugin*>(owned.get());
    ASSERT_TRUE(plugin);
    const auto runGraph = [&](bool requestEditorRate, bool closeEditor) {
        plugin->baseClassInitialise({ tracktion::TimePosition(), 48000.0, 64 });
        DEFER { plugin->baseClassDeinitialise();
        };
        EXPECT_DOUBLE_EQ(processor->getLV2Client()->getInstanceSampleRate(), 48000.0);
        EXPECT_DOUBLE_EQ(processor->getSampleRate(), 48000.0);
        EXPECT_EQ(processor->getBlockSize(), 64);
        EXPECT_FLOAT_EQ(probe()->sampleRate, 48000.0f);
        if (requestEditorRate) {
            const int opened = probe()->opened;
            ASSERT_TRUE(editor->prepareLv2Editor(96000.0));
            EXPECT_EQ(probe()->opened, opened);
            EXPECT_DOUBLE_EQ(processor->getLV2Client()->getInstanceSampleRate(), 48000.0);
        }
        if (closeEditor) {
            ui.close();
        }
        EXPECT_TRUE(active(probe()->instance));
        juce::AudioBuffer<float> audio(1, 64);
        std::fill_n(audio.getWritePointer(0), 64, 0.8f);
        tracktion::PluginRenderContext context(&audio, 0, 64, nullptr, 0.0,
                                               { tracktion::TimePosition(), tracktion::TimePosition::fromSeconds(64.0 / 48000.0) },
                                               false, false, true, false);
        context.referenceSampleRange = { 0, 64 };
        context.referenceSampleRate = 48000.0;
        plugin->applyToBuffer(context);
        EXPECT_NEAR(audio.getSample(0, 63), 0.4f, 1e-6f);
    };
    ASSERT_NO_FATAL_FAILURE(runGraph(true, false));
    ASSERT_TRUE(editor->prepareLv2Editor(96000.0));
    EXPECT_FLOAT_EQ(probe()->sampleRate, 96000.0f);
    ASSERT_NO_FATAL_FAILURE(runGraph(false, false));
    ASSERT_TRUE(editor->prepareLv2Editor(48000.0));
    EXPECT_EQ(processor->getBlockSize(), m_runtime->engine().getDeviceManager().getBlockSize());
    ASSERT_NO_FATAL_FAILURE(runGraph(false, true));
}

TEST_F(Lv2LoaderTests, GenericEnabledControlKeepsLv2ValuesThroughSavedStateAndAudioProcessing)
{
    ASSERT_NO_FATAL_FAILURE(select("enabled"));
    const auto* effect = load();
    ASSERT_TRUE(effect);
    const auto editor = instance(*effect);
    ASSERT_TRUE(editor);
    auto settings = effect->MakeSettings();
    ASSERT_TRUE(editor->loadSettings(settings, true));
    auto* processor = editor->tracktionPlugin().getAudioPluginInstance();
    ASSERT_TRUE(processor);
    auto* bypass = processor->getBypassParameter();
    ASSERT_TRUE(bypass);
    const auto parameter = editor->parameter(u"enabled");
    EXPECT_EQ(parameter.type, effects::ParameterType::Toggle);
    EXPECT_DOUBLE_EQ(parameter.defaultValue, 1.0);
    for (const float enabled : { 0.0f, 1.0f }) {
        SCOPED_TRACE(enabled);
        ASSERT_TRUE(editor->setParameterValue(u"enabled", enabled));
        EXPECT_DOUBLE_EQ(editor->parameterValue(u"enabled"), enabled);
        EXPECT_FLOAT_EQ(bypass->getValue(), 1.0f - enabled);
        ASSERT_TRUE(editor->saveSettings(settings));
        const auto restored = instance(*effect);
        ASSERT_TRUE(restored);
        ASSERT_TRUE(restored->loadSettings(settings, true));
        EXPECT_DOUBLE_EQ(restored->parameterValue(u"enabled"), enabled);
        EXPECT_NEAR(process(*restored, settings, 48000.0), enabled > 0.0f ? 0.4f : 0.8f, 1e-6f);
    }
}

TEST_F(Lv2LoaderTests, BypassedEditorChangesReachSavedStateWithoutChangingAudio)
{
    ASSERT_NO_FATAL_FAILURE(select("ui-state"));
    const auto* effect = load();
    ASSERT_TRUE(effect);
    const RealtimeEffectState::EffectFactory::Scope factory([&](const PluginID&) -> const EffectInstanceFactory* { return effect; });
    RealtimeEffectList list;
    const auto state = std::make_shared<RealtimeEffectState>(wxT("lv2-bypassed-editor"));
    ASSERT_TRUE(list.AddState(state));
    const auto editor = std::dynamic_pointer_cast<TracktionHostedEffectInstance>(state->GetInstance());
    ASSERT_TRUE(editor);
    ASSERT_TRUE(editor->loadSettings(state->GetAccess()->Get(), true));
    auto* processor = editor->tracktionPlugin().getAudioPluginInstance();
    QLibrary library(QString::fromStdString(processor->getLV2Client()->getUis().front().binaryPath.toStdString()));
    const auto setState = reinterpret_cast<void (*)(float)>(library.resolve("tracktion_ui_atom"));
    ASSERT_TRUE(setState);
    TracktionLv2Ui ui(*processor, {});
    ASSERT_TRUE(ui.open());
    const auto id = m_runtime->effects()->add(state, list, nullptr, 1, true);
    DEFER { m_runtime->effects()->remove(id);
    };
    tracktion::Edit edit(m_runtime->engine(), tracktion::Edit::forEditing);
    juce::ValueTree value(tracktion::IDs::PLUGIN);
    value.setProperty(tracktion::IDs::type, AUDACITY_EFFECT_PLUGIN_TYPE, nullptr);
    value.setProperty(AUDACITY_EFFECT_STATE_PROPERTY, id, nullptr);
    const auto owned = edit.getPluginCache().createNewPlugin(value);
    const auto plugin = dynamic_cast<tracktion::ExternalPlugin*>(owned.get());
    ASSERT_TRUE(plugin);
    plugin->baseClassInitialise({ tracktion::TimePosition(), 48000.0, 64 });
    DEFER { plugin->baseClassDeinitialise();
    };
    for (const bool bypassList : { false, true }) {
        SCOPED_TRACE(bypassList);
        state->SetActive(bypassList);
        list.SetActive(!bypassList);
        const float privateGain = bypassList ? 0.5f : 0.25f;
        setState(privateGain);
        editor->processEditorIdle();
        auto saved = effect->MakeSettings();
        ASSERT_TRUE(editor->saveSettings(saved));
        const auto preview = instance(*effect);
        ASSERT_TRUE(preview);
        EXPECT_NEAR(process(*preview, saved, 48000.0), 0.4f * privateGain, 1e-6f);
        pump();
        juce::AudioBuffer<float> audio(1, 64);
        std::fill_n(audio.getWritePointer(0), 64, 0.8f);
        tracktion::PluginRenderContext context(&audio, 0, 64, nullptr, 0.0,
                                               { tracktion::TimePosition(), tracktion::TimePosition::fromSeconds(64.0 / 48000.0) },
                                               false, false, true, false);
        context.referenceSampleRange = { 0, 64 };
        context.referenceSampleRate = 48000.0;
        plugin->applyToBuffer(context);
        EXPECT_FLOAT_EQ(audio.getSample(0, 63), 0.8f);
    }
}

TEST_F(Lv2LoaderTests, PausedEditorChangesReachSavedStateWithoutMovingThePlayhead)
{
    auto& manager = m_runtime->engine().getDeviceManager();
    auto& audioIO = manager.getHostedAudioDeviceInterface();
    tracktion::HostedAudioDeviceInterface::Parameters parameters;
    parameters.sampleRate = 48000.0;
    parameters.blockSize = 64;
    parameters.inputChannels = 0;
    parameters.outputChannels = 2;
    audioIO.initialise(parameters);
    audioIO.prepareToPlay(48000.0, 64);
    manager.dispatchPendingUpdates();
    DEFER { manager.deviceManager.closeAudioDevice();
            manager.removeHostedAudioDeviceInterface();
    };
    ASSERT_NO_FATAL_FAILURE(select("ui-state"));
    const auto* effect = load();
    ASSERT_TRUE(effect);
    const RealtimeEffectState::EffectFactory::Scope factory([&](const PluginID&) -> const EffectInstanceFactory* { return effect; });
    RealtimeEffectList list;
    const auto state = std::make_shared<RealtimeEffectState>(wxT("lv2-paused-editor"));
    ASSERT_TRUE(list.AddState(state));
    const auto editor = std::dynamic_pointer_cast<TracktionHostedEffectInstance>(state->GetInstance());
    ASSERT_TRUE(editor);
    ASSERT_TRUE(editor->loadSettings(state->GetAccess()->Get(), true));
    auto* processor = editor->tracktionPlugin().getAudioPluginInstance();
    QLibrary library(QString::fromStdString(processor->getLV2Client()->getUis().front().binaryPath.toStdString()));
    const auto setState = reinterpret_cast<void (*)(float)>(library.resolve("tracktion_ui_atom"));
    ASSERT_TRUE(setState);
    TracktionLv2Ui ui(*processor, {});
    ASSERT_TRUE(ui.open());
    const auto id = m_runtime->effects()->add(state, list, nullptr, 1, true);
    DEFER { m_runtime->effects()->remove(id);
    };
    tracktion::Edit edit(m_runtime->engine(), tracktion::Edit::forEditing);
    juce::ValueTree value(tracktion::IDs::PLUGIN);
    value.setProperty(tracktion::IDs::type, AUDACITY_EFFECT_PLUGIN_TYPE, nullptr);
    value.setProperty(AUDACITY_EFFECT_STATE_PROPERTY, id, nullptr);
    const auto owned = edit.getPluginCache().createNewPlugin(value);
    const auto plugin = dynamic_cast<tracktion::ExternalPlugin*>(owned.get());
    ASSERT_TRUE(plugin);
    plugin->baseClassInitialise({ tracktion::TimePosition(), 48000.0, 64 });
    DEFER { plugin->baseClassDeinitialise();
    };
    auto& transport = edit.getTransport();
    transport.play(false);
    DEFER { transport.stop(false, false);
    };
    ASSERT_TRUE(transport.isPlaying());
    juce::AudioBuffer<float> audio(2, 64);
    juce::MidiBuffer midi;
    audio.clear();
    audioIO.processBlock(audio, midi);
    transport.setPaused(true);
    ASSERT_TRUE(transport.isPaused());
    const auto* context = edit.getCurrentPlaybackContext();
    ASSERT_TRUE(context);
    const auto position = context->getNodePlayHead()->getPosition();
    setState(0.25f);
    editor->processEditorIdle();
    auto saved = effect->MakeSettings();
    ASSERT_TRUE(editor->saveSettings(saved));
    const auto preview = instance(*effect);
    ASSERT_TRUE(preview);
    EXPECT_NEAR(process(*preview, saved, 48000.0), 0.1f, 1e-6f);
    audio.clear();
    audioIO.processBlock(audio, midi);
    EXPECT_EQ(context->getNodePlayHead()->getPosition(), position);
    EXPECT_FLOAT_EQ(audio.getRMSLevel(0, 0, 64), 0.0f);
}
}
