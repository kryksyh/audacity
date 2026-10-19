/*
* Audacity: A Digital Audio Editor
*/
#include <tracktion_engine/tracktion_engine.h>

#include <gtest/gtest.h>
#include <QTemporaryDir>
#include <QUrl>
#include <cstring>
#include <chrono>
#include <thread>
#include <QLibrary>
#include "../internal/tracktionlv2ui.h"
#include "plugin/lv2uiprobe.h"
#include "plugin/lv2workerprobe.h"
#include <lv2/urid/urid.h>
#include <lv2/atom/atom.h>
#include <lv2/state/state.h>

#include "../internal/tracktionruntime.h"
#include "../internal/tracktionpluginloader.h"
#include "../internal/tracktionplugindescriptions.h"

namespace au::tracktionaudio {
namespace {
class Lv2HostTests : public ::testing::Test
{
public:
    void SetUp() override
    {
        ASSERT_TRUE(directory.isValid());
        runtime.init(directory.filePath("cache").toStdString(), directory.filePath("settings").toStdString());
    }

    std::unique_ptr<juce::AudioPluginInstance> load(const char* name)
    {
        auto& manager = runtime.engine().getPluginManager();
        const auto bundle = QUrl::fromLocalFile(QString::fromUtf8(AU_TRACKTION_TEST_LV2_BUNDLE) + "/").toEncoded().toStdString();
        const auto identifier = std::string("urn:audacity:tracktion-test:") + name + "@" + bundle;
        for (auto* format : manager.pluginFormatManager.getFormats()) {
            if (format->getName() == "LV2") {
                const auto description = findTracktionPluginDescription(manager, *format, identifier, {});
                if (!description) {
                    return nullptr;
                }
                juce::String error;
                return loadTracktionPlugin(manager.pluginFormatManager, *description, error);
            }
        }
        return nullptr;
    }

    static float process(juce::AudioPluginInstance& processor, float input)
    {
        juce::AudioBuffer<float> audio(1, 64);
        std::fill_n(audio.getWritePointer(0), 64, input);
        juce::MidiBuffer midi;
        processor.processBlock(audio, midi);
        return audio.getSample(0, 63);
    }

    QTemporaryDir directory;
    TracktionRuntime runtime;
};

bool waitForWorker(const std::atomic<bool>& flag)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!flag.load() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return flag.load();
}
}

TEST_F(Lv2HostTests, StateIncludesParameterChangesBeforeTheFirstAudioBlock)
{
    const auto processor = load("gain");
    ASSERT_TRUE(processor);
    ASSERT_EQ(processor->getParameters().size(), 1);
    processor->getParameters()[0]->setValueNotifyingHost(0.75f);
    juce::MemoryBlock state;
    processor->getStateInformation(state);
    ASSERT_GT(state.getSize(), 0u);
    const auto restored = load("gain");
    ASSERT_TRUE(restored);
    restored->setStateInformation(state.getData(), static_cast<int>(state.getSize()));
    EXPECT_NEAR(restored->getParameters()[0]->getValue(), 0.75f, 0.00001f);
    restored->prepareToPlay(48000.0, 64);
    EXPECT_NEAR(process(*restored, 0.8f), 1.2f, 0.00001f);
    restored->releaseResources();
}

TEST_F(Lv2HostTests, SampleRateControlsUsePhysicalValuesAndKeepTheirRelativeValue)
{
    const auto processor = load("sample-rate");
    ASSERT_TRUE(processor);
    ASSERT_EQ(processor->getParameters().size(), 1);
    auto* parameter = processor->getParameters()[0];
    auto* client = processor->getLV2Client();
    ASSERT_TRUE(client);
    EXPECT_DOUBLE_EQ(client->getInstanceSampleRate(), 44100.0);
    EXPECT_FLOAT_EQ(*client->getControlPortValue(2), 11025.0f);
    EXPECT_NEAR(parameter->getText(parameter->getDefaultValue(), 64).getFloatValue(), 11025.0f, 0.01f);
    EXPECT_NEAR(parameter->getValueForText("11025"), 0.5f, 0.00001f);
    processor->prepareToPlay(48000.0, 64);
    EXPECT_DOUBLE_EQ(client->getInstanceSampleRate(), 48000.0);
    EXPECT_FLOAT_EQ(*client->getControlPortValue(2), 12000.0f);
    EXPECT_NEAR(parameter->getText(parameter->getValue(), 64).getFloatValue(), 12000.0f, 0.01f);
    EXPECT_NEAR(process(*processor, 0.8f), 0.2f, 0.00001f);
    parameter->setValueNotifyingHost(0.75f);
    processor->prepareToPlay(96000.0, 64);
    EXPECT_NEAR(parameter->getValue(), 0.75f, 0.00001f);
    EXPECT_DOUBLE_EQ(client->getInstanceSampleRate(), 96000.0);
    EXPECT_FLOAT_EQ(*client->getControlPortValue(2), 36000.0f);
    EXPECT_NEAR(parameter->getText(parameter->getValue(), 64).getFloatValue(), 36000.0f, 0.01f);
    EXPECT_NEAR(process(*processor, 0.8f), 0.3f, 0.00001f);
    processor->releaseResources();
}

TEST_F(Lv2HostTests, BypassNotificationsMatchTheParameterAfterControlWritesAndStateRestore)
{
    const auto processor = load("enabled");
    ASSERT_TRUE(processor);
    auto* client = processor->getLV2Client();
    ASSERT_TRUE(client);
    auto* bypass = processor->getBypassParameter();
    ASSERT_TRUE(bypass);
    EXPECT_FLOAT_EQ(bypass->getValue(), 0.0f);
    class Changes final : private juce::AudioProcessorParameter::Listener
    {
    public:
        explicit Changes(juce::AudioProcessorParameter& source)
            : m_source(source) { m_source.addListener(this); }
        ~Changes() override { m_source.removeListener(this); }
        std::vector<float> values;
    private:
        void parameterValueChanged(int, float value) override { values.push_back(value); }
        void parameterGestureChanged(int, bool) override {}
        juce::AudioProcessorParameter& m_source;
    } changes(*bypass);
    const auto expectBypass = [&](bool bypassed) {
        EXPECT_FLOAT_EQ(bypass->getValue(), bypassed ? 1.0f : 0.0f);
        EXPECT_FLOAT_EQ(*client->getControlPortValue(3), bypassed ? 0.0f : 1.0f);
        ASSERT_EQ(changes.values.size(), 1u);
        EXPECT_FLOAT_EQ(changes.values.front(), bypass->getValue());
        changes.values.clear();
    };
    ASSERT_TRUE(client->setControlPortValue(3, 0.0f));
    expectBypass(true);
    ASSERT_TRUE(client->setControlPortValue(3, 1.0f));
    expectBypass(false);
    QLibrary library(QString::fromStdString(client->getUis().front().binaryPath.toStdString()));
    const auto setEnabled = reinterpret_cast<void (*)(float)>(library.resolve("tracktion_ui_enabled"));
    ASSERT_TRUE(setEnabled);
    TracktionLv2Ui ui(*processor, {});
    ASSERT_TRUE(ui.open());
    setEnabled(0.0f);
    expectBypass(true);
    juce::MemoryBlock snapshot;
    processor->getStateInformation(snapshot);
    setEnabled(1.0f);
    expectBypass(false);
    ASSERT_TRUE(client->restoreState(snapshot));
    expectBypass(true);
    processor->prepareToPlay(48000.0, 64);
    EXPECT_NEAR(process(*processor, 0.8f), 0.8f, 1e-6f);
    bypass->setValueNotifyingHost(0.0f);
    expectBypass(false);
    EXPECT_NEAR(process(*processor, 0.8f), 0.4f, 1e-6f);
    bypass->setValueNotifyingHost(1.0f);
    expectBypass(true);
    EXPECT_NEAR(process(*processor, 0.8f), 0.8f, 1e-6f);
    processor->releaseResources();
}

