/*
* Audacity: A Digital Audio Editor
*/
#include <tracktion_engine/tracktion_engine.h>

#include <gtest/gtest.h>
#include <QTemporaryDir>
#include <QCoreApplication>
#include <QEventLoop>
#include <thread>
#include <cmath>
#include <numbers>

#if AU_TRACKTION_TEST_AUDIO_UNIT
#include <AudioToolbox/AudioUnitParameters.h>
#include <AudioToolbox/AudioUnitUtilities.h>
#include <CoreFoundation/CoreFoundation.h>
#include "au3-audio-unit/AudioUnitEffectBase.h"
#include "au3-audio-unit/AudioUnitInstance.h"
#include "../internal/tracktioneffectinstance.h"
#include "../internal/tracktionruntime.h"
#include "../internal/tracktionpluginloader.h"
#include "../internal/audacityeffectregistry.h"
#include "au3-realtime-effects/RealtimeEffectState.h"
#include "au3-realtime-effects/RealtimeEffectList.h"
#include "global/defer.h"
#include "au3-module-manager/ConfigInterface.h"
#include "au3-xml/XMLWriter.h"
#include "au3-xml/XMLFileReader.h"

namespace au::tracktionaudio {
namespace {
class AudioUnitHostTests : public ::testing::Test
{
protected:
    void SetUp() override
    {
        ASSERT_TRUE(m_directory.isValid());
        m_runtime.init(m_directory.filePath("cache").toStdString(), m_directory.filePath("settings").toStdString());
    }

    std::unique_ptr<AudioUnitEffectBase> makeEffect(OSType subtype, const wxString& code, const wxString& name)
    {
        AudioComponentDescription description {};
        description.componentType = kAudioUnitType_Effect;
        description.componentSubType = subtype;
        description.componentManufacturer = kAudioUnitManufacturer_Apple;
        const auto component = AudioComponentFindNext(nullptr, &description);
        if (!component) {
            return {};
        }
        auto effect = std::make_unique<AudioUnitEffectBase>("appl/aufx/" + code + "/Apple: " + name, "Apple: " + name, component);
        return effect->InitializePlugin() ? std::move(effect) : nullptr;
    }

    std::shared_ptr<TracktionHostedEffectInstance> makeEditor(const AudioUnitEffectBase& effect)
    {
        return std::dynamic_pointer_cast<TracktionHostedEffectInstance>(createTracktionEffectInstance(m_runtime, effect));
    }

    static void pump()
    {
        const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(60);
        while (std::chrono::steady_clock::now() < end) {
            QCoreApplication::processEvents(QEventLoop::AllEvents);
            CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.0, true);
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }

    static AudioUnitParameterValue read(AudioUnit unit, AudioUnitParameterID id)
    {
        AudioUnitParameterValue value = 0.0f;
        EXPECT_EQ(AudioUnitGetParameter(unit, id, kAudioUnitScope_Global, 0, &value), noErr);
        return value;
    }

    static void setPresetName(AudioUnit unit, CFStringRef name)
    {
        const AUPreset preset { -1, name };
        ASSERT_EQ(AudioUnitSetProperty(unit, kAudioUnitProperty_PresentPreset, kAudioUnitScope_Global, 0, &preset, sizeof(preset)), noErr);
    }

    static std::string presetName(AudioUnit unit)
    {
        AUPreset preset {};
        UInt32 size = sizeof(preset);
        EXPECT_EQ(AudioUnitGetProperty(unit, kAudioUnitProperty_PresentPreset, kAudioUnitScope_Global, 0, &preset, &size), noErr);
        const auto name = preset.presetName ? juce::String::fromCFString(preset.presetName).toStdString() : std::string();
        if (preset.presetName) {
            CFRelease(preset.presetName);
        }
        return name;
    }

