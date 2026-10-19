/*
* Audacity: A Digital Audio Editor
*/
#include <tracktion_engine/tracktion_engine.h>

#include <gtest/gtest.h>
#include <QTemporaryDir>
#include <QEventLoop>
#include <QTimer>

#if JUCE_MAC
#include <CoreFoundation/CoreFoundation.h>
#endif

#include "../internal/tracktioneffectinstance.h"
#include "../internal/tracktionruntime.h"
#include "../internal/audacityeffectregistry.h"
#include "au3-realtime-effects/RealtimeEffectState.h"
#include "au3-realtime-effects/RealtimeEffectList.h"
#include "global/defer.h"

namespace au::tracktionaudio {
namespace {
struct LoadProbe {
    bool fail = false;
    int completions = 0;
    juce::PluginDescription description;
};

class TestProcessor final : public juce::AudioPluginInstance
{
public:
    explicit TestProcessor(juce::PluginDescription description)
        : AudioPluginInstance(BusesProperties().withInput("Input", juce::AudioChannelSet::mono(), true)
                              .withOutput("Output", juce::AudioChannelSet::mono(), true)), m_description(std::move(description)) {}

    void fillInPluginDescription(juce::PluginDescription& description) const override { description = m_description; }
    const juce::String getName() const override { return m_description.name; }
    void prepareToPlay(double, int) override {}
    void releaseResources() override {}
    void processBlock(juce::AudioBuffer<float>& audio, juce::MidiBuffer&) override { audio.applyGain(0.5f); }
    double getTailLengthSeconds() const override { return 0.0; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    bool hasEditor() const override { return false; }
    juce::AudioProcessorEditor* createEditor() override { return nullptr; }
    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram(int) override {}
    const juce::String getProgramName(int) override { return "Default"; }
    void changeProgramName(int, const juce::String&) override {}
    void getStateInformation(juce::MemoryBlock& state) override { state.setSize(1, true); }
    void setStateInformation(const void*, int) override {}
    bool isBusesLayoutSupported(const BusesLayout& layout) const override
    {
        return layout.getMainInputChannelSet() == juce::AudioChannelSet::mono()
               && layout.getMainOutputChannelSet() == juce::AudioChannelSet::mono();
    }

private:
    juce::PluginDescription m_description;
};

class TestFormat final : public juce::AudioPluginFormat
{
public:
    explicit TestFormat(std::shared_ptr<LoadProbe> probe)
        : m_probe(std::move(probe)) {}
    juce::String getName() const override { return "VST3"; }
    void findAllTypesForFile(juce::OwnedArray<juce::PluginDescription>& descriptions, const juce::String&) override
    {
        descriptions.add(new juce::PluginDescription(m_probe->description));
    }

    bool fileMightContainThisPluginType(const juce::String& path) override { return path.endsWith(".async"); }
    juce::String getNameOfPluginFromIdentifier(const juce::String&) override { return m_probe->description.name; }
    bool pluginNeedsRescanning(const juce::PluginDescription&) override { return false; }
    bool doesPluginStillExist(const juce::PluginDescription&) override { return true; }
    bool canScanForPlugins() const override { return true; }
    bool isTrivialToScan() const override { return true; }
    juce::StringArray searchPathsForPlugins(const juce::FileSearchPath&, bool, bool) override { return {}; }
    juce::FileSearchPath getDefaultLocationsToSearch() override { return {}; }
    bool requiresUnblockedMessageThreadDuringCreation(const juce::PluginDescription&) const override { return true; }

private:
    void createPluginInstance(const juce::PluginDescription& description, double, int, PluginCreationCallback callback) override
    {
        QTimer::singleShot(20, [probe = m_probe, description, callback = std::move(callback)]() mutable {
            ++probe->completions;
            if (probe->fail) {
                callback(nullptr, "The test plug-in load failed.");
            } else {
                callback(std::make_unique<TestProcessor>(description), {});
            }
        });
    }

    std::shared_ptr<LoadProbe> m_probe;
};

class TestEffect final : public PerTrackEffect
{
public:
    PluginPath GetPath() const override { return "/audacity-test/plugin.async;00000000000000000000000000000001"; }
    ComponentInterfaceSymbol GetSymbol() const override { return wxT("Test plug-in"); }
    VendorSymbol GetVendor() const override { return wxT("Audacity"); }
    wxString GetVersion() const override { return "1.0"; }
    TranslatableString GetDescription() const override { return {}; }
    EffectFamilySymbol GetFamily() const override { return wxT("VST3"); }
    ::EffectType GetType() const override { return EffectTypeProcess; }
    RealtimeSince RealtimeSupport() const override { return RealtimeSince::After_3_1; }
    EffectSettings MakeSettings() const override { return EffectSettings::Make<double>(1.0); }
    bool SaveSettings(const EffectSettings&, CommandParameters&) const override { return true; }
    bool LoadSettings(const CommandParameters&, EffectSettings&) const override { return true; }
    bool CopySettingsContents(const EffectSettings&, EffectSettings&) const override { return true; }
    std::shared_ptr<EffectInstance> MakeInstance() const override { return {}; }
};

class AsyncPluginTests : public ::testing::Test
{
protected:
    void SetUp() override
    {
        ASSERT_TRUE(m_directory.isValid());
        m_probe->description.name = "Test plug-in";
        m_probe->description.descriptiveName = m_probe->description.name;
        m_probe->description.manufacturerName = "Audacity";
        m_probe->description.pluginFormatName = "VST3";
        m_probe->description.fileOrIdentifier = "/audacity-test/plugin.async";
        m_probe->description.uniqueIdString = "00000000000000000000000000000001";
        m_probe->description.uniqueId = 1;
        m_probe->description.numInputChannels = 1;
        m_probe->description.numOutputChannels = 1;
        m_runtime.init(m_directory.filePath("cache").toStdString(), m_directory.filePath("settings").toStdString());
        auto& formats = m_runtime.engine().getPluginManager().pluginFormatManager;
        std::destroy_at(&formats);
        std::construct_at(&formats);
        formats.addFormat(std::make_unique<TestFormat>(m_probe));
    }