TEST_F(Lv2HostTests, LatencyIsAvailableBeforeAudioWithoutAdvancingTheProcessor)
{
    const auto processor = load("latency");
    ASSERT_TRUE(processor);
    auto* client = processor->getLV2Client();
    ASSERT_TRUE(client);
    const auto connection = client->createUiConnection({});
    ASSERT_TRUE(connection);
    QLibrary library(QString::fromUtf8(AU_TRACKTION_TEST_LV2_BINARY));
    const auto frames = reinterpret_cast<uint64_t (*)(const void*)>(library.resolve("tracktion_dsp_frames"));
    ASSERT_TRUE(frames);
    processor->prepareToPlay(48000.0, 64);
    EXPECT_EQ(processor->getLatencySamples(), 17);
    EXPECT_EQ(frames(connection->getInstanceHandle()), 0u);
    EXPECT_NEAR(process(*processor, 0.8f), 0.4f, 1e-6f);
    EXPECT_EQ(frames(connection->getInstanceHandle()), 64u);
    EXPECT_EQ(processor->getLatencySamples(), 17);
    processor->releaseResources();
}

TEST_F(Lv2HostTests, WorkerLatencyIsMeasuredBeforePlaybackAndWorkerModeIsRestored)
{
    const auto processor = load("latency-worker");
    ASSERT_TRUE(processor);
    auto* client = processor->getLV2Client();
    ASSERT_TRUE(client);
    const auto connection = client->createUiConnection({});
    ASSERT_TRUE(connection);
    QLibrary library(QString::fromUtf8(AU_TRACKTION_TEST_LV2_BINARY));
    const auto synchronous = reinterpret_cast<bool (*)(const void*)>(library.resolve("tracktion_dsp_worker_synchronous"));
    const auto request = reinterpret_cast<void (*)(void*)>(library.resolve("tracktion_dsp_request_work"));
    const auto frames = reinterpret_cast<uint64_t (*)(const void*)>(library.resolve("tracktion_dsp_frames"));
    ASSERT_TRUE(synchronous);
    ASSERT_TRUE(request);
    ASSERT_TRUE(frames);
    processor->prepareToPlay(48000.0, 64);
    auto* handle = connection->getInstanceHandle();
    EXPECT_EQ(processor->getLatencySamples(), 17);
    EXPECT_EQ(frames(handle), 0u);
    EXPECT_TRUE(synchronous(handle));
    EXPECT_FALSE(processor->isNonRealtime());
    request(handle);
    process(*processor, 0.8f);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    while (synchronous(handle) && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    EXPECT_FALSE(synchronous(handle));
    processor->setNonRealtime(true);
    request(handle);
    process(*processor, 0.8f);
    EXPECT_TRUE(synchronous(handle));
    EXPECT_EQ(processor->getLatencySamples(), 17);
    processor->releaseResources();
}

TEST_F(Lv2HostTests, WorkerCompletionCommitsAudioAfterResponsesAndEveryRun)
{
    QLibrary library(QString::fromUtf8(AU_TRACKTION_TEST_LV2_BINARY));
    const auto attach = reinterpret_cast<void (*)(void*, Lv2WorkerProbe*)>(library.resolve("tracktion_dsp_worker_probe"));
    ASSERT_TRUE(attach);
    for (const bool offline : { false, true }) {
        SCOPED_TRACE(offline);
        Lv2WorkerProbe probe;
        probe.release.store(offline);
        const auto processor = load("worker-end");
        ASSERT_TRUE(processor);
        processor->setNonRealtime(offline);
        processor->prepareToPlay(48000.0, 64);
        const auto connection = processor->getLV2Client()->createUiConnection({});
        ASSERT_TRUE(connection);
        attach(connection->getInstanceHandle(), &probe);
        EXPECT_NEAR(process(*processor, 0.8f), 0.4f, 1e-6f);
        EXPECT_EQ(probe.runs.load(), 1);
        EXPECT_EQ(probe.endRuns.load(), 1);
        EXPECT_EQ(probe.repliesAtEnd.load(), offline ? 1 : 0);
        probe.release.store(true);
        ASSERT_TRUE(waitForWorker(probe.finished));
        EXPECT_NEAR(process(*processor, 0.8f), offline ? 0.1f : 0.4f, 1e-6f);
        EXPECT_EQ(probe.repliesAtEnd.load(), 1);
        EXPECT_NEAR(process(*processor, 0.8f), 0.1f, 1e-6f);
        juce::AudioBuffer<float> empty(1, 0);
        juce::MidiBuffer midi;
        processor->processBlock(empty, midi);
        EXPECT_EQ(probe.runs.load(), 4);
        EXPECT_EQ(probe.endRuns.load(), 4);
        EXPECT_EQ(probe.repliesAtEnd.load(), 1);
        EXPECT_NEAR(process(*processor, 0.8f), 0.1f, 1e-6f);
        EXPECT_EQ(probe.endRuns.load(), 5);
        processor->releaseResources();
    }
}

TEST_F(Lv2HostTests, EmptyWorkerRequestsAndRepliesKeepTheProcessingCycle)
{
    QLibrary library(QString::fromUtf8(AU_TRACKTION_TEST_LV2_BINARY));
    const auto attach = reinterpret_cast<void (*)(void*, Lv2WorkerProbe*)>(library.resolve("tracktion_dsp_worker_probe"));
    const auto request = reinterpret_cast<void (*)(void*)>(library.resolve("tracktion_dsp_request_empty_work"));
    ASSERT_TRUE(attach);
    ASSERT_TRUE(request);
    for (const bool offline : { false, true }) {
        SCOPED_TRACE(offline);
        Lv2WorkerProbe probe;
        probe.release.store(offline);
        const auto processor = load("worker-end");
        ASSERT_TRUE(processor);
        processor->setNonRealtime(offline);
        processor->prepareToPlay(48000.0, 64);
        const auto connection = processor->getLV2Client()->createUiConnection({});
        ASSERT_TRUE(connection);
        auto* handle = connection->getInstanceHandle();
        attach(handle, &probe);
        request(handle);
        EXPECT_NEAR(process(*processor, 0.8f), 0.4f, 1e-6f);
        probe.release.store(true);
        ASSERT_TRUE(waitForWorker(probe.finished));
        process(*processor, 0.8f);
        EXPECT_EQ(probe.workCalls.load(), 1);
        EXPECT_EQ(probe.emptyRequests.load(), 1);
        EXPECT_EQ(probe.replies.load(), 1);
        EXPECT_EQ(probe.emptyReplies.load(), 1);
        EXPECT_EQ(probe.repliesAtEnd.load(), 1);
        EXPECT_EQ(probe.endRuns.load(), 2);
        EXPECT_NEAR(process(*processor, 0.8f), 0.1f, 1e-6f);
        processor->releaseResources();
    }
}

TEST_F(Lv2HostTests, PendingWorkerRepliesDoNotReachADestroyedProcessor)
{
    QLibrary library(QString::fromUtf8(AU_TRACKTION_TEST_LV2_BINARY));
    const auto attach = reinterpret_cast<void (*)(void*, Lv2WorkerProbe*)>(library.resolve("tracktion_dsp_worker_probe"));
    const auto request = reinterpret_cast<void (*)(void*)>(library.resolve("tracktion_dsp_request_work"));
    ASSERT_TRUE(attach);
    ASSERT_TRUE(request);
    Lv2WorkerProbe probe;
    const auto survivor = load("gain");
    auto processor = load("latency-worker");
    ASSERT_TRUE(survivor);
    ASSERT_TRUE(processor);
    survivor->prepareToPlay(48000.0, 64);
    processor->prepareToPlay(48000.0, 64);
    const auto connection = processor->getLV2Client()->createUiConnection({});
    ASSERT_TRUE(connection);
    auto* handle = connection->getInstanceHandle();
    attach(handle, &probe);
    request(handle);
    process(*processor, 0.8f);
    probe.release.store(true);
    ASSERT_TRUE(waitForWorker(probe.finished));
    connection->disconnect();
    processor.reset();
    EXPECT_TRUE(probe.cleaned.load());
    EXPECT_NEAR(process(*survivor, 0.8f), 0.4f, 1e-6f);
    EXPECT_EQ(probe.replies.load(), 0);
    survivor->releaseResources();
}

TEST_F(Lv2HostTests, DestroyingAProcessorWaitsForRunningWork)
{
    QLibrary library(QString::fromUtf8(AU_TRACKTION_TEST_LV2_BINARY));
    const auto attach = reinterpret_cast<void (*)(void*, Lv2WorkerProbe*)>(library.resolve("tracktion_dsp_worker_probe"));
    const auto request = reinterpret_cast<void (*)(void*)>(library.resolve("tracktion_dsp_request_work"));
    ASSERT_TRUE(attach);
    ASSERT_TRUE(request);
    Lv2WorkerProbe probe;
    const auto survivor = load("gain");
    auto processor = load("latency-worker");
    ASSERT_TRUE(survivor);
    ASSERT_TRUE(processor);
    processor->prepareToPlay(48000.0, 64);
    const auto connection = processor->getLV2Client()->createUiConnection({});
    ASSERT_TRUE(connection);
    auto* handle = connection->getInstanceHandle();
    attach(handle, &probe);
    request(handle);
    process(*processor, 0.8f);
    const bool entered = waitForWorker(probe.entered);
    if (!entered) {
        probe.release.store(true);
    }
    ASSERT_TRUE(entered);
    std::thread release([&probe] {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        probe.release.store(true);
    });
    connection->disconnect();
    processor.reset();
    release.join();
    EXPECT_TRUE(probe.finished.load());
    EXPECT_TRUE(probe.cleaned.load());
    EXPECT_FALSE(probe.cleanedWhileRunning.load());
}

TEST_F(Lv2HostTests, QueuedWorkIsDiscardedWhenItsProcessorIsDestroyed)
{
    QLibrary library(QString::fromUtf8(AU_TRACKTION_TEST_LV2_BINARY));
    const auto attach = reinterpret_cast<void (*)(void*, Lv2WorkerProbe*)>(library.resolve("tracktion_dsp_worker_probe"));
    const auto request = reinterpret_cast<void (*)(void*)>(library.resolve("tracktion_dsp_request_work"));
    ASSERT_TRUE(attach);
    ASSERT_TRUE(request);
    Lv2WorkerProbe firstProbe;
    Lv2WorkerProbe removedProbe;
    Lv2WorkerProbe lastProbe;
    removedProbe.release.store(true);
    lastProbe.release.store(true);
    const auto first = load("latency-worker");
    auto removed = load("latency-worker");
    const auto last = load("latency-worker");
    ASSERT_TRUE(first);
    ASSERT_TRUE(removed);
    ASSERT_TRUE(last);
    first->prepareToPlay(48000.0, 64);
    removed->prepareToPlay(48000.0, 64);
    last->prepareToPlay(48000.0, 64);
    const auto firstConnection = first->getLV2Client()->createUiConnection({});
    const auto removedConnection = removed->getLV2Client()->createUiConnection({});
    const auto lastConnection = last->getLV2Client()->createUiConnection({});
    ASSERT_TRUE(firstConnection);
    ASSERT_TRUE(removedConnection);
    ASSERT_TRUE(lastConnection);
    attach(firstConnection->getInstanceHandle(), &firstProbe);
    attach(removedConnection->getInstanceHandle(), &removedProbe);
    attach(lastConnection->getInstanceHandle(), &lastProbe);
    request(firstConnection->getInstanceHandle());
    process(*first, 0.8f);
    const bool entered = waitForWorker(firstProbe.entered);
    if (!entered) {
        firstProbe.release.store(true);
    }
    ASSERT_TRUE(entered);
    request(removedConnection->getInstanceHandle());
    process(*removed, 0.8f);
    removedConnection->disconnect();
    removed.reset();
    request(lastConnection->getInstanceHandle());
    process(*last, 0.8f);
    firstProbe.release.store(true);
    ASSERT_TRUE(waitForWorker(lastProbe.finished));
    EXPECT_TRUE(firstProbe.finished.load());
    EXPECT_TRUE(removedProbe.cleaned.load());
    EXPECT_EQ(removedProbe.workCalls.load(), 0);
    process(*first, 0.8f);
    process(*last, 0.8f);
    EXPECT_EQ(firstProbe.replies.load(), 1);
    EXPECT_EQ(lastProbe.replies.load(), 1);
    first->releaseResources();
    last->releaseResources();
}

TEST_F(Lv2HostTests, WorkerRepliesAreDeliveredOnlyWhenTheirProcessorRuns)
{
    QLibrary library(QString::fromUtf8(AU_TRACKTION_TEST_LV2_BINARY));
    const auto attach = reinterpret_cast<void (*)(void*, Lv2WorkerProbe*)>(library.resolve("tracktion_dsp_worker_probe"));
    const auto request = reinterpret_cast<void (*)(void*)>(library.resolve("tracktion_dsp_request_work"));
    ASSERT_TRUE(attach);
    ASSERT_TRUE(request);
    Lv2WorkerProbe firstProbe;
    Lv2WorkerProbe secondProbe;
    const auto first = load("latency-worker");
    const auto second = load("latency-worker");
    ASSERT_TRUE(first);
    ASSERT_TRUE(second);
    first->prepareToPlay(48000.0, 64);
    second->prepareToPlay(48000.0, 64);
    const auto firstConnection = first->getLV2Client()->createUiConnection({});
    const auto secondConnection = second->getLV2Client()->createUiConnection({});
    ASSERT_TRUE(firstConnection);
    ASSERT_TRUE(secondConnection);
    attach(firstConnection->getInstanceHandle(), &firstProbe);
    attach(secondConnection->getInstanceHandle(), &secondProbe);
    request(firstConnection->getInstanceHandle());
    process(*first, 0.8f);
    request(secondConnection->getInstanceHandle());
    process(*second, 0.8f);
    firstProbe.release.store(true);
    secondProbe.release.store(true);
    ASSERT_TRUE(waitForWorker(firstProbe.finished));
    ASSERT_TRUE(waitForWorker(secondProbe.finished));
    process(*first, 0.8f);
    EXPECT_EQ(firstProbe.replies.load(), 1);
    EXPECT_EQ(secondProbe.replies.load(), 0);
    process(*second, 0.8f);
    EXPECT_EQ(secondProbe.replies.load(), 1);
    first->releaseResources();
    second->releaseResources();
}

TEST_F(Lv2HostTests, RecreatingAProcessorDiscardsItsPendingWorkerReplies)
{
    QLibrary library(QString::fromUtf8(AU_TRACKTION_TEST_LV2_BINARY));
    const auto attach = reinterpret_cast<void (*)(void*, Lv2WorkerProbe*)>(library.resolve("tracktion_dsp_worker_probe"));
    const auto request = reinterpret_cast<void (*)(void*)>(library.resolve("tracktion_dsp_request_work"));
    ASSERT_TRUE(attach);
    ASSERT_TRUE(request);
    Lv2WorkerProbe probe;
    const auto processor = load("latency-worker");
    ASSERT_TRUE(processor);
    processor->prepareToPlay(48000.0, 64);
    const auto connection = processor->getLV2Client()->createUiConnection({});
    ASSERT_TRUE(connection);
    attach(connection->getInstanceHandle(), &probe);
    request(connection->getInstanceHandle());
    process(*processor, 0.8f);
    probe.release.store(true);
    ASSERT_TRUE(waitForWorker(probe.finished));
    processor->prepareToPlay(96000.0, 64);
    EXPECT_TRUE(probe.cleaned.load());
    EXPECT_EQ(probe.replies.load(), 0);
    EXPECT_EQ(processor->getLatencySamples(), 17);
    EXPECT_NEAR(process(*processor, 0.8f), 0.4f, 1e-6f);
    processor->releaseResources();
}

TEST_F(Lv2HostTests, RecreatingAProcessorWaitsForWorkBeforeDeactivation)
{
    QLibrary library(QString::fromUtf8(AU_TRACKTION_TEST_LV2_BINARY));
    const auto attach = reinterpret_cast<void (*)(void*, Lv2WorkerProbe*)>(library.resolve("tracktion_dsp_worker_probe"));
    const auto request = reinterpret_cast<void (*)(void*)>(library.resolve("tracktion_dsp_request_work"));
    ASSERT_TRUE(attach);
    ASSERT_TRUE(request);
    Lv2WorkerProbe probe;
    const auto processor = load("latency-worker");
    ASSERT_TRUE(processor);
    processor->prepareToPlay(48000.0, 64);
    const auto connection = processor->getLV2Client()->createUiConnection({});
    ASSERT_TRUE(connection);
    attach(connection->getInstanceHandle(), &probe);
    request(connection->getInstanceHandle());
    process(*processor, 0.8f);
    const bool entered = waitForWorker(probe.entered);
    if (!entered) {
        probe.release.store(true);
    }
    ASSERT_TRUE(entered);
    std::thread release([&probe] {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        probe.release.store(true);
    });
    processor->prepareToPlay(96000.0, 64);
    release.join();
    EXPECT_TRUE(probe.finished.load());
    EXPECT_TRUE(probe.cleaned.load());
    EXPECT_FALSE(probe.deactivatedWhileRunning.load());
    EXPECT_FALSE(probe.cleanedWhileRunning.load());
    EXPECT_EQ(probe.replies.load(), 0);
    EXPECT_EQ(processor->getLatencySamples(), 17);
    processor->releaseResources();
}

TEST_F(Lv2HostTests, MissingDefaultsUseTheDeclaredBound)
{
    const auto processor = load("bounded-default");
    ASSERT_TRUE(processor);
    ASSERT_EQ(processor->getParameters().size(), 1);
    auto* parameter = processor->getParameters()[0];
    EXPECT_NEAR(parameter->getDefaultValue(), 0.0f, 0.00001f);
    EXPECT_NEAR(parameter->getText(parameter->getDefaultValue(), 64).getFloatValue(), 1.0f, 0.00001f);
    processor->prepareToPlay(44100.0, 64);
    EXPECT_NEAR(process(*processor, 0.8f), 0.8f, 0.00001f);
    processor->releaseResources();
}

TEST_F(Lv2HostTests, ControlPortMetadataKeepsNamesSymbolsGroupsAndScalePoints)
{
    const auto processor = load("rich");
    ASSERT_TRUE(processor);
    const auto* client = processor->getLV2Client();
    ASSERT_TRUE(client);
    const auto ports = client->getControlPorts();
    ASSERT_EQ(ports.size(), 6u);
    EXPECT_EQ(ports[0].index, 2u);
    EXPECT_EQ(ports[0].symbol, "gain");
    EXPECT_EQ(ports[0].name, "Gain");
    EXPECT_EQ(ports[0].group, juce::String::fromUTF8("Advanced Étage"));
    EXPECT_EQ(ports[0].units, "db");
    EXPECT_TRUE(ports[0].isInput);
    EXPECT_TRUE(ports[0].hasMinimum);
    EXPECT_TRUE(ports[0].hasMaximum);
    EXPECT_FLOAT_EQ(ports[0].minimum, 0.0f);
    EXPECT_FLOAT_EQ(ports[0].maximum, 2.0f);
    EXPECT_FLOAT_EQ(ports[0].defaultValue, 0.5f);
    EXPECT_TRUE(ports[1].isLogarithmic);
    EXPECT_TRUE(ports[2].isToggle);
    EXPECT_TRUE(ports[3].isInteger);
    EXPECT_TRUE(ports[3].isEnumeration);
    ASSERT_EQ(ports[3].scalePoints.size(), 2u);
    EXPECT_FLOAT_EQ(ports[3].scalePoints[0].first, 1.0f);
    EXPECT_EQ(ports[3].scalePoints[0].second, "Low");
    EXPECT_FLOAT_EQ(ports[3].scalePoints[1].first, 4.0f);
    EXPECT_EQ(ports[3].scalePoints[1].second, juce::String::fromUTF8("Élevé"));
    EXPECT_TRUE(ports[4].isTrigger);
    EXPECT_FALSE(ports[5].isInput);
    const auto sampled = load("sample-rate");
    ASSERT_TRUE(sampled);
    ASSERT_EQ(sampled->getLV2Client()->getControlPorts().size(), 1u);
    EXPECT_TRUE(sampled->getLV2Client()->getControlPorts()[0].isSampleRate);
    EXPECT_FLOAT_EQ(sampled->getLV2Client()->getControlPorts()[0].maximum, 0.5f);
}

TEST_F(Lv2HostTests, ControlPortValuesUsePhysicalUnitsAndKeepOutputPortsReadOnly)
{
    const auto processor = load("rich");
    ASSERT_TRUE(processor);
    auto* client = processor->getLV2Client();
    ASSERT_TRUE(client);
    ASSERT_TRUE(client->getControlPortValue(2));
    EXPECT_FLOAT_EQ(*client->getControlPortValue(2), 0.5f);
    EXPECT_TRUE(client->setControlPortValue(2, 1.5f));
    EXPECT_FLOAT_EQ(*client->getControlPortValue(2), 1.5f);
    EXPECT_FALSE(client->setControlPortValue(7, 1.0f));
    EXPECT_FALSE(client->setControlPortValue(100, 1.0f));
    EXPECT_FALSE(client->getControlPortValue(100));
    processor->prepareToPlay(44100.0, 64);
    EXPECT_NEAR(process(*processor, 0.8f), 1.2f, 0.00001f);
    EXPECT_FLOAT_EQ(*client->getControlPortValue(7), 1.5f);
    processor->releaseResources();
}

TEST_F(Lv2HostTests, FactoryPresetsAndSnapshotsKeepPrivateState)
{
    const auto processor = load("private");
    ASSERT_TRUE(processor);
    ASSERT_EQ(processor->getNumPrograms(), 4);
    int quiet = -1;
    int unlabelled = -1;
    for (int index = 0; index < processor->getNumPrograms(); ++index) {
        const auto name = processor->getProgramName(index);
        if (name == "Quiet") {
            quiet = index;
        } else if (name == "unlabelled") {
            unlabelled = index;
        }
    }
    ASSERT_GE(quiet, 0);
    ASSERT_GE(unlabelled, 0);
    processor->setCurrentProgram(quiet);
    auto* client = processor->getLV2Client();
    ASSERT_TRUE(client);
    ASSERT_TRUE(client->getControlPortValue(2));
    EXPECT_FLOAT_EQ(*client->getControlPortValue(2), 1.5f);
    processor->prepareToPlay(48000.0, 64);
    EXPECT_NEAR(process(*processor, 0.8f), 0.3f, 0.00001f);
    EXPECT_TRUE(client->setControlPortValue(2, 2.0f));
    juce::MemoryBlock quietState;
    processor->getStateInformation(quietState);
    processor->setCurrentProgram(unlabelled);
    EXPECT_NEAR(process(*processor, 0.8f), 0.6f, 0.00001f);
    EXPECT_TRUE(client->restoreState(quietState));
    EXPECT_NEAR(process(*processor, 0.8f), 0.4f, 0.00001f);
    const auto restored = load("private");
    ASSERT_TRUE(restored);
    ASSERT_TRUE(restored->getLV2Client()->restoreState(quietState));
    restored->prepareToPlay(44100.0, 64);
    EXPECT_NEAR(process(*restored, 0.8f), 0.4f, 0.00001f);
    const auto foreign = load("gain");
    ASSERT_TRUE(foreign);
    juce::MemoryBlock foreignState;
    foreign->getStateInformation(foreignState);
    EXPECT_FALSE(client->restoreState(foreignState));
    const juce::MemoryBlock invalid("not a state", 11);
    EXPECT_FALSE(client->restoreState(invalid));
    EXPECT_FALSE(client->restoreState({}));
    EXPECT_NEAR(process(*processor, 0.8f), 0.4f, 0.00001f);
    restored->releaseResources();
    processor->releaseResources();
}

TEST_F(Lv2HostTests, FactoryPresetIndicesRemainTheSameAcrossHostInstances)
{
    const std::array<juce::String, 4> names { "Bad", "Partial", "Quiet", "unlabelled" };
    for (int attempt = 0; attempt < 3; ++attempt) {
        const auto processor = load("private");
        ASSERT_TRUE(processor);
        ASSERT_EQ(processor->getNumPrograms(), names.size());
        for (int index = 0; index < processor->getNumPrograms(); ++index) {
            EXPECT_EQ(processor->getProgramName(index), names[index]);
        }
        ASSERT_TRUE(processor->getLV2Client()->loadFactoryPreset(2));
        processor->prepareToPlay(48000.0, 64);
        EXPECT_NEAR(process(*processor, 0.8f), 0.3f, 0.00001f);
        processor->releaseResources();
    }
}

TEST_F(Lv2HostTests, FailedRestoresKeepControlAndPrivateState)
{
    const auto processor = load("private");
    ASSERT_TRUE(processor);
    auto* client = processor->getLV2Client();
    ASSERT_TRUE(client);
    int quiet = -1;
    int bad = -1;
    for (int index = 0; index < processor->getNumPrograms(); ++index) {
        const auto name = processor->getProgramName(index);
        if (name == "Quiet") {
            quiet = index;
        } else if (name == "Bad") {
            bad = index;
        }
    }
    ASSERT_GE(quiet, 0);
    ASSERT_GE(bad, 0);
    ASSERT_TRUE(client->loadFactoryPreset(quiet));
    ASSERT_TRUE(client->setControlPortValue(2, 2.0f));
    processor->prepareToPlay(48000.0, 64);
    EXPECT_NEAR(process(*processor, 0.8f), 0.4f, 0.00001f);
    constexpr auto badState
        =
            R"(@prefix lv2: <http://lv2plug.in/ns/lv2core#> .
@prefix pset: <http://lv2plug.in/ns/ext/presets#> .
@prefix state: <http://lv2plug.in/ns/ext/state#> .
<urn:audacity:badState> a pset:Preset ;
    lv2:appliesTo <urn:audacity:tracktion-test:private> ;
    lv2:port [ lv2:symbol "gain" ; pset:value 0.0 ] ;
    state:state [ <urn:audacity:privateGain> "invalid" ] .)";
    EXPECT_FALSE(client->restoreState(juce::MemoryBlock(badState, std::strlen(badState))));
    EXPECT_FLOAT_EQ(*client->getControlPortValue(2), 2.0f);
    EXPECT_NEAR(process(*processor, 0.8f), 0.4f, 0.00001f);
    EXPECT_FALSE(client->loadFactoryPreset(bad));
    EXPECT_EQ(processor->getCurrentProgram(), quiet);
    EXPECT_FLOAT_EQ(*client->getControlPortValue(2), 2.0f);
    EXPECT_NEAR(process(*processor, 0.8f), 0.4f, 0.00001f);
    processor->setCurrentProgram(bad);
    EXPECT_EQ(processor->getCurrentProgram(), quiet);
    EXPECT_NEAR(process(*processor, 0.8f), 0.4f, 0.00001f);
    EXPECT_FALSE(client->loadFactoryPreset(-1));
    EXPECT_FALSE(client->loadFactoryPreset(processor->getNumPrograms()));
    processor->releaseResources();
}

