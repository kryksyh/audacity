/*
* Audacity: A Digital Audio Editor
*/
#include <tracktion_engine/tracktion_engine.h>

#include <gtest/gtest.h>
#include <QTemporaryDir>
#include <QCoreApplication>
#include <QEventLoop>
#include <thread>
#include <stdexcept>

#include "../internal/tracktioneffectinstance.h"
#include "../internal/tracktionruntime.h"
#include "../internal/audacityeffectregistry.h"
#include "au3-realtime-effects/RealtimeEffectState.h"
#include "au3-realtime-effects/RealtimeEffectList.h"
#include "effects/vst/internal/vstparameterextractorservice.h"
#include "effects/vst/internal/musevstplugininstance.h"
#include "effects/vst/internal/musevstpluginsregister.h"
#include "effects/effects_base/internal/effectsutils.h"
#include "basstreble/basstrebleeffect.h"
#include "audioplugins/tests/mocks/audiopluginsloadguardmock.h"
#include "global/defer.h"
#include "global/modularity/ioc.h"

#if AU_TRACKTION_TEST_VST3
#include "au3-vst3/VST3EffectsModule.h"
#include "au3-vst3/VST3Instance.h"
#include "au3-vst3/VST3ParameterExtraction.h"
#include "au3-module-manager/ModuleManager.h"
#if JUCE_MAC
#include <CoreFoundation/CoreFoundation.h>
#endif

namespace au::tracktionaudio {
namespace {
class HostedEffectTests : public ::testing::Test
{
protected:
    void SetUp() override
    {
        ASSERT_TRUE(m_directory.isValid());
        VST3EffectsModule provider;
        ASSERT_TRUE(provider.Initialize());
        PluginID id;
        PluginPath path;
        TranslatableString error;
        ASSERT_EQ(provider.DiscoverPluginsAtPath(wxString::FromUTF8(AU_TRACKTION_TEST_VST3_PATH), error,
                                                 [&](PluginProvider*, ComponentInterface* component) -> const PluginID& {
            path = component->GetPath();
            return id;
        }), 1u);
        m_component = provider.LoadPlugin(path);
        m_effect = dynamic_cast<PerTrackEffect*>(m_component.get());
        ASSERT_TRUE(m_effect);
        m_runtime.init(m_directory.filePath("cache").toStdString(), m_directory.filePath("settings").toStdString());
    }

    std::shared_ptr<TracktionHostedEffectInstance> makeEditor()
    {
        return std::dynamic_pointer_cast<TracktionHostedEffectInstance>(createTracktionEffectInstance(m_runtime, *m_effect));
    }

    void TearDown() override
    {
        for (const auto& id : m_effectIds) {
            m_runtime.effects()->remove(id);
        }
    }

    void useTracktionEditors()
    {
        m_editorFactory = std::make_unique<EffectInstanceFactory::EditorInstanceFactory::Scope>([this](const EffectInstanceFactory& effect) {
            return createTracktionEffectInstance(m_runtime,
                                                 effect);
        });
        m_effectFactory = std::make_unique<RealtimeEffectState::EffectFactory::Scope>([this](const PluginID&) -> const EffectInstanceFactory* {
            return m_effect;
        });
    }

    tracktion::ExternalPlugin::Ptr makePlaybackPlugin(tracktion::Edit& edit, const std::shared_ptr<RealtimeEffectState>& state,
                                                      RealtimeEffectList& list, int channels = 2, bool live = true)
    {
        const auto id = m_runtime.effects()->add(state, list, nullptr, channels, live);
        m_effectIds.push_back(id);
        juce::ValueTree value(tracktion::IDs::PLUGIN);
        value.setProperty(tracktion::IDs::type, AUDACITY_EFFECT_PLUGIN_TYPE, nullptr);
        value.setProperty(AUDACITY_EFFECT_STATE_PROPERTY, id, nullptr);
        const auto plugin = edit.getPluginCache().createNewPlugin(value);
        return dynamic_cast<tracktion::ExternalPlugin*>(plugin.get());
    }