    QTemporaryDir m_directory;
    TracktionRuntime m_runtime;
};
}

TEST_F(AudioUnitHostTests, InstalledAudioUnitCanBeLoadedAndDisposed)
{
    const auto identifier = qEnvironmentVariable("AU_TRACKTION_TEST_AUDIO_UNIT_IDENTIFIER");
    if (identifier.isEmpty()) {
        GTEST_SKIP() << "Set AU_TRACKTION_TEST_AUDIO_UNIT_IDENTIFIER to test an installed Audio Unit.";
    }
    juce::PluginDescription description;
    description.pluginFormatName = "AudioUnit";
    description.fileOrIdentifier = identifier.toStdString();
    for (int iteration = 0; iteration < 3; ++iteration) {
        SCOPED_TRACE(iteration);
        juce::String error;
        auto instance = loadTracktionPlugin(m_runtime.engine().getPluginManager().pluginFormatManager, description, error);
        ASSERT_TRUE(instance) << error.toStdString();
        juce::MemoryBlock state;
        instance->getStateInformation(state);
        auto* client = instance->getAudioUnitClient();
        ASSERT_TRUE(client);
        ASSERT_TRUE(client->prepareForEditor());
        client->editorClosed();
        instance->setRateAndBufferSizeDetails(44100.0, 512);
        instance->prepareToPlay(44100.0, 512);
        juce::AudioBuffer<float> buffer(std::max(instance->getTotalNumInputChannels(), instance->getTotalNumOutputChannels()), 512);
        buffer.clear();
        juce::MidiBuffer midi;
        instance->processBlock(buffer, midi);
        instance->releaseResources();
        instance.reset();
        juce::AudioUnitPluginFormatHeadless::waitForPendingDisposals();
        pump();
    }
}

TEST_F(AudioUnitHostTests, ParametersUseTheTracktionUnitAndKeepPhysicalUnits)
{
    const auto effect = makeEffect(kAudioUnitSubType_LowPassFilter, "lpas", "AULowpass");
    ASSERT_TRUE(effect);
    const auto editor = makeEditor(*effect);
    ASSERT_TRUE(editor);
    ASSERT_TRUE(editor->Init()) << editor->GetLastError();
    ASSERT_TRUE(editor->audioUnit());
    EXPECT_NE(editor->audioUnit(), effect->GetAudioUnit());
    EXPECT_FALSE(editor->vstComponent());
    EXPECT_FALSE(std::dynamic_pointer_cast<AudioUnitInstance>(editor));
    const auto id = muse::String::number(static_cast<size_t>(kLowPassParam_CutoffFrequency));
    const auto info = editor->parameter(id);
    ASSERT_TRUE(info.isValid());
    EXPECT_EQ(info.units, u"Hz");
    EXPECT_FALSE(info.isReadOnly);
    EXPECT_GT(info.maxValue, 1000.0);
    ASSERT_TRUE(editor->setParameterValue(id, 1000.0));
    EXPECT_NEAR(read(editor->audioUnit(), kLowPassParam_CutoffFrequency), 1000.0f, 0.01f);
    EXPECT_NEAR(editor->parameterValue(id), 1000.0, 0.01);
    EXPECT_TRUE(editor->takeSettingsChanges());
    auto settings = effect->MakeSettings();
    ASSERT_TRUE(editor->saveSettings(settings));
    const auto& captured = AudioUnitWrapper::GetSettings(settings);
    ASSERT_TRUE(captured.values.at(kLowPassParam_CutoffFrequency));
    EXPECT_FLOAT_EQ(captured.values.at(kLowPassParam_CutoffFrequency)->second, 1000.0f);
    EXPECT_EQ(captured.values.at(kLowPassParam_CutoffFrequency)->first,
              AudioUnitWrapper::GetSettings(effect->MakeSettings()).values.at(kLowPassParam_CutoffFrequency)->first);
    const auto second = makeEditor(*effect);
    ASSERT_TRUE(second);
    ASSERT_TRUE(second->loadSettings(settings, true));
    EXPECT_NEAR(read(second->audioUnit(), kLowPassParam_CutoffFrequency), 1000.0f, 0.01f);
    EXPECT_NEAR(second->parameterValue(id), 1000.0, 0.01);
}

TEST_F(AudioUnitHostTests, AudioUnitNotificationsUpdateTheTracktionParameterCache)
{
    const auto effect = makeEffect(kAudioUnitSubType_LowPassFilter, "lpas", "AULowpass");
    ASSERT_TRUE(effect);
    const auto editor = makeEditor(*effect);
    ASSERT_TRUE(editor);
    auto settings = effect->MakeSettings();
    ASSERT_TRUE(editor->loadSettings(settings, true));
    pump();
    editor->takeSettingsChanges();
    AudioUnitParameter parameter { editor->audioUnit(), kLowPassParam_CutoffFrequency, kAudioUnitScope_Global, 0 };
    ASSERT_EQ(AUParameterSet(nullptr, nullptr, &parameter, 800.0f, 0), noErr);
    pump();
    EXPECT_TRUE(editor->takeSettingsChanges());
    const auto id = muse::String::number(static_cast<size_t>(kLowPassParam_CutoffFrequency));
    EXPECT_NEAR(editor->parameterValue(id), 800.0, 0.01);
    ASSERT_TRUE(editor->savePlaybackSettings(settings));
    EXPECT_NEAR(AudioUnitWrapper::GetSettings(settings).values.at(kLowPassParam_CutoffFrequency)->second, 800.0f, 0.01f);
}

TEST_F(AudioUnitHostTests, ParameterUpdatesKeepTheCurrentFactoryPresetAndOtherValues)
{
    const auto effect = makeEffect(kAudioUnitSubType_MatrixReverb, "mrev", "AUMatrixReverb");
    ASSERT_TRUE(effect);
    const auto presets = effect->GetFactoryPresets();
    ASSERT_FALSE(presets.empty());
    auto settings = effect->MakeSettings();
    ASSERT_TRUE(effect->LoadFactoryPreset(0, settings));
    const auto editor = makeEditor(*effect);
    ASSERT_TRUE(editor);
    ASSERT_TRUE(editor->loadSettings(settings, true));
    const auto reference = makeEditor(*effect);
    ASSERT_TRUE(reference);
    ASSERT_TRUE(reference->loadSettings(settings, true));
    const auto unchanged = read(editor->audioUnit(), 1);
    ASSERT_TRUE(reference->setParameterValue(u"0", 3.0));
    AUPreset before {};
    UInt32 size = sizeof(before);
    ASSERT_EQ(AudioUnitGetProperty(reference->audioUnit(), kAudioUnitProperty_PresentPreset, kAudioUnitScope_Global, 0, &before, &size),
              noErr);
    auto& values = AudioUnitWrapper::GetSettings(settings).values;
    ASSERT_TRUE(values.at(0));
    values.at(0)->second = 3.0f;
    ASSERT_TRUE(editor->loadParameterSettings(settings));
    EXPECT_NEAR(read(editor->audioUnit(), 0), 3.0f, 1e-5f);
    EXPECT_FLOAT_EQ(read(editor->audioUnit(), 1), unchanged);
    AUPreset after {};
    size = sizeof(after);
    ASSERT_EQ(AudioUnitGetProperty(editor->audioUnit(), kAudioUnitProperty_PresentPreset, kAudioUnitScope_Global, 0, &after, &size), noErr);
    EXPECT_EQ(after.presetNumber, before.presetNumber);
    ASSERT_TRUE(editor->saveSettings(settings));
    EXPECT_EQ(AudioUnitWrapper::GetSettings(settings).mPresetNumber,
              before.presetNumber >= 0 ? std::make_optional(before.presetNumber) : std::nullopt);
}

TEST_F(AudioUnitHostTests, ModeChangesApplyParametersAfterTheyBecomeAvailable)
{
    const auto effect = makeEffect(kAudioUnitSubType_GraphicEQ, "greq", "AUGraphicEQ");
    ASSERT_TRUE(effect);
    const auto editor = makeEditor(*effect);
    ASSERT_TRUE(editor);
    ASSERT_EQ(AudioUnitSetParameter(editor->audioUnit(), kGraphicEQParam_NumberOfBands, kAudioUnitScope_Global, 0, 10.0f, 0), noErr);
    auto settings = effect->MakeSettings();
    CommandParameters parameters;
    parameters.Write("<AAA Band,14>", 3.0);
    parameters.Write("<ZZZ Bands,2710>", 31.0);
    ASSERT_TRUE(effect->LoadSettings(parameters, settings));
    ASSERT_TRUE(editor->loadSettings(settings, false));
    EXPECT_FLOAT_EQ(read(editor->audioUnit(), kGraphicEQParam_NumberOfBands), 31.0f);
    EXPECT_NEAR(read(editor->audioUnit(), 20), 3.0f, 1e-5f);
    ASSERT_TRUE(editor->saveSettings(settings));
    const auto& values = AudioUnitWrapper::GetSettings(settings).values;
    ASSERT_TRUE(values.at(20));
    EXPECT_NEAR(values.at(20)->second, 3.0f, 1e-5f);
}

TEST_F(AudioUnitHostTests, UnchangedNotificationsDoNotChangeTheDocument)
{
    const auto effect = makeEffect(kAudioUnitSubType_LowPassFilter, "lpas", "AULowpass");
    ASSERT_TRUE(effect);
    const auto editor = makeEditor(*effect);
    ASSERT_TRUE(editor);
    auto settings = effect->MakeSettings();
    ASSERT_TRUE(editor->loadSettings(settings, true));
    pump();
    editor->takeSettingsChanges();
    const auto current = read(editor->audioUnit(), kLowPassParam_CutoffFrequency);
    AudioUnitParameter parameter { editor->audioUnit(), kLowPassParam_CutoffFrequency, kAudioUnitScope_Global, 0 };
    ASSERT_EQ(AUParameterSet(nullptr, nullptr, &parameter, current, 0), noErr);
    pump();
    EXPECT_FALSE(editor->takeSettingsChanges());
}

TEST_F(AudioUnitHostTests, PendingNotificationsDoNotAccessADeletedProcessor)
{
    juce::PluginDescription description;
    description.pluginFormatName = "AudioUnit";
    description.fileOrIdentifier = "AudioUnit:aufx,lpas,appl";
    for (int iteration = 0; iteration < 16; ++iteration) {
        SCOPED_TRACE(iteration);
        juce::String error;
        auto processor = loadTracktionPlugin(m_runtime.engine().getPluginManager().pluginFormatManager, description, error);
        ASSERT_TRUE(processor) << error.toStdString();
        const auto unit = processor->getAudioUnitClient()->getAudioUnitHandle();
        AudioUnitParameter parameter { unit, kLowPassParam_CutoffFrequency, kAudioUnitScope_Global, 0 };
        ASSERT_EQ(AUParameterSet(nullptr, nullptr, &parameter, 800.0f, 0), noErr);
        processor.reset();
        EXPECT_FLOAT_EQ(read(unit, kLowPassParam_CutoffFrequency), 800.0f);
        juce::AudioUnitPluginFormatHeadless::waitForPendingDisposals();
        pump();
    }
}

TEST_F(AudioUnitHostTests, EngineShutdownProcessesPendingAudioUnitDisposal)
{
    juce::PluginDescription description;
    description.pluginFormatName = "AudioUnit";
    description.fileOrIdentifier = "AudioUnit:aufx,lpas,appl";
    juce::String error;
    auto processor = loadTracktionPlugin(m_runtime.engine().getPluginManager().pluginFormatManager, description, error);
    ASSERT_TRUE(processor) << error.toStdString();
    bool mainCallback = false;
    bool ready = false;
    juce::MessageManager::callAsync([&] { mainCallback = true; });
    processor.reset();
    ASSERT_FALSE(mainCallback);
    m_runtime.engine().prepareForShutdown([&] { ready = true; });
    EXPECT_TRUE(ready);
    EXPECT_TRUE(mainCallback);
}

TEST_F(AudioUnitHostTests, EditorFactorySelectsTracktionForDestructiveAndRealtimeEffects)
{
    const auto effect = makeEffect(kAudioUnitSubType_LowPassFilter, "lpas", "AULowpass");
    ASSERT_TRUE(effect);
    const EffectInstanceFactory::EditorInstanceFactory::Scope factory([&](const EffectInstanceFactory& definition) {
        return createTracktionEffectInstance(m_runtime, definition);
    });
    const auto editor = std::dynamic_pointer_cast<TracktionHostedEffectInstance>(effect->MakeEditorInstance());
    ASSERT_TRUE(editor);
    ASSERT_TRUE(editor->Init()) << editor->GetLastError();
    ASSERT_TRUE(editor->audioUnit());
    EXPECT_FALSE(std::dynamic_pointer_cast<AudioUnitInstance>(editor));
    const RealtimeEffectState::EffectFactory::Scope effectFactory([&](const PluginID&) -> const EffectInstanceFactory* {
        return effect.get();
    });
    RealtimeEffectState state(wxT("native-au-editor"));
    const auto realtimeEditor = std::dynamic_pointer_cast<TracktionHostedEffectInstance>(state.GetInstance());
    ASSERT_TRUE(realtimeEditor);
    ASSERT_TRUE(realtimeEditor->audioUnit());
    EXPECT_NE(realtimeEditor->audioUnit(), editor->audioUnit());
}

TEST_F(AudioUnitHostTests, LivePlaybackUsesTheEditorAudioUnitAndKeepsControlChanges)
{
    const auto effect = makeEffect(kAudioUnitSubType_LowPassFilter, "lpas", "AULowpass");
    ASSERT_TRUE(effect);
    const EffectInstanceFactory::EditorInstanceFactory::Scope editorFactory([&](const EffectInstanceFactory& definition) {
        return createTracktionEffectInstance(m_runtime, definition);
    });
    const RealtimeEffectState::EffectFactory::Scope effectFactory([&](const PluginID&) -> const EffectInstanceFactory* {
        return effect.get();
    });
    RealtimeEffectList list;
    const auto state = std::make_shared<RealtimeEffectState>(wxT("shared-au"));
    ASSERT_TRUE(list.AddState(state));
    const auto editor = std::dynamic_pointer_cast<TracktionHostedEffectInstance>(state->GetInstance());
    ASSERT_TRUE(editor);
    const auto unit = editor->audioUnit();
    ASSERT_TRUE(unit);
    ASSERT_TRUE(editor->openAudioUnitEditor());
    DEFER { editor->closeAudioUnitEditor();
    };
    const auto id = m_runtime.effects()->add(state, list, nullptr, 2, true);
    DEFER { m_runtime.effects()->remove(id);
    };
    tracktion::Edit edit(m_runtime.engine(), tracktion::Edit::forEditing);
    juce::ValueTree value(tracktion::IDs::PLUGIN);
    value.setProperty(tracktion::IDs::type, AUDACITY_EFFECT_PLUGIN_TYPE, nullptr);
    value.setProperty(AUDACITY_EFFECT_STATE_PROPERTY, id, nullptr);
    const auto owned = edit.getPluginCache().createNewPlugin(value);
    const auto plugin = dynamic_cast<tracktion::ExternalPlugin*>(owned.get());
    ASSERT_TRUE(plugin);
    EXPECT_EQ(plugin->getAudioPluginInstance(), editor->tracktionPlugin().getAudioPluginInstance());
    plugin->baseClassInitialise({ tracktion::TimePosition(), 48000.0, 128 });
    DEFER { plugin->baseClassDeinitialise();
    };
    int64_t position = 0;
    const auto renderRms = [&]() {
        juce::AudioBuffer<float> audio(2, 128);
        double energy = 0.0;
        for (int block = 0; block < 32; ++block) {
            for (int channel = 0; channel < 2; ++channel) {
                for (int frame = 0; frame < 128; ++frame) {
                    audio.setSample(channel, frame, 0.4f * std::sin(2.0 * std::numbers::pi * 4000.0 * (position + frame) / 48000.0));
                }
            }
            const auto time = tracktion::TimePosition::fromSamples(position, 48000.0);
            tracktion::PluginRenderContext context(&audio, 0, 128, nullptr, 0.0,
                                                   { time, time + tracktion::TimeDuration::fromSamples(128, 48000.0) },
                                                   true, false, false, false);
            context.referenceSampleRange = { position, position + 128 };
            context.referenceSampleRate = 48000.0;
            plugin->applyToBuffer(context);
            if (block >= 24) {
                for (int frame = 0; frame < 128; ++frame) {
                    energy += audio.getSample(0, frame) * audio.getSample(0, frame);
                }
            }
            position += 128;
        }
        return std::sqrt(energy / 1024.0);
    };
    AudioUnitParameter parameter { unit, kLowPassParam_CutoffFrequency, kAudioUnitScope_Global, 0 };
    ASSERT_EQ(AUParameterSet(nullptr, nullptr, &parameter, 800.0f, 0), noErr);
    pump();
    const auto low = renderRms();
    ASSERT_EQ(AUParameterSet(nullptr, nullptr, &parameter, 8000.0f, 0), noErr);
    pump();
    EXPECT_GT(renderRms(), low * 4.0);
    editor->closeAudioUnitEditor();
    EXPECT_GT(renderRms(), low * 4.0);
    ASSERT_TRUE(editor->openAudioUnitEditor());
    EXPECT_EQ(editor->audioUnit(), unit);
    const auto access = state->GetAccess();
    EXPECT_NEAR(AudioUnitWrapper::GetSettings(access->Get()).values.at(kLowPassParam_CutoffFrequency)->second, 8000.0f, 0.01f);
    access->ModifySettings([](EffectSettings& settings) {
        AudioUnitWrapper::GetSettings(settings).values.at(kLowPassParam_CutoffFrequency)->second = 400.0f;
        return nullptr;
    });
    access->Flush();
    pump();
    EXPECT_NEAR(read(unit, kLowPassParam_CutoffFrequency), 400.0f, 0.01f);
    EXPECT_LT(renderRms(), low);
}

TEST_F(AudioUnitHostTests, LivePresetNotificationsUpdateSettingsWithoutRestoringThePreset)
{
    const auto effect = makeEffect(kAudioUnitSubType_MatrixReverb, "mrev", "AUMatrixReverb");
    ASSERT_TRUE(effect);
    const EffectInstanceFactory::EditorInstanceFactory::Scope editorFactory([&](const EffectInstanceFactory& definition) {
        return createTracktionEffectInstance(m_runtime, definition);
    });
    const RealtimeEffectState::EffectFactory::Scope effectFactory([&](const PluginID&) -> const EffectInstanceFactory* {
        return effect.get();
    });
    RealtimeEffectList list;
    const auto state = std::make_shared<RealtimeEffectState>(wxT("shared-au-preset"));
    ASSERT_TRUE(list.AddState(state));
    const auto editor = std::dynamic_pointer_cast<TracktionHostedEffectInstance>(state->GetInstance());
    ASSERT_TRUE(editor);
    const auto id = m_runtime.effects()->add(state, list, nullptr, 2, true);
    DEFER { m_runtime.effects()->remove(id);
    };
    tracktion::Edit edit(m_runtime.engine(), tracktion::Edit::forEditing);
    juce::ValueTree value(tracktion::IDs::PLUGIN);
    value.setProperty(tracktion::IDs::type, AUDACITY_EFFECT_PLUGIN_TYPE, nullptr);
    value.setProperty(AUDACITY_EFFECT_STATE_PROPERTY, id, nullptr);
    const auto owned = edit.getPluginCache().createNewPlugin(value);
    const auto plugin = dynamic_cast<tracktion::ExternalPlugin*>(owned.get());
    ASSERT_TRUE(plugin);
    plugin->baseClassInitialise({ tracktion::TimePosition(), 48000.0, 128 });
    DEFER { plugin->baseClassDeinitialise();
    };
    pump();
    EXPECT_FALSE(editor->takeSettingsChanges());
    const auto unit = editor->audioUnit();
    CFArrayRef presets = nullptr;
    UInt32 size = sizeof(presets);
    ASSERT_EQ(AudioUnitGetProperty(unit, kAudioUnitProperty_FactoryPresets, kAudioUnitScope_Global, 0, &presets, &size), noErr);
    ASSERT_TRUE(presets);
    DEFER { CFRelease(presets);
    };
    ASSERT_GE(CFArrayGetCount(presets), 2);
    std::atomic<int> changes { 0 };
    const auto listener = +[] (void* context, AudioUnit, AudioUnitPropertyID, AudioUnitScope, AudioUnitElement) {
        ++*static_cast<std::atomic<int>*>(context);
    };
    ASSERT_EQ(AudioUnitAddPropertyListener(unit, kAudioUnitProperty_PresentPreset, listener, &changes), noErr);
    DEFER { AudioUnitRemovePropertyListenerWithUserData(unit, kAudioUnitProperty_PresentPreset, listener, &changes);
    };
    const auto preset = static_cast<const AUPreset*>(CFArrayGetValueAtIndex(presets, 1));
    ASSERT_EQ(AudioUnitSetProperty(unit, kAudioUnitProperty_PresentPreset, kAudioUnitScope_Global, 0, preset, sizeof(*preset)), noErr);
    const auto notifications = changes.load();
    pump();
    EXPECT_TRUE(editor->takeSettingsChanges());
    EXPECT_EQ(AudioUnitWrapper::GetSettings(state->GetAccess()->Get()).mPresetNumber, preset->presetNumber);
    EXPECT_EQ(changes.load(), notifications);
    pump();
    EXPECT_FALSE(editor->takeSettingsChanges());
}

TEST_F(AudioUnitHostTests, RefreshedParametersKeepCurrentValuesWithoutNotifications)
{
    const auto effect = makeEffect(kAudioUnitSubType_MatrixReverb, "mrev", "AUMatrixReverb");
    ASSERT_TRUE(effect);
    const auto editor = makeEditor(*effect);
    ASSERT_TRUE(editor);
    ASSERT_TRUE(editor->loadSettings(effect->MakeSettings(), true));
    ASSERT_TRUE(editor->setParameterValue(u"0", 3.0));
    pump();
    editor->takeSettingsChanges();
    const auto unit = editor->audioUnit();
    editor->tracktionPlugin().getAudioPluginInstance()->refreshParameterList();
    EXPECT_NEAR(editor->parameterValue(u"0"), 3.0, 1e-5);
    EXPECT_EQ(editor->audioUnit(), unit);
    EXPECT_FALSE(editor->takeSettingsChanges());
    pump();
    EXPECT_NEAR(editor->parameterValue(u"0"), 3.0, 1e-5);
    EXPECT_FALSE(editor->takeSettingsChanges());
}

TEST_F(AudioUnitHostTests, EditorInitializationFollowsPlaybackAndEditorLifetime)
{
    const auto effect = makeEffect(kAudioUnitSubType_LowPassFilter, "lpas", "AULowpass");
    ASSERT_TRUE(effect);
    const EffectInstanceFactory::EditorInstanceFactory::Scope editorFactory([&](const EffectInstanceFactory& definition) {
        return createTracktionEffectInstance(m_runtime, definition);
    });
    const RealtimeEffectState::EffectFactory::Scope effectFactory([&](const PluginID&) -> const EffectInstanceFactory* {
        return effect.get();
    });
    RealtimeEffectList list;
    const auto state = std::make_shared<RealtimeEffectState>(wxT("au-editor-initialization"));
    ASSERT_TRUE(list.AddState(state));
    const auto editor = std::dynamic_pointer_cast<TracktionHostedEffectInstance>(state->GetInstance());
    ASSERT_TRUE(editor);
    ASSERT_TRUE(editor->loadSettings(state->GetAccess()->Get(), true));
    DEFER { editor->closeAudioUnitEditor();
    };
    const auto unit = editor->audioUnit();
    ASSERT_TRUE(unit);
    const auto expectFormatWritable = [unit](bool expected) {
        UInt32 size = 0;
        Boolean writable = false;
        ASSERT_EQ(AudioUnitGetPropertyInfo(unit, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Input, 0, &size, &writable), noErr);
        EXPECT_EQ(size, sizeof(AudioStreamBasicDescription));
        EXPECT_EQ(static_cast<bool>(writable), expected);
    };
    expectFormatWritable(true);
    ASSERT_TRUE(editor->openAudioUnitEditor());
    expectFormatWritable(false);
    editor->closeAudioUnitEditor();
    expectFormatWritable(true);
    ASSERT_TRUE(editor->openAudioUnitEditor());
    pump();
    EXPECT_FALSE(editor->takeSettingsChanges());
    const auto id = m_runtime.effects()->add(state, list, nullptr, 1, true);
    DEFER { m_runtime.effects()->remove(id);
    };
    tracktion::Edit edit(m_runtime.engine(), tracktion::Edit::forEditing);
    juce::ValueTree value(tracktion::IDs::PLUGIN);
    value.setProperty(tracktion::IDs::type, AUDACITY_EFFECT_PLUGIN_TYPE, nullptr);
    value.setProperty(AUDACITY_EFFECT_STATE_PROPERTY, id, nullptr);
    const auto owned = edit.getPluginCache().createNewPlugin(value);
    const auto plugin = dynamic_cast<tracktion::ExternalPlugin*>(owned.get());
    ASSERT_TRUE(plugin);
    EXPECT_EQ(plugin->getAudioPluginInstance(), editor->tracktionPlugin().getAudioPluginInstance());
    EXPECT_EQ(plugin->getAudioPluginInstance()->getMainBusNumInputChannels(), 1);
    expectFormatWritable(false);
    plugin->baseClassInitialise({ tracktion::TimePosition(), 48000.0, 128 });
    bool processing = true;
    DEFER { if (processing) {
                plugin->baseClassDeinitialise();
            }
    };
    EXPECT_DOUBLE_EQ(plugin->getAudioPluginInstance()->getSampleRate(), 48000.0);
    expectFormatWritable(false);
    editor->closeAudioUnitEditor();
    expectFormatWritable(false);
    ASSERT_TRUE(editor->openAudioUnitEditor());
    plugin->baseClassDeinitialise();
    processing = false;
    expectFormatWritable(false);
    pump();
    EXPECT_FALSE(editor->takeSettingsChanges());
    plugin->getAudioPluginInstance()->releaseResources();
    expectFormatWritable(false);
    editor->closeAudioUnitEditor();
    expectFormatWritable(true);
    ASSERT_TRUE(editor->openAudioUnitEditor());
    expectFormatWritable(false);
    editor->closeAudioUnitEditor();
    expectFormatWritable(true);
}

TEST_F(AudioUnitHostTests, CompleteStateKeepsPrivateDataThroughSerializationAndSettingsCopies)
{
    const auto effect = makeEffect(kAudioUnitSubType_LowPassFilter, "lpas", "AULowpass");
    ASSERT_TRUE(effect);
    const auto source = makeEditor(*effect);
    ASSERT_TRUE(source);
    ASSERT_TRUE(source->loadSettings(effect->MakeSettings(), true));
    setPresetName(source->audioUnit(), CFSTR("State-A"));
    ASSERT_TRUE(source->setParameterValue(u"0", 1234.0));
    auto first = effect->MakeSettings();
    ASSERT_TRUE(source->saveSettings(first));
    ASSERT_TRUE(AudioUnitWrapper::GetSettings(first).mState);
    CommandParameters parameters;
    ASSERT_TRUE(effect->SaveSettings(first, parameters));
    EXPECT_TRUE(parameters.HasEntry("AudioUnitState"));
    wxString encoded;
    parameters.GetParameters(encoded);
    CommandParameters decoded;
    ASSERT_TRUE(decoded.SetParameters(encoded));
    auto restored = effect->MakeSettings();
    ASSERT_TRUE(effect->LoadSettings(decoded, restored));
    auto previous = restored;
    setPresetName(source->audioUnit(), CFSTR("State-B"));
    ASSERT_TRUE(source->setParameterValue(u"0", 4321.0));
    auto next = effect->MakeSettings();
    ASSERT_TRUE(source->saveSettings(next));
    const auto target = makeEditor(*effect);
    ASSERT_TRUE(target);
    ASSERT_TRUE(target->loadSettings(restored, true));
    EXPECT_EQ(presetName(target->audioUnit()), "State-A");
    EXPECT_NEAR(read(target->audioUnit(), kLowPassParam_CutoffFrequency), 1234.0f, 0.01f);
    ASSERT_TRUE(target->loadSettings(next, true));
    EXPECT_EQ(presetName(target->audioUnit()), "State-B");
    ASSERT_TRUE(target->loadSettings(previous, true));
    EXPECT_EQ(presetName(target->audioUnit()), "State-A");
    EXPECT_NEAR(read(target->audioUnit(), kLowPassParam_CutoffFrequency), 1234.0f, 0.01f);
    ASSERT_TRUE(target->loadSettings(next, true));
    EXPECT_NEAR(read(target->audioUnit(), kLowPassParam_CutoffFrequency), 4321.0f, 0.01f);
}

TEST_F(AudioUnitHostTests, LegacyParametersAndFactoryPresetsClearTheCompleteState)
{
    const auto effect = makeEffect(kAudioUnitSubType_MatrixReverb, "mrev", "AUMatrixReverb");
    ASSERT_TRUE(effect);
    const auto editor = makeEditor(*effect);
    ASSERT_TRUE(editor);
    auto settings = effect->MakeSettings();
    ASSERT_TRUE(editor->loadSettings(settings, true));
    setPresetName(editor->audioUnit(), CFSTR("State-A"));
    ASSERT_TRUE(editor->saveSettings(settings));
    ASSERT_TRUE(AudioUnitWrapper::GetSettings(settings).mState);
    CommandParameters legacy;
    ASSERT_TRUE(effect->SaveSettings(effect->MakeSettings(), legacy));
    EXPECT_FALSE(legacy.HasEntry("AudioUnitState"));
    ASSERT_TRUE(effect->LoadSettings(legacy, settings));
    EXPECT_FALSE(AudioUnitWrapper::GetSettings(settings).mState);
    ASSERT_TRUE(editor->saveSettings(settings));
    ASSERT_TRUE(AudioUnitWrapper::GetSettings(settings).mState);
    ASSERT_TRUE(effect->LoadFactoryPreset(0, settings));
    EXPECT_FALSE(AudioUnitWrapper::GetSettings(settings).mState);
    ASSERT_TRUE(editor->saveSettings(settings));
    ASSERT_TRUE(effect->LoadFactoryDefaults(settings));
    EXPECT_FALSE(AudioUnitWrapper::GetSettings(settings).mState);
    ASSERT_TRUE(editor->loadSettings(settings, true));
    EXPECT_NE(presetName(editor->audioUnit()), "State-A");
    CommandParameters invalid;
    invalid.Write("AudioUnitState", "?invalid?");
    EXPECT_FALSE(effect->LoadSettings(invalid, settings));
}

TEST_F(AudioUnitHostTests, InvalidAndForeignStatesKeepTheCurrentProcessorState)
{
    const auto effect = makeEffect(kAudioUnitSubType_LowPassFilter, "lpas", "AULowpass");
    ASSERT_TRUE(effect);
    const auto editor = makeEditor(*effect);
    ASSERT_TRUE(editor);
    ASSERT_TRUE(editor->loadSettings(effect->MakeSettings(), true));
    setPresetName(editor->audioUnit(), CFSTR("State-A"));
    ASSERT_TRUE(editor->setParameterValue(u"0", 1234.0));
    auto settings = effect->MakeSettings();
    ASSERT_TRUE(editor->saveSettings(settings));
    const auto unit = editor->audioUnit();
    AudioUnitWrapper::GetSettings(settings).mState = std::make_shared<const AudioUnitEffectSettings::State>(8, 'x');
    EXPECT_FALSE(editor->loadSettings(settings, true));
    const juce::File file(m_directory.filePath("unchanged.aupreset").toStdString());
    ASSERT_TRUE(file.replaceWithText("existing preset"));
    EXPECT_FALSE(effect->Export(AudioUnitWrapper::GetSettings(settings), wxString::FromUTF8(file.getFullPathName().toStdString())).empty());
    EXPECT_EQ(file.loadFileAsString(), "existing preset");
    const std::string scalar = "<plist version=\"1.0\"><integer>1</integer></plist>";
    AudioUnitWrapper::GetSettings(settings).mState = std::make_shared<const AudioUnitEffectSettings::State>(scalar.begin(), scalar.end());
    EXPECT_FALSE(editor->loadSettings(settings, true));
    EXPECT_FALSE(effect->Export(AudioUnitWrapper::GetSettings(settings), wxString::FromUTF8(file.getFullPathName().toStdString())).empty());
    EXPECT_EQ(file.loadFileAsString(), "existing preset");
    EXPECT_EQ(editor->audioUnit(), unit);
    EXPECT_EQ(presetName(unit), "State-A");
    EXPECT_NEAR(read(unit, kLowPassParam_CutoffFrequency), 1234.0f, 0.01f);
    const auto otherEffect = makeEffect(kAudioUnitSubType_MatrixReverb, "mrev", "AUMatrixReverb");
    ASSERT_TRUE(otherEffect);
    const auto other = makeEditor(*otherEffect);
    ASSERT_TRUE(other);
    auto foreign = otherEffect->MakeSettings();
    ASSERT_TRUE(other->saveSettings(foreign));
    AudioUnitWrapper::GetSettings(settings).mState = AudioUnitWrapper::GetSettings(foreign).mState;
    ASSERT_TRUE(AudioUnitWrapper::GetSettings(settings).mState);
    EXPECT_FALSE(editor->loadSettings(settings, true));
    EXPECT_EQ(editor->audioUnit(), unit);
    EXPECT_EQ(presetName(unit), "State-A");
    EXPECT_NEAR(read(unit, kLowPassParam_CutoffFrequency), 1234.0f, 0.01f);
}

TEST_F(AudioUnitHostTests, UserPresetsAndXmlPresetFilesKeepCompleteState)
{
    const auto effect = makeEffect(kAudioUnitSubType_LowPassFilter, "lpas", "AULowpass");
    ASSERT_TRUE(effect);
    const auto source = makeEditor(*effect);
    ASSERT_TRUE(source);
    ASSERT_TRUE(source->loadSettings(effect->MakeSettings(), true));
    setPresetName(source->audioUnit(), CFSTR("State-A"));
    ASSERT_TRUE(source->setParameterValue(u"0", 1234.0));
    auto settings = effect->MakeSettings();
    ASSERT_TRUE(source->saveSettings(settings));
    const auto group = UserPresetsGroup(wxT("tracktion-au-state"));
    ASSERT_TRUE(effect->SaveUserPreset(group, settings));
    auto loaded = effect->MakeSettings();
    ASSERT_TRUE(effect->LoadUserPreset(group, loaded));
    ASSERT_TRUE(AudioUnitWrapper::GetSettings(loaded).mState);
    const auto target = makeEditor(*effect);
    ASSERT_TRUE(target);
    ASSERT_TRUE(target->loadSettings(loaded, true));
    EXPECT_EQ(presetName(target->audioUnit()), "State-A");
    EXPECT_NEAR(read(target->audioUnit(), kLowPassParam_CutoffFrequency), 1234.0f, 0.01f);
    ASSERT_TRUE(RemoveConfig(*effect, PluginSettings::Private, group, wxT("Parameters")));
    auto legacy = effect->MakeSettings();
    ASSERT_TRUE(effect->LoadUserPreset(group, legacy));
    ASSERT_TRUE(AudioUnitWrapper::GetSettings(legacy).mState);
    ASSERT_TRUE(target->loadSettings(legacy, true));
    EXPECT_EQ(presetName(target->audioUnit()), "tracktion-au-state");
    EXPECT_NEAR(read(target->audioUnit(), kLowPassParam_CutoffFrequency), 1234.0f, 0.01f);
    const auto path = m_directory.filePath(QString::fromUtf8("state-\xc3\xa9.aupreset")).toStdString();
    const auto file = wxString::FromUTF8(path);
    EXPECT_TRUE(effect->Export(AudioUnitWrapper::GetSettings(settings), file).empty());
    auto imported = effect->MakeSettings();
    EXPECT_TRUE(effect->Import(AudioUnitWrapper::GetSettings(imported), file).empty());
    ASSERT_TRUE(AudioUnitWrapper::GetSettings(imported).mState);
    ASSERT_TRUE(target->loadSettings(imported, true));
    EXPECT_EQ(presetName(target->audioUnit()), "state-\xc3\xa9");
    EXPECT_NEAR(read(target->audioUnit(), kLowPassParam_CutoffFrequency), 1234.0f, 0.01f);
}

TEST_F(AudioUnitHostTests, LivePrivateStateChangesReachProjectXmlAndIndependentRenderInstances)
{
    const auto effect = makeEffect(kAudioUnitSubType_LowPassFilter, "lpas", "AULowpass");
    ASSERT_TRUE(effect);
    const EffectInstanceFactory::EditorInstanceFactory::Scope editorFactory([&](const EffectInstanceFactory& definition) {
        return createTracktionEffectInstance(m_runtime, definition);
    });
    const RealtimeEffectState::EffectFactory::Scope effectFactory([&](const PluginID&) -> const EffectInstanceFactory* {
        return effect.get();
    });
    RealtimeEffectList list;
    const auto state = std::make_shared<RealtimeEffectState>(wxT("au-state-document"));
    ASSERT_TRUE(list.AddState(state));
    const auto editor = std::dynamic_pointer_cast<TracktionHostedEffectInstance>(state->GetInstance());
    ASSERT_TRUE(editor);
    const auto id = m_runtime.effects()->add(state, list, nullptr, 2, true);
    DEFER { m_runtime.effects()->remove(id);
    };
    tracktion::Edit edit(m_runtime.engine(), tracktion::Edit::forEditing);
    juce::ValueTree value(tracktion::IDs::PLUGIN);
    value.setProperty(tracktion::IDs::type, AUDACITY_EFFECT_PLUGIN_TYPE, nullptr);
    value.setProperty(AUDACITY_EFFECT_STATE_PROPERTY, id, nullptr);
    const auto owned = edit.getPluginCache().createNewPlugin(value);
    const auto plugin = dynamic_cast<tracktion::ExternalPlugin*>(owned.get());
    ASSERT_TRUE(plugin);
    const auto unit = editor->audioUnit();
    ASSERT_TRUE(unit);
    plugin->baseClassInitialise({ tracktion::TimePosition(), 48000.0, 128 });
    DEFER { plugin->baseClassDeinitialise();
    };
    pump();
    EXPECT_FALSE(editor->takeSettingsChanges());
    setPresetName(unit, CFSTR("State-A"));
    pump();
    EXPECT_TRUE(editor->takeSettingsChanges());
    const auto access = state->GetAccess();
    const auto undo = state->Clone();
    const auto first = undo->GetAccess()->Get();
    ASSERT_TRUE(AudioUnitWrapper::GetSettings(first).mState);
    const auto other = makeEditor(*effect);
    ASSERT_TRUE(other);
    ASSERT_TRUE(other->loadSettings(effect->MakeSettings(), true));
    setPresetName(other->audioUnit(), CFSTR("State-B"));
    ASSERT_TRUE(other->setParameterValue(u"0", 4321.0));
    auto second = effect->MakeSettings();
    ASSERT_TRUE(other->saveSettings(second));
    access->ModifySettings([&](EffectSettings& settings) { settings = second; return nullptr; });
    access->Flush();
    pump();
    EXPECT_EQ(presetName(unit), "State-B");
    EXPECT_NEAR(read(unit, kLowPassParam_CutoffFrequency), 4321.0f, 0.01f);
    EXPECT_FALSE(editor->takeSettingsChanges());
    access->ModifySettings([&](EffectSettings& settings) { settings = first; return nullptr; });
    access->Flush();
    pump();
    EXPECT_EQ(presetName(unit), "State-A");
    EXPECT_FALSE(editor->takeSettingsChanges());
    access->ModifySettings([&](EffectSettings& settings) { settings = second; return nullptr; });
    access->Flush();
    pump();
    EXPECT_EQ(presetName(unit), "State-B");
    EXPECT_EQ(plugin->getAudioPluginInstance(), editor->tracktionPlugin().getAudioPluginInstance());
    XMLStringWriter xml;
    state->WriteXML(xml);
    const auto reloaded = std::make_shared<RealtimeEffectState>(wxT(""));
    XMLFileReader reader;
    ASSERT_TRUE(reader.ParseString(reloaded.get(), xml));
    const auto reopened = std::dynamic_pointer_cast<TracktionHostedEffectInstance>(reloaded->GetInstance());
    ASSERT_TRUE(reopened);
    ASSERT_TRUE(reopened->loadSettings(reloaded->GetAccess()->Get(), true));
    EXPECT_EQ(presetName(reopened->audioUnit()), "State-B");
    EXPECT_NEAR(read(reopened->audioUnit(), kLowPassParam_CutoffFrequency), 4321.0f, 0.01f);
    const auto renderId = m_runtime.effects()->add(state, list, nullptr, 2, false);
    DEFER { m_runtime.effects()->remove(renderId);
    };
    tracktion::Edit render(m_runtime.engine(), tracktion::Edit::forRendering);
    value.setProperty(AUDACITY_EFFECT_STATE_PROPERTY, renderId, nullptr);
    const auto independent = render.getPluginCache().createNewPlugin(value);
    const auto instance = dynamic_cast<tracktion::ExternalPlugin*>(independent.get());
    ASSERT_TRUE(instance);
    EXPECT_NE(instance->getAudioPluginInstance(), plugin->getAudioPluginInstance());
    const auto renderUnit = instance->getAudioPluginInstance()->getAudioUnitClient()->getAudioUnitHandle();
    EXPECT_EQ(presetName(renderUnit), "State-B");
    EXPECT_NEAR(read(renderUnit, kLowPassParam_CutoffFrequency), 4321.0f, 0.01f);
}
}
#endif