TEST_F(Lv2HostTests, StateNotificationsExcludeFailedRestoresAndUnchangedControls)
{
    const auto processor = load("private");
    ASSERT_TRUE(processor);
    class Changes final : public juce::AudioProcessorListener
    {
    public:
        explicit Changes(juce::AudioProcessor& source)
            : m_source(source) { m_source.addListener(this); }
        ~Changes() override { m_source.removeListener(this); }
        void audioProcessorParameterChanged(juce::AudioProcessor*, int, float) override { ++parameters; }
        void audioProcessorChanged(juce::AudioProcessor*, const ChangeDetails& details) override
        {
            programs += details.programChanged ? 1 : 0;
            states += details.nonParameterStateChanged ? 1 : 0;
        }

        void reset() { parameters = programs = states = 0; }
        int parameters = 0;
        int programs = 0;
        int states = 0;
    private:
        juce::AudioProcessor& m_source;
    } changes(*processor);
    auto* client = processor->getLV2Client();
    ASSERT_TRUE(client);
    int quiet = -1;
    int bad = -1;
    for (int index = 0; index < processor->getNumPrograms(); ++index) {
        const auto name = processor->getProgramName(index);
        if (name == "Quiet") {
            quiet = index;
        } else if (name == "Bad") {
            bad = index;
        }
    }
    ASSERT_GE(quiet, 0);
    ASSERT_GE(bad, 0);
    ASSERT_TRUE(client->loadFactoryPreset(quiet));
    EXPECT_EQ(changes.programs, 1);
    EXPECT_EQ(changes.states, 1);
    juce::MemoryBlock snapshot;
    processor->getStateInformation(snapshot);
    changes.reset();
    ASSERT_TRUE(client->setControlPortValue(2, 1.5f));
    EXPECT_EQ(changes.parameters, 0);
    EXPECT_FALSE(client->loadFactoryPreset(bad));
    EXPECT_EQ(changes.parameters, 0);
    EXPECT_EQ(changes.programs, 0);
    EXPECT_EQ(changes.states, 0);
    ASSERT_TRUE(client->setControlPortValue(2, 2.0f));
    EXPECT_EQ(changes.parameters, 1);
    changes.reset();
    ASSERT_TRUE(client->restoreState(snapshot));
    EXPECT_EQ(changes.parameters, 1);
    EXPECT_EQ(changes.programs, 0);
    EXPECT_EQ(changes.states, 1);
}