    static void pump()
    {
        const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(45);
        while (std::chrono::steady_clock::now() < end) {
            QCoreApplication::processEvents(QEventLoop::AllEvents);
#if JUCE_MAC
            CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.0, true);
#endif
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }

    static void process(tracktion::ExternalPlugin& plugin, float expected, double start = 5.0)
    {
        juce::AudioBuffer<float> audio(2, 128);
        for (int block = 0; block < 4; ++block) {
            for (int channel = 0; channel < 2; ++channel) {
                std::fill_n(audio.getWritePointer(channel), 128, 0.4f);
            }
            const auto time = tracktion::TimePosition::fromSeconds(start + block * 128.0 / 48000.0);
            tracktion::PluginRenderContext context(&audio, 0, 128, nullptr, 0.0,
                                                   { time, time + tracktion::TimeDuration::fromSamples(128, 48000.0) },
                                                   true, false, false, false);
            context.referenceSampleRange = { block* 128, (block + 1) * 128 };
            context.referenceSampleRate = 48000.0;
            plugin.applyToBuffer(context);
        }
        for (int channel = 0; channel < 2; ++channel) {
            for (int frame = 0; frame < 128; ++frame) {
                ASSERT_NEAR(audio.getSample(channel, frame), expected, 1e-6f);
            }
        }
    }