    static void pumpUntil(const std::function<bool()>& ready)
    {
        QEventLoop loop;
        QTimer timer;
        QObject::connect(&timer, &QTimer::timeout, &loop, [&] {
#if JUCE_MAC
            CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.0, true);
#endif
            if (ready()) {
                loop.quit();
            }
        });
        timer.start(5);
        QTimer::singleShot(2000, &loop, &QEventLoop::quit);
        loop.exec();
    }

    QTemporaryDir m_directory;
    std::shared_ptr<LoadProbe> m_probe = std::make_shared<LoadProbe>();
    TracktionRuntime m_runtime;
    TestEffect m_effect;
};
}

TEST_F(AsyncPluginTests, EditorWaitsForTheProcessor)
{
    const auto instance = std::dynamic_pointer_cast<TracktionHostedEffectInstance>(createTracktionEffectInstance(m_runtime, m_effect));
    ASSERT_TRUE(instance);
    ASSERT_TRUE(instance->Init()) << instance->GetLastError();
    EXPECT_EQ(m_probe->completions, 1);
    EXPECT_EQ(instance->name(), "Test plug-in");
    EXPECT_TRUE(instance->tracktionPlugin().getAudioPluginInstance());
    EXPECT_FALSE(instance->tracktionPlugin().isInitialisingAsync());
}

TEST_F(AsyncPluginTests, FailedCompletionClearsTheEngineLoadingFlag)
{
    m_probe->fail = true;
    m_runtime.engine().getPluginManager().knownPluginList.addType(m_probe->description);
    tracktion::Edit edit(m_runtime.engine(), tracktion::Edit::forRendering);
    const auto state = tracktion::ExternalPlugin::create(m_runtime.engine(), m_probe->description);
    const tracktion::ExternalPlugin::Ptr plugin = new tracktion::ExternalPlugin({ edit, state, true });
    EXPECT_TRUE(plugin->isInitialisingAsync());
    pumpUntil([&] { return !plugin->isInitialisingAsync(); });
    EXPECT_EQ(m_probe->completions, 1);
    EXPECT_FALSE(plugin->getAudioPluginInstance());
    EXPECT_FALSE(plugin->isInitialisingAsync());
    EXPECT_EQ(plugin->getLoadError(), "The test plug-in load failed.");
}

TEST_F(AsyncPluginTests, EditorReportsTheLoadErrorAfterCompletion)
{
    m_probe->fail = true;
    const auto instance = std::dynamic_pointer_cast<TracktionHostedEffectInstance>(createTracktionEffectInstance(m_runtime, m_effect));
    ASSERT_TRUE(instance);
    EXPECT_FALSE(instance->Init());
    EXPECT_EQ(m_probe->completions, 1);
    EXPECT_EQ(instance->GetLastError(), "The test plug-in load failed.");
}

TEST_F(AsyncPluginTests, EffectGraphWaitsForEveryMonoProcessor)
{
    const RealtimeEffectState::EffectFactory::Scope factory([&](const PluginID&) -> const EffectInstanceFactory* { return &m_effect; });
    RealtimeEffectList list;
    const auto state = std::make_shared<RealtimeEffectState>(wxT("async-test"));
    ASSERT_TRUE(list.AddState(state));
    const auto id = m_runtime.effects()->add(state, list, nullptr, 2, false);
    DEFER { m_runtime.effects()->remove(id);
    };
    tracktion::Edit edit(m_runtime.engine(), tracktion::Edit::forRendering);
    juce::ValueTree value(tracktion::IDs::PLUGIN);
    value.setProperty(tracktion::IDs::type, AUDACITY_EFFECT_PLUGIN_TYPE, nullptr);
    value.setProperty(AUDACITY_EFFECT_STATE_PROPERTY, id, nullptr);
    const auto plugin = edit.getPluginCache().createNewPlugin(value);
    ASSERT_TRUE(plugin);
    EXPECT_EQ(m_probe->completions, 2);
    plugin->baseClassInitialise({ tracktion::TimePosition(), 48000.0, 128 });
    DEFER { plugin->baseClassDeinitialise();
    };
    juce::AudioBuffer<float> audio(2, 128);
    for (int channel = 0; channel < 2; ++channel) {
        std::fill_n(audio.getWritePointer(channel), 128, 0.5f);
    }
    tracktion::PluginRenderContext context(&audio, 0, 128, nullptr, 0.0,
                                           { tracktion::TimePosition(), tracktion::TimePosition::fromSeconds(128.0 / 48000.0) },
                                           false, false, true, false);
    context.referenceSampleRange = { 0, 128 };
    context.referenceSampleRate = 48000.0;
    plugin->applyToBuffer(context);
    for (int channel = 0; channel < 2; ++channel) {
        for (int frame = 0; frame < 128; ++frame) {
            EXPECT_FLOAT_EQ(audio.getSample(channel, frame), 0.25f);
        }
    }
}
}