TEST_F(Lv2HostTests, PartialPresetsKeepPendingControlsThatThePresetDoesNotSpecify)
{
    const auto processor = load("private");
    ASSERT_TRUE(processor);
    auto* client = processor->getLV2Client();
    ASSERT_TRUE(client);
    int partial = -1;
    for (int index = 0; index < processor->getNumPrograms(); ++index) {
        if (processor->getProgramName(index) == "Partial") {
            partial = index;
        }
    }
    ASSERT_GE(partial, 0);
    ASSERT_TRUE(client->setControlPortValue(2, 1.5f));
    ASSERT_TRUE(client->loadFactoryPreset(partial));
    EXPECT_FLOAT_EQ(*client->getControlPortValue(2), 1.5f);
    processor->prepareToPlay(44100.0, 64);
    EXPECT_NEAR(process(*processor, 0.8f), 0.6f, 0.00001f);
    ASSERT_TRUE(client->setControlPortValue(2, 2.0f));
    constexpr auto state
        =
            R"(@prefix lv2: <http://lv2plug.in/ns/lv2core#> .
@prefix pset: <http://lv2plug.in/ns/ext/presets#> .
@prefix state: <http://lv2plug.in/ns/ext/state#> .
@prefix xsd: <http://www.w3.org/2001/XMLSchema#> .
<urn:audacity:partialState> a pset:Preset ;
    lv2:appliesTo <urn:audacity:tracktion-test:private> ;
    state:state [ <urn:audacity:privateGain> "0.25"^^xsd:float ] .)";
    ASSERT_TRUE(client->restoreState(juce::MemoryBlock(state, std::strlen(state))));
    EXPECT_FLOAT_EQ(*client->getControlPortValue(2), 2.0f);
    EXPECT_NEAR(process(*processor, 0.8f), 0.4f, 0.00001f);
    processor->releaseResources();
}