    QTemporaryDir m_directory;
    TracktionRuntime m_runtime;
    std::unique_ptr<ComponentInterface> m_component;
    PerTrackEffect* m_effect = nullptr;
    std::vector<juce::String> m_effectIds;
    std::unique_ptr<EffectInstanceFactory::EditorInstanceFactory::Scope> m_editorFactory;
    std::unique_ptr<RealtimeEffectState::EffectFactory::Scope> m_effectFactory;
};
}

TEST_F(HostedEffectTests, EditorFactorySelectsTracktionForDestructiveAndRealtimeEffects)
{
    const EffectInstanceFactory::EditorInstanceFactory::Scope factory([&](const EffectInstanceFactory& effect) {
        return createTracktionEffectInstance(m_runtime, effect);
    });
    const auto editor = std::dynamic_pointer_cast<TracktionHostedEffectInstance>(m_effect->MakeEditorInstance());
    ASSERT_TRUE(editor);
    ASSERT_TRUE(editor->Init()) << editor->GetLastError();
    EXPECT_TRUE(editor->vstController());
    EXPECT_TRUE(editor->vstComponent());
    EXPECT_FALSE(std::dynamic_pointer_cast<VST3Instance>(editor));
    const RealtimeEffectState::EffectFactory::Scope effectFactory([&](const PluginID&) -> const EffectInstanceFactory* {
        return m_effect;
    });
    RealtimeEffectState state(wxT("native-vst3-editor"));
    const auto realtimeEditor = std::dynamic_pointer_cast<TracktionHostedEffectInstance>(state.GetInstance());
    ASSERT_TRUE(realtimeEditor);
    EXPECT_TRUE(realtimeEditor->vstController());
    effects::BassTrebleEffect builtin;
    EXPECT_FALSE(std::dynamic_pointer_cast<TracktionHostedEffectInstance>(builtin.MakeEditorInstance()));
}

TEST_F(HostedEffectTests, ParameterGesturesKeepAudacitySettingsAndAudio)
{
    const auto editor = makeEditor();
    ASSERT_TRUE(editor);
    ASSERT_TRUE(editor->Init()) << editor->GetLastError();
    auto settings = m_effect->MakeSettings();
    const auto access = std::make_shared<SimpleEffectSettingsAccess>(settings);
    effects::VstParameterExtractorService extractor;
    extractor.beginParameterEditing(editor.get(), access);
    const auto parameters = extractor.extractParameters(editor.get(), access);
    const auto gain = std::find_if(parameters.begin(), parameters.end(), [](const auto& info) { return info.name == u"Gain"; });
    ASSERT_NE(gain, parameters.end());
    EXPECT_DOUBLE_EQ(gain->minValue, 0.0);
    EXPECT_DOUBLE_EQ(gain->maxValue, 1.0);
    EXPECT_DOUBLE_EQ(gain->defaultValue, 0.5);
    EXPECT_DOUBLE_EQ(gain->currentValue, 0.5);
    EXPECT_FALSE(editor->takeSettingsChanges());
    extractor.beginParameterGesture(editor.get(), gain->id, access);
    ASSERT_TRUE(extractor.setParameterValue(editor.get(), gain->id, 0.25, access));
    EXPECT_DOUBLE_EQ(extractor.getParameterValue(editor.get(), gain->id), 0.25);
    extractor.endParameterGesture(editor.get(), gain->id);
    EXPECT_TRUE(editor->takeSettingsChanges());
    CommandParameters saved;
    ASSERT_TRUE(m_effect->SaveSettings(settings, saved));
    EXPECT_TRUE(saved.HasEntry("ProcessorState"));
    EXPECT_TRUE(saved.HasEntry("Parameters"));
    auto restored = m_effect->MakeSettings();
    ASSERT_TRUE(m_effect->LoadSettings(saved, restored));
    const auto second = makeEditor();
    ASSERT_TRUE(second);
    ASSERT_TRUE(second->loadSettings(restored, true));
    EXPECT_DOUBLE_EQ(second->parameterValue(gain->id), 0.25);

    const auto legacy = m_effect->MakeInstance();
    ASSERT_TRUE(legacy);
    ASSERT_EQ(legacy->SetBlockSize(128), 128u);
    ASSERT_TRUE(legacy->ProcessInitialize(restored, 44100.0, nullptr));
    std::vector<float> input(128, 0.4f), left(128), right(128);
    const float* inputs[] { input.data(), input.data() };
    float* outputs[] { left.data(), right.data() };
    EXPECT_EQ(legacy->ProcessBlock(restored, inputs, outputs, 128), 128u);
    EXPECT_NEAR(left[80], 0.1f, 1e-6f);
    EXPECT_NEAR(right[80], 0.1f, 1e-6f);
    EXPECT_TRUE(legacy->ProcessFinalize());
    extractor.endParameterEditing(editor.get());
    extractor.onInstanceDestroyed(editor.get());
}

TEST_F(HostedEffectTests, Vst3PresetFilesUpdateParametersAndRejectAnotherClass)
{
    const auto source = makeEditor();
    ASSERT_TRUE(source);
    ASSERT_TRUE(source->Init());
    const auto parameters = source->parameters();
    const auto gain = std::find_if(parameters.begin(), parameters.end(), [](const auto& info) { return info.name == u"Gain"; });
    ASSERT_NE(gain, parameters.end());
    ASSERT_TRUE(source->setParameterValue(gain->id, 0.875));
    auto sourceSettings = m_effect->MakeSettings();
    ASSERT_TRUE(source->saveSettings(sourceSettings));
    const auto* processor = source->tracktionPlugin().getAudioPluginInstance();
    ASSERT_TRUE(processor);
    const auto* client = processor->getVST3Client();
    ASSERT_TRUE(client);
    const auto preset = client->getPreset();
    ASSERT_GT(preset.getSize(), 40u);
    const juce::File file(m_directory.filePath(QString::fromUtf8("preset-\xc3\xa9.vstpreset")).toStdString());
    ASSERT_TRUE(file.replaceWithData(preset.getData(), preset.getSize()));
    const auto target = makeEditor();
    ASSERT_TRUE(target);
    ASSERT_TRUE(target->loadVst3PresetFile(file.getFullPathName().toStdString()));
    EXPECT_NEAR(target->parameterValue(gain->id), 0.875, 1e-6);
    auto settings = m_effect->MakeSettings();
    ASSERT_TRUE(target->saveSettings(settings));
    const auto restored = makeEditor();
    ASSERT_TRUE(restored);
    ASSERT_TRUE(restored->loadSettings(settings, true));
    EXPECT_NEAR(restored->parameterValue(gain->id), 0.875, 1e-6);
    auto wrongClass = preset;
    std::fill_n(static_cast<char*>(wrongClass.getData()) + 8, 32, '0');
    ASSERT_TRUE(file.replaceWithData(wrongClass.getData(), wrongClass.getSize()));
    EXPECT_FALSE(target->loadVst3PresetFile(file.getFullPathName().toStdString()));
    EXPECT_NEAR(target->parameterValue(gain->id), 0.875, 1e-6);
    EXPECT_FALSE(target->loadVst3PresetFile(m_directory.filePath("missing.vstpreset").toStdString()));
}

TEST_F(HostedEffectTests, EditorRetainsTheRuntimeAndUsesItsLoadedController)
{
    const auto editor = makeEditor();
    ASSERT_TRUE(editor);
    ASSERT_TRUE(editor->Init());
    const auto id = effects::utils::effectId(m_effect);
    effects::MuseVstPluginInstance first(id, editor->id(), editor);
    effects::MuseVstPluginInstance second(id + u"-second", editor->id() + 1, editor);
    EXPECT_TRUE(first.isLoaded());
    EXPECT_EQ(first.controller().get(), editor->vstController());
    EXPECT_EQ(first.component().get(), editor->vstComponent());
    EXPECT_EQ(first.resourceId(), id.toStdString());
    EXPECT_NE(first.resourceId(), second.resourceId());
    m_runtime.deinit();
    EXPECT_FALSE(editor->parameters().empty());
    auto settings = m_effect->MakeSettings();
    EXPECT_TRUE(editor->saveSettings(settings));
}

TEST_F(HostedEffectTests, ParameterMetadataMatchesTheVst3ControllerAndKeepsNonautomatableValues)
{
    const auto editor = makeEditor();
    ASSERT_TRUE(editor);
    auto settings = m_effect->MakeSettings();
    ASSERT_TRUE(editor->loadSettings(settings, true));
    const auto parameters = editor->parameters();
    const auto frequency = std::find_if(parameters.begin(), parameters.end(), [](const auto& info) { return info.name == u"Frequency"; });
    const auto mode = std::find_if(parameters.begin(), parameters.end(), [](const auto& info) { return info.name == u"Mode"; });
    ASSERT_NE(frequency, parameters.end());
    ASSERT_NE(mode, parameters.end());
    const auto legacy = std::dynamic_pointer_cast<EffectInstanceEx>(m_effect->MakeInstance());
    ASSERT_TRUE(legacy);
    const auto expected = VST3ParameterExtraction::extractParameters(legacy.get());
    const auto expectedFrequency
        = std::find_if(expected.begin(), expected.end(), [](const auto& info) { return info.name == "Frequency"; });
    const auto expectedMode = std::find_if(expected.begin(), expected.end(), [](const auto& info) { return info.name == "Mode"; });
    ASSERT_NE(expectedFrequency, expected.end());
    ASSERT_NE(expectedMode, expected.end());
    EXPECT_DOUBLE_EQ(frequency->minValue, expectedFrequency->minValue);
    EXPECT_DOUBLE_EQ(frequency->maxValue, expectedFrequency->maxValue);
    EXPECT_DOUBLE_EQ(frequency->defaultValue, expectedFrequency->defaultValue);
    EXPECT_EQ(frequency->units, u"Hz");
    EXPECT_FALSE(frequency->canAutomate);
    EXPECT_EQ(mode->type, effects::ParameterType::Dropdown);
    EXPECT_EQ(mode->stepCount, 2);
    EXPECT_EQ(mode->enumValues, (std::vector<muse::String> { u"A", u"B", u"C" }));
    EXPECT_EQ(mode->enumIndices, expectedMode->enumIndices);
    ASSERT_TRUE(editor->setParameterValue(frequency->id, 0.73));
    ASSERT_TRUE(editor->setParameterValue(mode->id, expectedMode->enumIndices.back()));
    EXPECT_EQ(editor->parameterValueString(frequency->id, 0.73).toStdString(),
              VST3ParameterExtraction::getParameterValueString(legacy.get(), expectedFrequency->id, 0.73));
    ASSERT_TRUE(editor->saveSettings(settings));
    const auto second = makeEditor();
    ASSERT_TRUE(second);
    ASSERT_TRUE(second->loadSettings(settings, true));
    EXPECT_NEAR(second->parameterValue(frequency->id), 0.73, 1e-6);
    EXPECT_DOUBLE_EQ(second->parameterValue(mode->id), expectedMode->enumIndices.back());
    EXPECT_FALSE(second->takeSettingsChanges());
    EXPECT_FALSE(second->parameter(u"invalid").isValid());
    EXPECT_FALSE(second->setParameterValue(u"invalid", 0.5));
}

TEST_F(HostedEffectTests, OpenInstancesOfTheSameVst3RemainAvailableByIdentifier)
{
    const auto firstEditor = makeEditor();
    const auto secondEditor = makeEditor();
    ASSERT_TRUE(firstEditor);
    ASSERT_TRUE(secondEditor);
    const auto effectId = effects::utils::effectId(m_effect);
    const auto first = std::make_shared<effects::MuseVstPluginInstance>(effectId, firstEditor->id(), firstEditor);
    const auto second = std::make_shared<effects::MuseVstPluginInstance>(effectId, secondEditor->id(), secondEditor);
    effects::MuseVstInstancesRegister registry;
    registry.registerFxPlugin(0, 0, first);
    registry.registerFxPlugin(0, 0, second);
    EXPECT_EQ(registry.instanceById(first->id()), first);
    EXPECT_EQ(registry.instanceById(second->id()), second);
    registry.unregisterById(first->id());
    EXPECT_FALSE(registry.instanceById(first->id()));
    EXPECT_EQ(registry.instanceById(second->id()), second);
    registry.unregisterFxPlugin(first->resourceId(), 0, 0);
    EXPECT_FALSE(registry.instanceById(second->id()));
}

TEST_F(HostedEffectTests, HostedInstanceLoadUsesTheExistingCrashGuard)
{
    using Guard = muse::audioplugins::IAudioPluginsLoadGuard;
    auto* ioc = muse::modularity::globalIoc();
    const auto previous = ioc->resolve<Guard>("utests");
    if (previous) {
        ioc->unregister<Guard>("utests");
    }
    const auto guard = std::make_shared<testing::StrictMock<muse::audioplugins::AudioPluginsLoadGuardMock> >();
    ioc->registerExport<Guard>("utests", guard);
    DEFER {
        ioc->unregister<Guard>("utests");
        if (previous) {
            ioc->registerExport<Guard>("utests", previous);
        }
    };
    const auto id = effects::utils::effectId(m_effect).toStdString();
    testing::InSequence sequence;
    EXPECT_CALL(*guard, beginLoad(id)).WillOnce(testing::Return(muse::make_ok()));
    EXPECT_CALL(*guard, endLoad(id));
    const auto editor = makeEditor();
    ASSERT_TRUE(editor);
    EXPECT_TRUE(editor->Init());
}

TEST_F(HostedEffectTests, PlaybackKeepsTheEditorProcessorAndDoesNotRestoreItsOwnSnapshots)
{
    useTracktionEditors();
    RealtimeEffectList list;
    const auto state = std::make_shared<RealtimeEffectState>(wxT("shared-vst3"));
    ASSERT_TRUE(list.AddState(state));
    const auto editor = std::dynamic_pointer_cast<TracktionHostedEffectInstance>(state->GetInstance());
    ASSERT_TRUE(editor);
    const auto parameters = editor->parameters();
    const auto gain = std::find_if(parameters.begin(), parameters.end(), [](const auto& info) { return info.name == u"Gain"; });
    const auto output = std::find_if(parameters.begin(), parameters.end(), [](const auto& info) { return info.name == u"Output"; });
    const auto restores
        = std::find_if(parameters.begin(), parameters.end(), [](const auto& info) { return info.name == u"State Restores"; });
    ASSERT_NE(gain, parameters.end());
    ASSERT_NE(output, parameters.end());
    ASSERT_NE(restores, parameters.end());
    const auto reads = std::find_if(parameters.begin(), parameters.end(), [](const auto& info) { return info.name == u"State Reads"; });
    ASSERT_NE(reads, parameters.end());
    ASSERT_TRUE(output->isReadOnly);
    ASSERT_TRUE(editor->setParameterValue(gain->id, 0.25));
    tracktion::Edit edit(m_runtime.engine(), tracktion::Edit::forEditing);
    const auto plugin = makePlaybackPlugin(edit, state, list);
    ASSERT_TRUE(plugin);
    EXPECT_EQ(plugin->getAudioPluginInstance(), editor->tracktionPlugin().getAudioPluginInstance());
    plugin->baseClassInitialise({ tracktion::TimePosition(), 48000.0, 128 });
    DEFER { plugin->baseClassDeinitialise();
    };
    ASSERT_NO_FATAL_FAILURE(process(*plugin, 0.1f));
    pump();
    const double restoreCount = editor->parameterValue(restores->id);
    const double readCount = editor->parameterValue(reads->id);
    EXPECT_NEAR(editor->parameterValue(output->id), 0.1, 1e-6);
    editor->takeSettingsChanges();
    ASSERT_TRUE(editor->setParameterValue(gain->id, 0.75));
    ASSERT_NO_FATAL_FAILURE(process(*plugin, 0.3f));
    pump();
    ASSERT_NO_FATAL_FAILURE(process(*plugin, 0.3f));
    pump();
    EXPECT_DOUBLE_EQ(editor->parameterValue(restores->id), restoreCount);
    EXPECT_DOUBLE_EQ(editor->parameterValue(reads->id), readCount);
    EXPECT_TRUE(editor->takeSettingsChanges());
    ASSERT_NO_FATAL_FAILURE(process(*plugin, 0.3f));
    pump();
    EXPECT_FALSE(editor->takeSettingsChanges());
    const auto& settings = state->GetAccess()->Get();
    const auto second = makeEditor();
    ASSERT_TRUE(second);
    ASSERT_TRUE(second->loadSettings(settings, true));
    EXPECT_NEAR(second->parameterValue(gain->id), 0.75, 1e-6);
}

TEST_F(HostedEffectTests, RejectedStateStopsLiveAndRenderInitialisationAndValidStateCanLoad)
{
    useTracktionEditors();
    for (const bool live : { false, true }) {
        SCOPED_TRACE(live);
        RealtimeEffectList list;
        const auto state = std::make_shared<RealtimeEffectState>(wxT("rejected-vst3-state"));
        ASSERT_TRUE(list.AddState(state));
        const auto editor = std::dynamic_pointer_cast<TracktionHostedEffectInstance>(state->GetInstance());
        ASSERT_TRUE(editor);
        auto rejected = m_effect->MakeSettings();
        CommandParameters bad;
        bad.Write("ProcessorState", "");
        ASSERT_TRUE(m_effect->LoadSettings(bad, rejected));
        const auto access = state->GetAccess();
        access->Set(std::move(rejected));
        access->Flush();
        tracktion::Edit edit(m_runtime.engine(), tracktion::Edit::forEditing);
        EXPECT_THROW({
                const auto plugin = makePlaybackPlugin(edit, state, list, 2, live);
                ASSERT_TRUE(plugin);
                plugin->baseClassInitialise({ tracktion::TimePosition(), 48000.0, 128 });
            }, std::runtime_error);
        auto valid = m_effect->MakeSettings();
        ASSERT_TRUE(editor->loadSettings(valid, true));
        const auto parameters = editor->parameters();
        const auto gain = std::find_if(parameters.begin(), parameters.end(), [](const auto& value) { return value.name == u"Gain"; });
        ASSERT_NE(gain, parameters.end());
        ASSERT_TRUE(editor->setParameterValue(gain->id, 0.25));
        ASSERT_TRUE(editor->saveSettings(valid));
        access->Set(std::move(valid));
        access->Flush();
        const auto plugin = makePlaybackPlugin(edit, state, list, 2, live);
        ASSERT_TRUE(plugin);
        plugin->baseClassInitialise({ tracktion::TimePosition(), 48000.0, 128 });
        DEFER { plugin->baseClassDeinitialise();
        };
        ASSERT_NO_FATAL_FAILURE(process(*plugin, 0.1f));
    }
}

TEST_F(HostedEffectTests, DocumentParameterChangesKeepProcessorStateAndRemovingParametersRestoresThePreset)
{
    useTracktionEditors();
    RealtimeEffectList list;
    const auto state = std::make_shared<RealtimeEffectState>(wxT("document-vst3-parameters"));
    ASSERT_TRUE(list.AddState(state));
    const auto editor = std::dynamic_pointer_cast<TracktionHostedEffectInstance>(state->GetInstance());
    ASSERT_TRUE(editor);
    const auto parameters = editor->parameters();
    const auto gain = std::find_if(parameters.begin(), parameters.end(), [](const auto& info) { return info.name == u"Gain"; });
    const auto restores
        = std::find_if(parameters.begin(), parameters.end(), [](const auto& info) { return info.name == u"State Restores"; });
    const auto reads = std::find_if(parameters.begin(), parameters.end(), [](const auto& info) { return info.name == u"State Reads"; });
    ASSERT_NE(gain, parameters.end());
    ASSERT_NE(restores, parameters.end());
    ASSERT_NE(reads, parameters.end());
    auto initial = m_effect->MakeSettings();
    ASSERT_TRUE(editor->saveSettings(initial));
    CommandParameters saved;
    ASSERT_TRUE(m_effect->SaveSettings(initial, saved));
    const auto access = state->GetAccess();
    const auto changeGain = [&](const wxString& value) {
        saved.Write("Parameters", value);
        access->ModifySettings([&](EffectSettings& settings) {
            EXPECT_TRUE(m_effect->LoadSettings(saved, settings));
            return nullptr;
        });
        access->Flush();
    };
    const auto gainId = wxString::FromUTF8(gain->id.toStdString());
    changeGain(gainId + "=0.25;");
    tracktion::Edit edit(m_runtime.engine(), tracktion::Edit::forEditing);
    const auto plugin = makePlaybackPlugin(edit, state, list);
    ASSERT_TRUE(plugin);
    plugin->baseClassInitialise({ tracktion::TimePosition(), 48000.0, 128 });
    DEFER { plugin->baseClassDeinitialise();
    };
    ASSERT_NO_FATAL_FAILURE(process(*plugin, 0.1f));
    pump();
    const double restoreCount = editor->parameterValue(restores->id);
    const double readCount = editor->parameterValue(reads->id);
    changeGain(gainId + "=0.75;");
    pump();
    ASSERT_NO_FATAL_FAILURE(process(*plugin, 0.3f));
    EXPECT_DOUBLE_EQ(editor->parameterValue(restores->id), restoreCount);
    EXPECT_DOUBLE_EQ(editor->parameterValue(reads->id), readCount);
    changeGain("");
    pump();
    ASSERT_NO_FATAL_FAILURE(process(*plugin, 0.2f));
    EXPECT_GT(editor->parameterValue(restores->id), restoreCount);
}

TEST_F(HostedEffectTests, EditorOpenedAfterPlaybackUsesTheActiveClockAndSurvivesStop)
{
    useTracktionEditors();
    RealtimeEffectList list;
    const auto state = std::make_shared<RealtimeEffectState>(wxT("late-shared-vst3"));
    ASSERT_TRUE(list.AddState(state));
    tracktion::Edit edit(m_runtime.engine(), tracktion::Edit::forEditing);
    edit.tempoSequence.getTempo(0)->setBpm(137.0);
    edit.tempoSequence.getTimeSig(0)->setStringTimeSig("7/8");
    edit.tempoSequence.updateTempoData();
    auto plugin = makePlaybackPlugin(edit, state, list);
    ASSERT_TRUE(plugin);
    plugin->baseClassInitialise({ tracktion::TimePosition(), 48000.0, 128 });
    ASSERT_NO_FATAL_FAILURE(process(*plugin, 0.2f));
    const auto editor = std::dynamic_pointer_cast<TracktionHostedEffectInstance>(state->GetInstance());
    ASSERT_TRUE(editor);
    auto* processor = editor->tracktionPlugin().getAudioPluginInstance();
    EXPECT_EQ(processor, plugin->getAudioPluginInstance());
    ASSERT_TRUE(processor->getPlayHead());
    const auto position = processor->getPlayHead()->getPosition();
    ASSERT_TRUE(position);
    EXPECT_DOUBLE_EQ(*position->getBpm(), 137.0);
    EXPECT_EQ(position->getTimeSignature()->numerator, 7);
    EXPECT_EQ(position->getTimeSignature()->denominator, 8);
    EXPECT_NEAR(*position->getTimeInSeconds(), 5.0 + 3 * 128.0 / 48000.0, 1e-6);
    plugin->baseClassDeinitialise();
    plugin = nullptr;
    EXPECT_TRUE(editor->vstComponent());
    EXPECT_FALSE(processor->getPlayHead());
    auto settings = m_effect->MakeSettings();
    EXPECT_TRUE(editor->saveSettings(settings));
    plugin = makePlaybackPlugin(edit, state, list);
    ASSERT_TRUE(plugin);
    EXPECT_EQ(plugin->getAudioPluginInstance(), processor);
    plugin->baseClassInitialise({ tracktion::TimePosition(), 48000.0, 128 });
    DEFER { plugin->baseClassDeinitialise();
    };
    ASSERT_NO_FATAL_FAILURE(process(*plugin, 0.2f));
}

TEST_F(HostedEffectTests, ExportDoesNotShareOrReconfigureThePlaybackProcessor)
{
    useTracktionEditors();
    RealtimeEffectList list;
    const auto state = std::make_shared<RealtimeEffectState>(wxT("export-shared-vst3"));
    ASSERT_TRUE(list.AddState(state));
    tracktion::Edit liveEdit(m_runtime.engine(), tracktion::Edit::forEditing);
    const auto live = makePlaybackPlugin(liveEdit, state, list);
    ASSERT_TRUE(live);
    live->baseClassInitialise({ tracktion::TimePosition(), 48000.0, 128 });
    DEFER { live->baseClassDeinitialise();
    };
    ASSERT_NO_FATAL_FAILURE(process(*live, 0.2f));
    tracktion::Edit exportEdit(m_runtime.engine(), tracktion::Edit::forRendering);
    const auto rendered = makePlaybackPlugin(exportEdit, state->Clone(), list, 2, false);
    ASSERT_TRUE(rendered);
    EXPECT_NE(rendered->getAudioPluginInstance(), live->getAudioPluginInstance());
    rendered->baseClassInitialise({ tracktion::TimePosition(), 44100.0, 128 });
    DEFER { rendered->baseClassDeinitialise();
    };
    EXPECT_DOUBLE_EQ(rendered->getAudioPluginInstance()->getSampleRate(), 44100.0);
    EXPECT_DOUBLE_EQ(live->getAudioPluginInstance()->getSampleRate(), 48000.0);
    ASSERT_NO_FATAL_FAILURE(process(*live, 0.2f));
}
TEST_F(HostedEffectTests, PlaybackReleasesTheEditorAfterItsWrapperAndEditAreDeleted)
{
    useTracktionEditors();
    RealtimeEffectList list;
    const auto state = std::make_shared<RealtimeEffectState>(wxT("owned-shared-vst3"));
    ASSERT_TRUE(list.AddState(state));
    std::weak_ptr<TracktionHostedEffectInstance> editor;
    {
        tracktion::Edit edit(m_runtime.engine(), tracktion::Edit::forEditing);
        const auto plugin = makePlaybackPlugin(edit, state, list);
        ASSERT_TRUE(plugin);
        editor = std::dynamic_pointer_cast<TracktionHostedEffectInstance>(state->GetInstance());
        EXPECT_FALSE(editor.expired());
        plugin->baseClassInitialise({ tracktion::TimePosition(), 48000.0, 128 });
        DEFER { plugin->baseClassDeinitialise();
        };
        ASSERT_NO_FATAL_FAILURE(process(*plugin, 0.2f));
    }
    EXPECT_TRUE(editor.expired());
    EXPECT_FALSE(state->PeekInstance());
}
}
#endif