TEST_F(Lv2HostTests, VendorEditorMetadataUsesTheNativeBundleAndUiTypes)
{
    const auto processor = load("ui-echo");
    ASSERT_TRUE(processor);
    const auto uis = processor->getLV2Client()->getUis();
    ASSERT_EQ(uis.size(), 3u);
    const auto qt = std::find_if(uis.begin(), uis.end(), [](const auto& ui) { return ui.uri.endsWith("#qt"); });
    ASSERT_NE(qt, uis.end());
    EXPECT_TRUE(qt->types.contains("http://lv2plug.in/ns/extensions/ui#Qt6UI"));
    EXPECT_TRUE(qt->requiredFeatures.contains("http://lv2plug.in/ns/ext/instance-access"));
    EXPECT_EQ(juce::File(qt->bundlePath).getFullPathName(), juce::File(AU_TRACKTION_TEST_LV2_BUNDLE).getFullPathName());
    EXPECT_EQ(juce::File(qt->binaryPath).getFileNameWithoutExtension(), "hosttest");
    EXPECT_TRUE(juce::File(qt->binaryPath).existsAsFile());
    const auto plain = load("gain");
    ASSERT_TRUE(plain);
    EXPECT_TRUE(plain->getLV2Client()->getUis().empty());
}

TEST_F(Lv2HostTests, VendorEditorFeaturesAndInstanceAccessUseTheActiveProcessor)
{
    const auto processor = load("private");
    ASSERT_TRUE(processor);
    auto* client = processor->getLV2Client();
    const auto connection = client->createUiConnection({});
    ASSERT_TRUE(connection);
    ASSERT_TRUE(connection->getInstanceHandle());
    ASSERT_TRUE(connection->getExtensionDataFunction());
    EXPECT_FALSE(client->createUiConnection({}));
    const auto features = connection->getFeatures();
    const auto mapped = std::find_if(features.begin(), features.end(), [](const auto& feature) { return feature.uri == LV2_URID__map; });
    ASSERT_NE(mapped, features.end());
    const auto* map = static_cast<const LV2_URID_Map*>(mapped->data);
    ASSERT_TRUE(map);
    const auto* state = static_cast<const LV2_State_Interface*>(connection->getExtensionDataFunction()(LV2_STATE__interface));
    ASSERT_TRUE(state);
    const auto floatType = map->map(map->handle, LV2_ATOM__Float);
    const float privateGain = 0.25f;
    struct Property {
        LV2_URID type;
        const float* value;
    } property { floatType, &privateGain };
    const auto retrieve = [](LV2_State_Handle handle, uint32_t, size_t* size, uint32_t* type, uint32_t* flags) -> const void* {
        const auto& property = *static_cast<const Property*>(handle);
        *size = sizeof(float);
        *type = property.type;
        *flags = LV2_STATE_IS_POD | LV2_STATE_IS_PORTABLE;
        return property.value;
    };
    EXPECT_EQ(state->restore(connection->getInstanceHandle(), retrieve, &property, 0, nullptr), LV2_STATE_SUCCESS);
    const float gain = 1.5f;
    ASSERT_TRUE(connection->writePort(2, sizeof(gain), 0, &gain));
    processor->prepareToPlay(48000.0, 64);
    EXPECT_NEAR(process(*processor, 0.8f), 0.3f, 1e-6f);
    processor->releaseResources();
}

TEST_F(Lv2HostTests, VendorEditorControlEventsKeepPhysicalValuesAndReadOnlyOutputs)
{
    const auto processor = load("rich");
    ASSERT_TRUE(processor);
    auto* client = processor->getLV2Client();
    const auto connection = client->createUiConnection({});
    ASSERT_TRUE(connection);
    EXPECT_EQ(connection->getPortIndex("in"), 0u);
    EXPECT_EQ(connection->getPortIndex("gain"), 2u);
    EXPECT_EQ(connection->getPortIndex("meter"), 7u);
    EXPECT_EQ(connection->getPortIndex("missing"), UINT32_MAX);
    std::map<uint32_t, float> values;
    int events = 0;
    const auto receive = [&](uint32_t port, uint32_t size, uint32_t protocol, const void* data) {
        EXPECT_EQ(size, sizeof(float));
        EXPECT_EQ(protocol, 0u);
        float value = 0.0f;
        std::memcpy(&value, data, sizeof(value));
        values[port] = value;
        ++events;
    };
    connection->dispatchEvents(receive);
    EXPECT_EQ(values.size(), 6u);
    EXPECT_FLOAT_EQ(values.at(2), 0.5f);
    const auto initialEvents = events;
    connection->dispatchEvents(receive);
    EXPECT_EQ(events, initialEvents);
    const float gain = 1.25f;
    EXPECT_FALSE(connection->writePort(2, sizeof(gain), 0, nullptr));
    EXPECT_FALSE(connection->writePort(2, 1, 0, &gain));
    EXPECT_FALSE(connection->writePort(0, sizeof(gain), 0, &gain));
    EXPECT_FALSE(connection->writePort(7, sizeof(gain), 0, &gain));
    ASSERT_TRUE(connection->writePort(2, sizeof(gain), 0, &gain));
    connection->dispatchEvents(receive);
    EXPECT_FLOAT_EQ(values.at(2), gain);
    processor->prepareToPlay(48000.0, 64);
    EXPECT_NEAR(process(*processor, 0.8f), 1.0f, 1e-6f);
    connection->dispatchEvents(receive);
    EXPECT_FLOAT_EQ(values.at(7), gain);
    connection->disconnect();
    EXPECT_FALSE(connection->getInstanceHandle());
    EXPECT_TRUE(connection->getFeatures().empty());
    EXPECT_FALSE(connection->writePort(2, sizeof(gain), 0, &gain));
    const auto next = client->createUiConnection({});
    ASSERT_TRUE(next);
    int delivered = 0;
    next->dispatchEvents([&](uint32_t, uint32_t, uint32_t, const void*) { ++delivered; next->disconnect(); });
    EXPECT_EQ(delivered, 1);
    processor->releaseResources();
}

TEST_F(Lv2HostTests, VendorEditorLifecycleClosesBeforeTheInstanceAndFeaturesChange)
{
    const auto processor = load("sample-rate");
    ASSERT_TRUE(processor);
    auto* client = processor->getLV2Client();
    std::shared_ptr<juce::AudioPluginExtensions::LV2Client::UiConnection> connection;
    void* oldInstance = nullptr;
    int before = 0, after = 0;
    connection = client->createUiConnection({ [&] {
            EXPECT_TRUE(juce::MessageManager::getInstance()->isThisTheMessageThread());
            oldInstance = connection->getInstanceHandle();
            EXPECT_TRUE(oldInstance);
            ++before;
        }, [&] {
            EXPECT_TRUE(juce::MessageManager::getInstance()->isThisTheMessageThread());
            EXPECT_NE(connection->getInstanceHandle(), oldInstance);
            EXPECT_FALSE(connection->getFeatures().empty());
            ++after;
        } });
    ASSERT_TRUE(connection);
    for (const auto rate : { 48000.0, 96000.0 }) {
        processor->prepareToPlay(rate, 64);
        EXPECT_EQ(before, after);
        float physical = 0.0f;
        connection->dispatchEvents([&](uint32_t port, uint32_t size, uint32_t, const void* data) {
            EXPECT_EQ(port, 2u);
            ASSERT_EQ(size, sizeof(float));
            std::memcpy(&physical, data, size);
        });
        EXPECT_FLOAT_EQ(physical, rate * 0.25);
        EXPECT_NEAR(process(*processor, 0.8f), 0.2f, 1e-6f);
        processor->releaseResources();
    }
    EXPECT_EQ(before, 2);
    connection->disconnect();
    processor->prepareToPlay(44100.0, 64);
    EXPECT_EQ(before, 2);
    processor->releaseResources();
}

TEST_F(Lv2HostTests, VendorEditorAtomMessagesReachTheProcessorAndReturnToTheCurrentConnection)
{
    const auto processor = load("ui-echo");
    ASSERT_TRUE(processor);
    auto* client = processor->getLV2Client();
    auto connection = client->createUiConnection({});
    ASSERT_TRUE(connection);
    const auto features = connection->getFeatures();
    const auto mapped = std::find_if(features.begin(), features.end(), [](const auto& feature) { return feature.uri == LV2_URID__map; });
    ASSERT_NE(mapped, features.end());
    const auto* map = static_cast<const LV2_URID_Map*>(mapped->data);
    ASSERT_TRUE(map);
    const auto floatType = map->map(map->handle, LV2_ATOM__Float);
    const auto eventTransfer = map->map(map->handle, LV2_ATOM__eventTransfer);
    LV2_Atom_Float message { { sizeof(float), floatType }, 0.75f };
    EXPECT_EQ(connection->getPortIndex("events_in"), 8u);
    EXPECT_FALSE(connection->writePort(9, sizeof(message), eventTransfer, &message));
    EXPECT_FALSE(connection->writePort(8, sizeof(LV2_Atom) - 1, eventTransfer, &message));
    EXPECT_FALSE(connection->writePort(8, sizeof(message), UINT32_MAX, &message));
    ASSERT_TRUE(connection->writePort(8, sizeof(message), eventTransfer, &message));
    processor->prepareToPlay(48000.0, 64);
    process(*processor, 0.8f);
    int atoms = 0;
    const auto receive = [&](uint32_t port, uint32_t size, uint32_t protocol, const void* data) {
        if (protocol == eventTransfer) {
            EXPECT_EQ(port, 9u);
            ASSERT_EQ(size, sizeof(message));
            LV2_Atom_Float value;
            std::memcpy(&value, data, size);
            EXPECT_EQ(value.atom.type, floatType);
            EXPECT_FLOAT_EQ(value.body, 0.75f);
            ++atoms;
        }
    };
    connection->dispatchEvents(receive);
    EXPECT_EQ(atoms, 1);
    ASSERT_TRUE(connection->writePort(8, sizeof(message), eventTransfer, &message));
    process(*processor, 0.8f);
    connection.reset();
    const auto next = client->createUiConnection({});
    ASSERT_TRUE(next);
    next->dispatchEvents(receive);
    EXPECT_EQ(atoms, 1);
    processor->releaseResources();
}

namespace {
struct UiProbeFunctions {
    bool load(const juce::AudioPluginInstance& processor)
    {
        const auto uis = processor.getLV2Client()->getUis();
        if (uis.empty()) {
            return false;
        }
        library.setLoadHints(QLibrary::PreventUnloadHint);
        library.setFileName(QString::fromStdString(uis.front().binaryPath.toStdString()));
        probe = reinterpret_cast<const Lv2UiProbe* (*)()>(library.resolve("tracktion_ui_probe"));
        reset = reinterpret_cast<void (*)()>(library.resolve("tracktion_ui_reset"));
        gain = reinterpret_cast<void (*)(float)>(library.resolve("tracktion_ui_gain"));
        atom = reinterpret_cast<void (*)(float)>(library.resolve("tracktion_ui_atom"));
        close = reinterpret_cast<void (*)()>(library.resolve("tracktion_ui_close"));
        if (!probe || !reset || !gain || !atom || !close) {
            return false;
        }
        reset();
        return true;
    }

    QLibrary library;
    const Lv2UiProbe*(*probe)() = nullptr;
    void (*reset)() = nullptr;
    void (*gain)(float) = nullptr;
    void (*atom)(float) = nullptr;
    void (*close)() = nullptr;
};
}

TEST_F(Lv2HostTests, HostedVendorEditorUsesNativeFeaturesAndReopensForTheNewInstance)
{
    const auto processor = load("ui-echo");
    ASSERT_TRUE(processor);
    UiProbeFunctions api;
    ASSERT_TRUE(api.load(*processor));
    int resized = 0;
    TracktionLv2Ui ui(*processor, { [&](int width, int height) {
            EXPECT_EQ(width, 320);
            EXPECT_EQ(height, 200);
            ++resized;
            return 0;
        }, [] { ADD_FAILURE() << "The editor failed to reopen."; } });
    EXPECT_TRUE(TracktionLv2Ui::isSupported(*processor));
    ASSERT_TRUE(ui.open());
    EXPECT_EQ(api.probe()->opened, 1);
    EXPECT_EQ(resized, 1);
    EXPECT_TRUE(api.probe()->validFeatures);
    EXPECT_FLOAT_EQ(api.probe()->sampleRate, 44100.0f);
    ASSERT_TRUE(ui.idle());
    EXPECT_FLOAT_EQ(api.probe()->gain, 0.5f);
    api.gain(1.5f);
    processor->prepareToPlay(48000.0, 64);
    EXPECT_EQ(api.probe()->opened, 2);
    EXPECT_EQ(api.probe()->cleaned, 1);
    EXPECT_EQ(resized, 2);
    EXPECT_TRUE(api.probe()->validFeatures);
    EXPECT_FLOAT_EQ(api.probe()->sampleRate, 48000.0f);
    EXPECT_NEAR(process(*processor, 0.8f), 1.2f, 1e-6f);
    ASSERT_TRUE(ui.idle());
    EXPECT_FLOAT_EQ(api.probe()->gain, 1.5f);
    api.close();
    EXPECT_FALSE(ui.idle());
    ui.close();
    EXPECT_EQ(api.probe()->cleaned, 2);
    ui.close();
    EXPECT_EQ(api.probe()->cleaned, 2);
    processor->releaseResources();
}

TEST_F(Lv2HostTests, HostedVendorEditorSendsQueuedAtomChangesToTheNewProcessorAndReceivesReplies)
{
    const auto processor = load("ui-echo");
    ASSERT_TRUE(processor);
    UiProbeFunctions api;
    ASSERT_TRUE(api.load(*processor));
    TracktionLv2Ui ui(*processor, {});
    ASSERT_TRUE(ui.open());
    api.atom(0.75f);
    processor->prepareToPlay(48000.0, 64);
    process(*processor, 0.8f);
    ASSERT_TRUE(ui.idle());
    EXPECT_EQ(api.probe()->atomEvents, 1);
    EXPECT_FLOAT_EQ(api.probe()->atomValue, 0.75f);
    processor->releaseResources();
}

TEST_F(Lv2HostTests, HostedExternalEditorRunsAndClosesThroughTheExternalHostFeature)
{
    const auto processor = load("external-ui");
    ASSERT_TRUE(processor);
    UiProbeFunctions api;
    ASSERT_TRUE(api.load(*processor));
    TracktionLv2Ui ui(*processor, {});
    ASSERT_TRUE(ui.open());
    EXPECT_EQ(api.probe()->shown, 1);
    ASSERT_TRUE(ui.idle());
    api.close();
    EXPECT_FALSE(ui.idle());
    ui.close();
    EXPECT_EQ(api.probe()->hidden, 1);
    EXPECT_EQ(api.probe()->cleaned, 1);
}

TEST_F(Lv2HostTests, UnsupportedVendorEditorFeaturesDoNotLeaveAConnectionOrChangeControls)
{
    const auto processor = load("missing-ui-feature");
    ASSERT_TRUE(processor);
    UiProbeFunctions api;
    ASSERT_TRUE(api.load(*processor));
    TracktionLv2Ui ui(*processor, {});
    EXPECT_TRUE(TracktionLv2Ui::isSupported(*processor));
    EXPECT_FALSE(ui.open());
    EXPECT_EQ(api.probe()->opened, 0);
    EXPECT_FLOAT_EQ(*processor->getLV2Client()->getControlPortValue(2), 0.5f);
    const auto connection = processor->getLV2Client()->createUiConnection({});
    ASSERT_TRUE(connection);
    const auto plain = load("gain");
    ASSERT_TRUE(plain);
    EXPECT_FALSE(TracktionLv2Ui::isSupported(*plain));
    TracktionLv2Ui noEditor(*plain, {});
    EXPECT_FALSE(noEditor.open());
}

TEST_F(Lv2HostTests, HostedX11EditorReportsTheNewWindowAfterEachInstanceChange)
{
    const auto processor = load("x11-ui");
    ASSERT_TRUE(processor);
    UiProbeFunctions api;
    ASSERT_TRUE(api.load(*processor));
    int windows = 0;
    TracktionLv2Ui ui(*processor, { {}, {}, [&](void* window) {
            EXPECT_TRUE(window);
            ++windows;
        } });
    ASSERT_TRUE(ui.open());
    EXPECT_EQ(windows, 1);
    processor->prepareToPlay(48000.0, 64);
    EXPECT_EQ(windows, 2);
    EXPECT_EQ(api.probe()->cleaned, 1);
    processor->releaseResources();
}

TEST_F(Lv2HostTests, RestoredStateDiscardsPendingEditorChangesAndFailedRestoreKeepsThem)
{
    const auto processor = load("ui-state");
    ASSERT_TRUE(processor);
    auto* client = processor->getLV2Client();
    juce::MemoryBlock initial;
    processor->getStateInformation(initial);
    const auto connection = client->createUiConnection({});
    ASSERT_TRUE(connection);
    const auto features = connection->getFeatures();
    const auto mapped = std::find_if(features.begin(), features.end(), [](const auto& feature) { return feature.uri == LV2_URID__map; });
    ASSERT_NE(mapped, features.end());
    const auto* map = static_cast<const LV2_URID_Map*>(mapped->data);
    ASSERT_TRUE(map);
    const auto eventTransfer = map->map(map->handle, LV2_ATOM__eventTransfer);
    LV2_Atom_Float message { { sizeof(float), map->map(map->handle, LV2_ATOM__Float) }, 0.25f };
    ASSERT_TRUE(connection->writePort(8, sizeof(message), eventTransfer, &message));
    ASSERT_TRUE(client->restoreState(initial));
    client->processUiIdle();
    processor->prepareToPlay(48000.0, 64);
    EXPECT_NEAR(process(*processor, 0.8f), 0.4f, 1e-6f);
    processor->releaseResources();
    ASSERT_TRUE(connection->writePort(8, sizeof(message), eventTransfer, &message));
    EXPECT_FALSE(client->restoreState(juce::MemoryBlock("invalid", 7)));
    client->processUiIdle();
    processor->prepareToPlay(48000.0, 64);
    EXPECT_NEAR(process(*processor, 0.8f), 0.1f, 1e-6f);
    processor->releaseResources();
}
}
