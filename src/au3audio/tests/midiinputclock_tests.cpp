/*
* Audacity: A Digital Audio Editor
*/
#include <gtest/gtest.h>
#include <QCoreApplication>
#include <algorithm>
#include <array>
#include <chrono>
#include <thread>
#include <mutex>
#include <atomic>
#include <portaudio.h>
#include "au3-utility/MemoryX.h"

#include "async/processevents.h"
#include "midi/imidiinport.h"
#include "../internal/liveinstrumentsession.h"

#ifdef Q_OS_MAC
#include <CoreAudio/HostTime.h>
#include <CoreMIDI/CoreMIDI.h>
#include "midi/internal/platform/osx/coremidiinport.h"
#endif

namespace au::au3audio {
namespace {
using Clock = std::chrono::steady_clock;
using InputPort = muse::midi::IMidiInPort;

class TestInput final : public InputPort
{
public:
    explicit TestInput(bool timestamps, bool realtime = false)
        : timed(timestamps), direct(realtime) {}
    muse::midi::MidiDeviceList availableDevices() const override { return { { "clock-test", "Clock test" } }; }
    muse::async::Notification availableDevicesChanged() const override { return devicesChanged; }
    muse::Ret connect(const muse::midi::MidiDeviceID& id) override
    {
        if (isConnected()) {
            disconnect();
        }
        device = id;
        changed.notify();
        return muse::make_ok();
    }

    void disconnect() override
    {
        {
            std::lock_guard lock(mutex);
            if (sink) {
                sink->lost(Clock::now());
            }
            sink.reset();
        } device.clear();
        changed.notify();
    }

    bool isConnected() const override { return !device.empty(); }
    muse::midi::MidiDeviceID deviceID() const override { return device; }
    muse::async::Notification deviceChanged() const override { return changed; }
    muse::async::Channel<muse::midi::tick_t, muse::midi::Event> eventReceived() const override { return legacy; }
    std::optional<muse::async::Channel<InputTime, muse::midi::Event> > timestampedEventReceived() const override
    { return timed ? std::make_optional(events) : std::nullopt; }
    bool setRealtimeReceiver(const std::shared_ptr<RealtimeReceiver>& receiver) override
    {
        if (!direct) {
            return false;
        }
        std::lock_guard lock(mutex);
        if (sink && sink != receiver) {
            sink->lost(Clock::now());
        }
        sink = receiver;
        return true;
    }

    void clearRealtimeReceiver(const std::shared_ptr<RealtimeReceiver>& receiver) override
    {
        std::lock_guard lock(mutex);
        if (sink != receiver) {
            return;
        }
        sink->lost(Clock::now());
        sink.reset();
    }

    void send(InputTime time, const muse::midi::Event& event)
    {
        {
            std::lock_guard lock(mutex);
            if (sink) {
                sink->receive(time, event);
            }
        }
        events.send(time, event);
        legacy.send(0, event);
    }

    bool timed, direct;
    std::mutex mutex;
    std::shared_ptr<RealtimeReceiver> sink;
    std::string device;
    muse::async::Notification changed, devicesChanged;
    muse::async::Channel<muse::midi::tick_t, muse::midi::Event> legacy;
    muse::async::Channel<InputTime, muse::midi::Event> events;
};

class InputRegistration final
{
public:
    explicit InputRegistration(const std::shared_ptr<InputPort>& port)
    { muse::modularity::globalIoc()->registerExport<InputPort>("midi-clock-tests", port); }
    ~InputRegistration() { muse::modularity::globalIoc()->unregister<InputPort>("midi-clock-tests"); }
};

const auto noteOn = muse::midi::Event::fromMidi10Package(0x00643c90);
const auto noteOff = muse::midi::Event::fromMidi10Package(0x00003c80);

std::vector<audio::InstrumentMidiMessage> receive(LiveInstrumentSession& session, size_t count)
{
    std::vector<audio::InstrumentMidiMessage> result;
    const auto deadline = Clock::now() + std::chrono::seconds(5);
    while (result.size() < count && Clock::now() < deadline) {
        QCoreApplication::processEvents();
        muse::async::processMessages();
        auto messages = session.takeMidiMessages();
        result.insert(result.end(), messages.begin(), messages.end());
        if (result.size() < count) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
    return result;
}

double seconds(Clock::time_point time) { return std::chrono::duration<double>(time.time_since_epoch()).count(); }

class InputVoices final : public InstrumentProcessor
{
public:
    void syncLiveMidiControllers(std::span<const audio::InstrumentMidiMessage> messages) override
    {
        controllerThread = std::this_thread::get_id();
        controllerInput.insert(controllerInput.end(), messages.begin(), messages.end());
    }

    std::thread::id controllerThread;
    std::vector<audio::InstrumentMidiMessage> controllerInput;
    size_t latency() const override { return cachedLatency; }
    bool latencyUpdatePending() const noexcept override { return requestedLatency != cachedLatency; }
    bool refreshLatency() override
    {
        const bool changed = latencyUpdatePending();
        cachedLatency = requestedLatency;
        return changed;
    }

    void prepare(double, size_t) override { reset(); }
    void reset() override
    {
        keys = {};
        sustained = {};
        pedal = {};
    }

    std::vector<audio::InstrumentParameter> parameters() const override { return {}; }
    bool setParameter(const std::string&, double) override { return false; }
    std::string state() const override { return {}; }
    void process(float* const* output, size_t channels, size_t frames, std::span<const MidiEvent> midi,
                 const InstrumentProcessContext&) override
    {
        for (const auto& event : midi) {
            const auto channel = event.status & 15;
            const auto type = event.status & 0xf0;
            if (type == 0xb0 && event.data1 < 120) {
                controllers[channel][event.data1] = event.data2;
            }
            if (type == 0xe0) {
                pitch[channel] = event.data1 | (event.data2 << 7);
            }
            if (type == 0x90 && event.data2) {
                keys[channel][event.data1] = true;
                sustained[channel][event.data1] = false;
                ++ons[channel][event.data1];
            } else if (type == 0x80 || type == 0x90) {
                sustained[channel][event.data1] = pedal[channel] && keys[channel][event.data1];
                keys[channel][event.data1] = false;
                ++offs[channel][event.data1];
            } else if (type == 0xb0 && (event.data1 == 64 || event.data1 == 121)) {
                pedal[channel] = event.data1 == 64 && event.data2 >= 64;
                if (!pedal[channel]) {
                    sustained[channel] = {};
                }
            } else if (type == 0xb0 && (event.data1 == 120 || event.data1 == 123)) {
                keys[channel] = {};
                sustained[channel] = {};
            }
        }
        float voices = 0.0f;
        for (size_t channel = 0; channel < 16; ++channel) {
            for (size_t note = 0; note < 128; ++note) {
                voices += keys[channel][note] || sustained[channel][note];
            }
        }
        for (size_t channel = 0; channel < channels; ++channel) {
            std::fill_n(output[channel], frames, voices);
        }
    }

    std::array<std::array<bool, 128>, 16> keys {}, sustained {};
    std::array<std::array<int, 128>, 16> ons {}, offs {};
    std::array<bool, 16> pedal {};
    std::array<std::array<int, 128>, 16> controllers {};
    std::array<int, 16> pitch {};
    size_t cachedLatency = 0, requestedLatency = 0;
};

class InputOwnershipTests : public testing::TestWithParam<bool>
{
protected:
    InputOwnershipTests()
        : registration(port), session(MidiTrack::Create(), voices) {}
    void SetUp() override { session.prepare(48000.0, 32); ASSERT_TRUE(session.setMidiInput("clock-test")); }
    void device(int status, int note, int value)
    {
        port->send(Clock::now(), muse::midi::Event::fromMidi10Package(status | (note << 8) | (value << 16)));
    }

    float render(std::span<const MidiEvent> timeline = {})
    {
        std::array<float, 32> samples {};
        float* output[] { samples.data() };
        session.process(output, 1, samples.size(), timeline, {});
        return samples.front();
    }

    std::shared_ptr<TestInput> port = std::make_shared<TestInput>(true, GetParam());
    InputRegistration registration;
    std::shared_ptr<InputVoices> voices = std::make_shared<InputVoices>();
    LiveInstrumentSession session;
};
}

TEST_P(InputOwnershipTests, NativeControllerReceivesHardwareInputOnMainWithoutDocumentEvents)
{
    std::thread driver([&] {
        device(0xb0, 74, 65);
        device(0xb0, 74, 65); // Relative encoders may repeat the same value.
        device(0xb1, 7, 90);
    });
    driver.join();
    EXPECT_TRUE(voices->controllerInput.empty());
    const auto messages = receive(session, 3);
    ASSERT_EQ(messages.size(), 3u);
    ASSERT_EQ(voices->controllerInput.size(), 3u);
    EXPECT_EQ(voices->controllerThread, std::this_thread::get_id());
    EXPECT_EQ(voices->controllerInput[0].data2, 65);
    EXPECT_EQ(voices->controllerInput[1].data2, 65);
    EXPECT_EQ(voices->controllerInput[2].status, 0xb1);
    const std::array document { MidiEvent { 0, 0xb0, 74, 25 } };
    render(document);
    EXPECT_TRUE(session.takeMidiMessages().empty());
    EXPECT_EQ(voices->controllerInput.size(), 3u);
}

TEST_P(InputOwnershipTests, SamePitchFromBothInputsReleasesOnlyWhenBothKeysAreUp)
{
    for (const bool deviceFirst : { false, true }) {
        if (deviceFirst) {
            device(0x90, 60, 100);
        } else {
            session.sendMidi(0x90, 60, 95);
        }
        EXPECT_EQ(render(), 1.0f);
        if (deviceFirst) {
            session.sendMidi(0x90, 60, 95);
        } else {
            device(0x90, 60, 100);
        }
        EXPECT_EQ(render(), 1.0f);
        const auto offs = voices->offs[0][60];
        if (deviceFirst) {
            device(0x90, 60, 0);              // MIDI zero-velocity note-on is note-off.
        } else {
            session.sendMidi(0x80, 60, 0);
        }
        EXPECT_EQ(render(), 1.0f);
        EXPECT_EQ(voices->offs[0][60], offs);
        if (deviceFirst) {
            session.sendMidi(0x80, 60, 0);
        } else {
            device(0x80, 60, 0);
        }
        EXPECT_EQ(render(), 0.0f);
        EXPECT_EQ(voices->offs[0][60], offs + 1);
    }
    EXPECT_EQ(voices->ons[0][60], 2);
}

TEST_P(InputOwnershipTests, ChangingOrDisconnectingADeviceReleasesOnlyItsNotesAndRecordsTheBoundary)
{
    session.sendMidi(0x90, 60, 95);
    device(0x90, 60, 100);
    device(0x91, 60, 110);
    device(0x90, 64, 120);
    EXPECT_EQ(render(), 3.0f);
    const auto input = session.takeMidiMessages();
    ASSERT_EQ(input.size(), 3u);
    for (const auto& message : input) {
        EXPECT_EQ(message.source, audio::InstrumentMidiMessage::Source::MidiDevice);
    }
    ASSERT_TRUE(session.setMidiInput({}));
    EXPECT_EQ(render(), 1.0f);
    const auto releases = session.takeMidiMessages();
    ASSERT_EQ(releases.size(), 3u);
    for (const auto& message : releases) {
        EXPECT_EQ(message.status & 0xf0, 0x80);
        EXPECT_EQ(message.source, audio::InstrumentMidiMessage::Source::MidiDevice);
    }
    ASSERT_TRUE(session.setMidiInput("clock-test"));
    device(0x90, 67, 100);
    EXPECT_EQ(render(), 2.0f);
    port->disconnect();
    EXPECT_EQ(render(), 1.0f);
    session.sendMidi(0x80, 60, 0);
    EXPECT_EQ(render(), 0.0f);
}

TEST_P(InputOwnershipTests, SourceChannelModesAndDeviceLossPreserveTheOtherInputsSustain)
{
    session.sendMidi(0xb0, 64, 127);
    session.sendMidi(0x90, 60, 95);
    session.sendMidi(0x80, 60, 0);
    device(0xb0, 64, 127);
    device(0x90, 67, 100);
    EXPECT_EQ(render(), 2.0f);
    device(0xb0, 121, 0);
    EXPECT_EQ(render(), 2.0f);
    EXPECT_TRUE(voices->sustained[0][60]);
    device(0xb0, 123, 0);
    EXPECT_EQ(render(), 2.0f); // Shared channel is still sustained by the computer input.
    port->disconnect();
    EXPECT_EQ(render(), 2.0f);
    session.sendMidi(0xb0, 64, 0);
    EXPECT_EQ(render(), 0.0f);
}

TEST_P(InputOwnershipTests, DeviceLossDropsItsOwnPedalAndCannotLeaveAStuckVoice)
{
    device(0xb0, 64, 127);
    device(0x90, 60, 100);
    device(0x80, 60, 0);
    EXPECT_EQ(render(), 1.0f);
    port->disconnect();
    EXPECT_EQ(render(), 0.0f);
    const auto messages = session.takeMidiMessages();
    ASSERT_EQ(messages.size(), 4u);
    EXPECT_EQ(messages.back().status, 0xb0);
    EXPECT_EQ(messages.back().data1, 64);
    EXPECT_EQ(messages.back().data2, 0);
}

TEST_P(InputOwnershipTests, AllNotesOffRetainsTheDevicePedalWhileAllSoundOffSilencesAnUnsharedChannel)
{
    device(0xb0, 64, 127);
    device(0x90, 60, 100);
    EXPECT_EQ(render(), 1.0f);
    device(0xb0, 123, 0);
    EXPECT_EQ(render(), 1.0f);
    EXPECT_TRUE(voices->sustained[0][60]);
    device(0xb0, 120, 0);
    EXPECT_EQ(render(), 0.0f);
    EXPECT_TRUE(voices->pedal[0]);
    device(0x90, 60, 100);
    device(0x80, 60, 0);
    EXPECT_EQ(render(), 1.0f);
    device(0xb0, 64, 0);
    EXPECT_EQ(render(), 0.0f);
}

TEST_P(InputOwnershipTests, AnotherSessionTakingTheGlobalDriverReleasesOnlyTheOldDeviceInput)
{
    session.sendMidi(0x90, 60, 95);
    device(0x90, 64, 100);
    EXPECT_EQ(render(), 2.0f);
    session.takeMidiMessages();
    ASSERT_TRUE(port->connect("other-session"));
    EXPECT_EQ(port->deviceID(), "other-session");
    EXPECT_EQ(render(), 1.0f);
    device(0x90, 67, 100);
    EXPECT_EQ(render(), 1.0f); // The retired subscription cannot consume the new session's notes.
    const auto messages = session.takeMidiMessages();
    ASSERT_EQ(messages.size(), 1u);
    EXPECT_EQ(messages[0].status, 0x80);
    EXPECT_EQ(messages[0].data1, 64);
    ASSERT_TRUE(session.setMidiInput("clock-test"));
    device(0x90, 67, 100);
    EXPECT_EQ(render(), 2.0f);
}

TEST_P(InputOwnershipTests, DeviceChannelModesCannotReleaseComputerOrTimelineKeysSharingTheChannel)
{
    session.beginTimeline();
    for (const int mode : { 120, 123, 124, 125, 126, 127 }) {
        SCOPED_TRACE(mode);
        session.sendMidi(0x90, 60, 95);
        device(0x90, 60, 100);
        device(0x90, 67, 100);
        const std::array timeline { MidiEvent { 0, 0x90, 64, 100 } };
        EXPECT_EQ(render(timeline), 3.0f);
        device(0xb0, mode, 0);
        EXPECT_EQ(render(), 2.0f);
        EXPECT_TRUE(voices->keys[0][60]);
        EXPECT_TRUE(voices->keys[0][64]);
        EXPECT_FALSE(voices->keys[0][67]);
        session.sendMidi(0x80, 60, 0);
        const std::array off { MidiEvent { 0, 0x80, 64, 0 } };
        EXPECT_EQ(render(off), 0.0f);
    }
}

TEST_P(InputOwnershipTests, ControllerResetRestoresTheOtherInputsControlsAndRetainsVolumeAndPan)
{
    session.sendMidi(0xb0, 0, 3);
    session.sendMidi(0xb0, 7, 64);
    session.sendMidi(0xb0, 10, 37);
    session.sendMidi(0xb0, 1, 50);
    session.sendMidi(0xb0, 11, 80);
    session.sendMidi(0xe0, 0, 80);
    session.sendMidi(0xb0, 64, 127);
    session.sendMidi(0x90, 60, 100);
    session.sendMidi(0x80, 60, 0);
    ASSERT_EQ(render(), 1.0f);
    device(0xb0, 1, 90);
    device(0xb0, 11, 32);
    device(0xe0, 0, 96);
    device(0xb0, 7, 88);
    ASSERT_EQ(render(), 1.0f);
    EXPECT_EQ(voices->controllers[0][11], 32);
    EXPECT_EQ(voices->pitch[0], 96 * 128);
    device(0xb0, 121, 0);
    EXPECT_EQ(render(), 1.0f);
    EXPECT_EQ(voices->controllers[0][0], 3);
    EXPECT_EQ(voices->controllers[0][7], 88);
    EXPECT_EQ(voices->controllers[0][10], 37);
    EXPECT_EQ(voices->controllers[0][1], 50);
    EXPECT_EQ(voices->controllers[0][11], 80);
    EXPECT_EQ(voices->controllers[0][64], 127);
    EXPECT_EQ(voices->pitch[0], 80 * 128);
    EXPECT_TRUE(voices->sustained[0][60]);
    session.sendMidi(0xb0, 64, 0);
    EXPECT_EQ(render(), 0.0f);
}

TEST_P(InputOwnershipTests, ResettingOneSourceKeepsTheLatestControllerFromTheOtherSourceOrTimeline)
{
    session.beginTimeline();
    session.sendMidi(0xb0, 11, 80);
    session.sendMidi(0xe0, 0, 80);
    ASSERT_EQ(render(), 0);
    device(0xb0, 11, 32);
    device(0xe0, 0, 96);
    ASSERT_EQ(render(), 0);
    const std::array timeline { MidiEvent { 3, 0xb0, 11, 16 }, MidiEvent { 3, 0xe0, 0, 100 } };
    render(timeline);
    device(0xb0, 121, 0);
    render();
    EXPECT_EQ(voices->controllers[0][11], 16);
    EXPECT_EQ(voices->pitch[0], 100 * 128);
    // Once the timeline releases its overrides, the computer's retained
    // settings return. The retired device value must not become live state.
    session.resetTimeline();
    render();
    EXPECT_EQ(voices->controllers[0][11], 80);
    EXPECT_EQ(voices->pitch[0], 80 * 128);
}

INSTANTIATE_TEST_SUITE_P(QueuedAndRealtime, InputOwnershipTests, testing::Bool());

TEST(NativeMidiInputClockTests, QuiescentLatencyPublicationPreservesPendingDriverAudioAndExactRecordingTimes)
{
    const auto port = std::make_shared<TestInput>(true, true);
    const InputRegistration registration(port);
    const auto voices = std::make_shared<InputVoices>();
    LiveInstrumentSession session(MidiTrack::Create(), voices);
    session.prepare(48000.0, 32);
    ASSERT_TRUE(session.setMidiInput("clock-test"));
    std::array<float, 32> samples {};
    float* output[] { samples.data() };
    session.sendMidi(0x90, 60, 95);
    session.process(output, 1, 32, {}, {});
    EXPECT_FLOAT_EQ(samples.front(), 1.0f);
    const auto first = Clock::now(), second = first + std::chrono::milliseconds(2);
    std::thread driver([&] {
        port->send(first, muse::midi::Event::fromMidi10Package(0x00644390));
        port->send(second, muse::midi::Event::fromMidi10Package(0x00644090));
    });
    driver.join();
    voices->requestedLatency = 17;
    ASSERT_TRUE(session.latencyUpdatePending());
    EXPECT_TRUE(session.refreshLatency());
    EXPECT_EQ(session.latency(), 17u);
    const auto recorded = session.takeMidiMessages();
    ASSERT_EQ(recorded.size(), 2u);
    EXPECT_EQ(recorded[0].data1, 67);
    EXPECT_EQ(recorded[1].data1, 64);
    EXPECT_DOUBLE_EQ(recorded[0].time, seconds(first));
    EXPECT_DOUBLE_EQ(recorded[1].time, seconds(second));
    session.process(output, 1, 32, {}, {});
    EXPECT_FLOAT_EQ(samples.front(), 3.0f);
    for (const int key : { 60, 64, 67 }) {
        EXPECT_EQ(voices->ons[0][key], 1);
    }
    EXPECT_TRUE(session.takeMidiMessages().empty());
}

#ifdef AU_NATIVE_TEST_INSTRUMENT_PATH
TEST(NativeMidiInputClockTests, NativeVstReactivationRetainsQueuedDriverKeysAndTheirIndependentRecordingMessages)
{
    const auto port = std::make_shared<TestInput>(true, true);
    const InputRegistration registration(port);
    const auto processor = createVstInstrument(std::string(AU_NATIVE_TEST_INSTRUMENT_PATH) + ";F5A81297A6724B218E434FA1A4050C04", {});
    LiveInstrumentSession session(MidiTrack::Create(), processor);
    session.prepare(48000, 64);
    ASSERT_TRUE(session.setMidiInput("clock-test"));
    std::array<float, 64> samples {};
    float* output[] { samples.data() };
    session.sendMidi(0x90, 60, 127);
    session.process(output, 1, 64, {}, {});
    ASSERT_FLOAT_EQ(samples.back(), 0.5f);
    const auto first = Clock::now(), second = first + std::chrono::milliseconds(2);
    std::thread driver([&] {
        port->send(first, muse::midi::Event::fromMidi10Package(0x007f4390));
        port->send(second, muse::midi::Event::fromMidi10Package(0x007f4090));
    });
    driver.join();
    ASSERT_TRUE(session.setParameter("2", 7.0 / 31));
    ASSERT_TRUE(session.refreshLatency());
    const auto recorded = session.takeMidiMessages();
    ASSERT_EQ(recorded.size(), 2u);
    EXPECT_EQ(recorded[0].data1, 67);
    EXPECT_EQ(recorded[1].data1, 64);
    EXPECT_DOUBLE_EQ(recorded[0].time, seconds(first));
    EXPECT_DOUBLE_EQ(recorded[1].time, seconds(second));
    session.process(output, 1, 64, {}, {});
    for (size_t frame = 0; frame < 64; ++frame) {
        EXPECT_NEAR(samples[frame], frame < 7 ? 0.0 : 1.5, 1e-6) << frame;
    }
    EXPECT_TRUE(session.takeMidiMessages().empty());
}
#endif

TEST(NativeMidiInputClockTests, DriverTimeSurvivesUiDeliveryDelayWithoutDuplicatingLegacyEvents)
{
    auto port = std::make_shared<TestInput>(true);
    InputRegistration registration(port);
    LiveInstrumentSession session(MidiTrack::Create(), createBuiltinInstrument({}));
    ASSERT_TRUE(session.setMidiInput("clock-test"));
    const auto on = Clock::now() - std::chrono::milliseconds(250);
    const auto off = on + std::chrono::milliseconds(150);
    std::thread driver([&] {
        port->events.send(on, noteOn);
        port->legacy.send(123, noteOn);
        port->events.send(off, noteOff);
        port->legacy.send(456, noteOff);
    });
    driver.join();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    const auto messages = receive(session, 2);
    ASSERT_EQ(messages.size(), 2u);
    EXPECT_DOUBLE_EQ(messages[0].time, seconds(on));
    EXPECT_DOUBLE_EQ(messages[1].time, seconds(off));
    EXPECT_EQ(messages[0].status, 0x90);
    EXPECT_EQ(messages[1].status, 0x80);
    EXPECT_EQ(messages[0].data1, 60);
    EXPECT_EQ(messages[0].data2, 100);
    EXPECT_TRUE(session.setMidiInput({}));
    port->events.send(Clock::now(), noteOn);
    EXPECT_TRUE(session.takeMidiMessages().empty());
}

TEST(NativeMidiInputClockTests, LegacyDriversRetainArrivalClockWithoutInterpretingPlatformTickUnits)
{
    auto port = std::make_shared<TestInput>(false);
    InputRegistration registration(port);
    LiveInstrumentSession session(MidiTrack::Create(), createBuiltinInstrument({}));
    ASSERT_TRUE(session.setMidiInput("clock-test"));
    const auto begin = seconds(Clock::now());
    port->legacy.send(0xfefefefe, noteOn);
    const auto messages = receive(session, 1);
    ASSERT_EQ(messages.size(), 1u);
    EXPECT_GE(messages[0].time, begin);
    EXPECT_LE(messages[0].time, seconds(Clock::now()));
}

TEST(NativeMidiInputClockTests, SelectingTheSameInputReconnectsAfterTheDriverLostItsDevice)
{
    auto port = std::make_shared<TestInput>(true);
    InputRegistration registration(port);
    LiveInstrumentSession session(MidiTrack::Create(), createBuiltinInstrument({}));
    ASSERT_TRUE(session.setMidiInput("clock-test"));
    port->disconnect();
    ASSERT_FALSE(port->isConnected());
    ASSERT_TRUE(session.setMidiInput("clock-test"));
    EXPECT_TRUE(port->isConnected());
    EXPECT_EQ(port->deviceID(), "clock-test");
    // Reconnecting replaces the subscription instead of delivering a note twice.
    port->events.send(Clock::now(), noteOn);
    const auto messages = receive(session, 1);
    ASSERT_EQ(messages.size(), 1u);
    EXPECT_EQ(messages[0].data1, 60);
}

TEST(NativeMidiInputClockTests, RealtimeInputSoundsAndStopsOnTheAudioThreadWhileUiDeliveryIsBlocked)
{
    auto port = std::make_shared<TestInput>(true, true);
    InputRegistration registration(port);
    auto voices = std::make_shared<InputVoices>();
    LiveInstrumentSession session(MidiTrack::Create(), voices);
    session.prepare(48000.0, 32);
    ASSERT_TRUE(session.setMidiInput("clock-test"));
    std::atomic<bool> run { true };
    std::atomic<int64_t> heardOn { 0 }, heardOff { 0 };
    std::thread audio([&] {
        std::array<float, 32> samples {};
        float* output[] { samples.data() };
        while (run) {
            session.process(output, 1, samples.size(), {}, {});
            const auto nanos = std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count();
            if (samples[0] > 0 && !heardOn) {
                heardOn = nanos;
            }
            if (heardOn && samples[0] == 0 && !heardOff) {
                heardOff = nanos;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    });
    Clock::time_point on, off;
    std::thread driver([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        on = Clock::now();
        port->send(on, noteOn);
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        off = Clock::now();
        port->send(off, noteOff);
    });
    // No QCoreApplication/processMessages calls: neither DSP nor recording
    // may need a queued UI notification in order to observe this phrase.
    std::this_thread::sleep_for(std::chrono::milliseconds(700));
    driver.join();
    run = false;
    audio.join();
    ASSERT_NE(heardOn.load(), 0);
    ASSERT_NE(heardOff.load(), 0);
    EXPECT_GE(heardOn.load() * 1e-9 - seconds(on), -0.001);
    EXPECT_LT(heardOn.load() * 1e-9 - seconds(on), 0.05);
    EXPECT_LT(heardOff.load() * 1e-9 - seconds(off), 0.05);
    const auto recorded = session.takeMidiMessages();
    ASSERT_EQ(recorded.size(), 2u);
    EXPECT_DOUBLE_EQ(recorded[0].time, seconds(on));
    EXPECT_DOUBLE_EQ(recorded[1].time, seconds(off));
    EXPECT_EQ(voices->ons[0][60], 1);
    EXPECT_EQ(voices->offs[0][60], 1);
    // Pumping the legacy channels later must not replay the phrase.
    QCoreApplication::processEvents();
    muse::async::processMessages();
    EXPECT_TRUE(session.takeMidiMessages().empty());
}

TEST(NativeMidiInputClockTests, RealtimeOverflowReleasesVoicesReportsLostInputAndReconnectsWithoutStaleNotes)
{
    for (const bool consumeAudio : { false, true }) {
        SCOPED_TRACE(consumeAudio ? "UI recording queue backpressure" : "Audio queue backpressure");
        auto port = std::make_shared<TestInput>(true, true);
        InputRegistration registration(port);
        auto voices = std::make_shared<InputVoices>();
        LiveInstrumentSession session(MidiTrack::Create(), voices);
        session.prepare(48000.0, 32);
        ASSERT_TRUE(session.setMidiInput("clock-test"));
        const auto render = [&] {
            std::array<float, 32> samples {};
            float* output[] { samples.data() };
            session.process(output, 1, samples.size(), {}, {});
            return samples.front();
        };
        session.sendMidi(0x90, 64, 90);
        port->send(Clock::now(), noteOn);
        ASSERT_EQ(render(), 2.0f);
        // Exercise both limits: no consumer fills the audio queue; consuming
        // audio while the UI stays busy eventually fills the recording queue.
        for (int i = 0; i < (consumeAudio ? 20000 : 5000); ++i) {
            port->send(Clock::now(), noteOn);
            if (consumeAudio) {
                render();
            }
        }
        EXPECT_EQ(render(), 1.0f); // Only the computer's note survives.
        const auto partial = session.takeMidiMessages();
        ASSERT_FALSE(partial.empty());
        EXPECT_EQ(partial.back().status, 0x80);
        EXPECT_EQ(partial.back().data1, 60);
        EXPECT_FALSE(session.takeMidiInputError().empty());
        EXPECT_TRUE(session.takeMidiInputError().empty());
        ASSERT_TRUE(session.setMidiInput("clock-test"));
        EXPECT_EQ(render(), 1.0f);
        port->send(Clock::now(), noteOn);
        EXPECT_EQ(render(), 2.0f);
        port->send(Clock::now(), noteOff);
        EXPECT_EQ(render(), 1.0f);
        EXPECT_TRUE(session.takeMidiInputError().empty());
    }
}

#ifdef Q_OS_MAC
namespace {
struct VirtualSource {
    MIDIClientRef client = 0;
    MIDIEndpointRef source = 0;
    ~VirtualSource()
    {
        if (source) {
            MIDIEndpointDispose(source);
        }
        if (client) {
            MIDIClientDispose(client);
        }
    }
};
class ScopedCoreInput final : public muse::midi::CoreMidiInPort
{
public:
    ScopedCoreInput() { init(); }
    ~ScopedCoreInput() override { deinit(); }
};
}

TEST(NativeMidiInputClockTests, RealCoreMidiReachesThePhysicalOutputCallbackDuringAUiStall)
{
    if (qEnvironmentVariableIsEmpty("AU_NATIVE_TEST_AUDIO_HOST")) {
        GTEST_SKIP() << "Opt-in physical output test";
    }
    ASSERT_EQ(Pa_Initialize(), paNoError);
    const auto terminate = finally([] { Pa_Terminate(); });
    auto device = Pa_GetDefaultOutputDevice();
    const auto outputName = qgetenv("AU_NATIVE_TEST_AUDIO_OUTPUT_DEVICE");
    if (!outputName.isEmpty()) {
        device = paNoDevice;
        for (int index = 0; index < Pa_GetDeviceCount(); ++index) {
            const auto* candidate = Pa_GetDeviceInfo(index);
            if (candidate && candidate->maxOutputChannels >= 2 && outputName == candidate->name) {
                ASSERT_EQ(device, paNoDevice) << "Output device name must be unique";
                device = index;
            }
        }
    }
    ASSERT_NE(device, paNoDevice);
    const auto* info = Pa_GetDeviceInfo(device);
    ASSERT_NE(info, nullptr);
    ASSERT_GE(info->maxOutputChannels, 2);
    bool valid = false;
    const auto latencySetting = qgetenv("AU_NATIVE_TEST_AUDIO_OUTPUT_LATENCY_MS");
    const auto latencyMs = latencySetting.isEmpty() ? 10.0 : latencySetting.toDouble(&valid);
    ASSERT_TRUE(latencySetting.isEmpty() || valid);
    ASSERT_GE(latencyMs, 0.0);
    ASSERT_LE(latencyMs, 1000.0);
    const auto frameSetting = qgetenv("AU_NATIVE_TEST_AUDIO_OUTPUT_FRAMES");
    const auto frames = frameSetting.isEmpty() ? 128UL : frameSetting.toULong(&valid);
    ASSERT_TRUE(frameSetting.isEmpty() || valid);
    ASSERT_LE(frames, 8192UL);
    VirtualSource source;
    ASSERT_EQ(MIDIClientCreate(CFSTR("Audacity realtime output test"), nullptr, nullptr, &source.client), noErr);
    ASSERT_EQ(MIDISourceCreate(source.client, CFSTR("Audacity realtime output test"), &source.source), noErr);
    SInt32 id = 0;
    ASSERT_EQ(MIDIObjectGetIntegerProperty(source.source, kMIDIPropertyUniqueID, &id), noErr);
    auto port = std::make_shared<ScopedCoreInput>();
    InputRegistration registration(port);
    auto voices = std::make_shared<InputVoices>();
    LiveInstrumentSession session(MidiTrack::Create(), voices);
    session.Prepare(info->defaultSampleRate);
    ASSERT_TRUE(session.setMidiInput(std::to_string(id)));
    struct Callback {
        LiveInstrumentSession* session;
        std::atomic<int64_t> on { 0 }, off { 0 };
        std::atomic<size_t> blocks { 0 };
        std::atomic<unsigned long> maximumFrames { 0 };
        static int render(const void*, void* rawOutput, unsigned long frames,
                          const PaStreamCallbackTimeInfo*, PaStreamCallbackFlags, void* user)
        {
            auto& self = *static_cast<Callback*>(user);
            auto* output = static_cast<float*>(rawOutput);
            self.maximumFrames.store(std::max(self.maximumFrames.load(), frames));
            self.session->Render(output, 2, frames);
            const auto now = std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count();
            const bool note = std::any_of(output, output + frames * 2, [](float x) { return x != 0; });
            if (note && !self.on) {
                self.on = now;
            }
            if (!note && self.on && !self.off) {
                self.off = now;
            }
            // Event delivery is measured at the real DSP/output callback. Keep
            // this diagnostic probe near-silent; it is not an acoustic test.
            for (size_t i = 0; i < frames * 2; ++i) {
                output[i] *= 0.00001f;
            }
            ++self.blocks;
            return paContinue;
        }
    } callback { &session };
    PaStream* stream = nullptr;
    const PaStreamParameters output { device, 2, paFloat32, latencyMs / 1000.0, nullptr };
    ASSERT_EQ(Pa_OpenStream(&stream, nullptr, &output, info->defaultSampleRate, frames, paNoFlag, Callback::render, &callback), paNoError);
    const auto close = finally([&] { Pa_CloseStream(stream); });
    ASSERT_EQ(Pa_StartStream(stream), paNoError);
    const auto stop = finally([&] {
        if (Pa_IsStreamActive(stream) == 1) {
            Pa_StopStream(stream);
        }
    });
    const auto outputLatency = Pa_GetStreamInfo(stream)->outputLatency;
    Clock::time_point on, off;
    std::atomic<int> sendError { noErr };
    std::thread driver([&] {
        const auto send = [&](const Byte* bytes) {
            MIDIPacketList packets {};
            auto* packet = MIDIPacketListInit(&packets);
            MIDIPacketListAdd(&packets, sizeof(packets), packet, AudioGetCurrentHostTime(), 3, bytes);
            const auto err = MIDIReceived(source.source, &packets);
            if (err != noErr) {
                sendError = err;
            }
        };
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        const Byte start[] { 0x90, 60, 100 }, end[] { 0x80, 60, 0 };
        on = Clock::now();
        send(start);
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        off = Clock::now();
        send(end);
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(700)); // No UI/event processing.
    driver.join();
    ASSERT_EQ(Pa_StopStream(stream), paNoError);
    ASSERT_EQ(sendError.load(), noErr);
    ASSERT_GT(callback.blocks.load(), 10u);
    ASSERT_NE(callback.on.load(), 0);
    ASSERT_NE(callback.off.load(), 0);
    const auto onDelay = callback.on.load() * 1e-9 - seconds(on);
    const auto offDelay = callback.off.load() * 1e-9 - seconds(off);
    std::cout << "CoreMIDI -> physical output callback: device=" << info->name
              << ", rate=" << info->defaultSampleRate << ", maximum callback frames=" << callback.maximumFrames.load()
              << ", on=" << onDelay * 1000
              << " ms, off=" << offDelay * 1000 << " ms, output device latency=" << outputLatency * 1000 << " ms\n";
    EXPECT_GE(onDelay, -0.001);
    EXPECT_LT(onDelay, 0.05);
    EXPECT_LT(offDelay, 0.05);
    const auto recorded = session.takeMidiMessages();
    ASSERT_EQ(recorded.size(), 2u);
    EXPECT_NEAR(recorded[0].time, seconds(on), 0.003);
    EXPECT_NEAR(recorded[1].time, seconds(off), 0.003);
    EXPECT_EQ(voices->ons[0][60], 1);
    EXPECT_EQ(voices->offs[0][60], 1);
    QCoreApplication::processEvents();
    muse::async::processMessages();
    EXPECT_TRUE(session.takeMidiMessages().empty());
}

TEST(NativeMidiInputClockTests, RemovingARealCoreMidiSourceReleasesItsPedalAndPreservesComputerKeys)
{
    VirtualSource source;
    ASSERT_EQ(MIDIClientCreate(CFSTR("Audacity input ownership test"), nullptr, nullptr, &source.client), noErr);
    ASSERT_EQ(MIDISourceCreate(source.client, CFSTR("Audacity input ownership test"), &source.source), noErr);
    SInt32 id = 0;
    ASSERT_EQ(MIDIObjectGetIntegerProperty(source.source, kMIDIPropertyUniqueID, &id), noErr);
    auto port = std::make_shared<ScopedCoreInput>();
    InputRegistration registration(port);
    auto voices = std::make_shared<InputVoices>();
    LiveInstrumentSession session(MidiTrack::Create(), voices);
    session.prepare(48000.0, 32);
    ASSERT_TRUE(session.setMidiInput(std::to_string(id)));
    session.sendMidi(0x90, 60, 95);
    const auto render = [&] {
        std::array<float, 32> samples {};
        float* output[] { samples.data() };
        session.process(output, 1, samples.size(), {}, {});
        return samples.front();
    };
    EXPECT_EQ(render(), 1.0f);
    MIDIPacketList packets {};
    auto* packet = MIDIPacketListInit(&packets);
    const Byte messages[][3] { { 0x90, 60, 100 }, { 0xb0, 64, 127 }, { 0x90, 67, 100 }, { 0x80, 67, 0 } };
    for (const auto& message : messages) {
        packet = MIDIPacketListAdd(&packets, sizeof(packets), packet, AudioGetCurrentHostTime(), 3, message);
        ASSERT_NE(packet, nullptr);
    }
    ASSERT_EQ(MIDIReceived(source.source, &packets), noErr);
    ASSERT_EQ(receive(session, 4).size(), 4u);
    EXPECT_EQ(render(), 2.0f);
    ASSERT_EQ(MIDIEndpointDispose(source.source), noErr);
    source.source = 0;
    const auto releases = receive(session, 2);
    ASSERT_EQ(releases.size(), 2u);
    EXPECT_FALSE(port->isConnected());
    EXPECT_EQ(releases[0].status, 0x80);
    EXPECT_EQ(releases[0].data1, 60);
    EXPECT_EQ(releases[1].status, 0xb0);
    EXPECT_EQ(releases[1].data1, 64);
    EXPECT_EQ(releases[1].data2, 0);
    EXPECT_EQ(render(), 1.0f);
    EXPECT_EQ(voices->ons[0][60], 1);
    EXPECT_EQ(voices->offs[0][60], 0);
    session.sendMidi(0x80, 60, 0);
    EXPECT_EQ(render(), 0.0f);
}

TEST(NativeMidiInputClockTests, RealCoreMidiPacketsKeepHostTimestampsAndZeroMeansDriverArrival)
{
    VirtualSource source;
    ASSERT_EQ(MIDIClientCreate(CFSTR("Audacity input clock test"), nullptr, nullptr, &source.client), noErr);
    ASSERT_EQ(MIDISourceCreate(source.client, CFSTR("Audacity input clock test"), &source.source), noErr);
    SInt32 id = 0;
    ASSERT_EQ(MIDIObjectGetIntegerProperty(source.source, kMIDIPropertyUniqueID, &id), noErr);
    auto port = std::make_shared<ScopedCoreInput>();
    InputRegistration registration(port);
    LiveInstrumentSession session(MidiTrack::Create(), createBuiltinInstrument({}));
    ASSERT_TRUE(session.setMidiInput(std::to_string(id)));
    const auto host = AudioGetCurrentHostTime();
    const auto before = Clock::now();
    MIDIPacketList packets {};
    auto* packet = MIDIPacketListInit(&packets);
    const Byte on[] { 0x90, 60, 100 }, off[] { 0x80, 60, 0 };
    packet = MIDIPacketListAdd(&packets, sizeof(packets), packet, host - AudioConvertNanosToHostTime(200000000), 3, on);
    ASSERT_NE(packet, nullptr);
    packet = MIDIPacketListAdd(&packets, sizeof(packets), packet, host - AudioConvertNanosToHostTime(50000000), 3, off);
    ASSERT_NE(packet, nullptr);
    ASSERT_EQ(MIDIReceived(source.source, &packets), noErr);
    // A busy UI must not move either note to the eventual delivery time.
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    const auto messages = receive(session, 2);
    ASSERT_EQ(messages.size(), 2u);
    EXPECT_NEAR(messages[0].time, seconds(before) - 0.2, 0.003);
    EXPECT_NEAR(messages[1].time, seconds(before) - 0.05, 0.003);
    EXPECT_NEAR(messages[1].time - messages[0].time, 0.15, 0.002);

    packet = MIDIPacketListInit(&packets);
    ASSERT_NE(MIDIPacketListAdd(&packets, sizeof(packets), packet, 0, 3, on), nullptr);
    const auto zeroBegin = seconds(Clock::now());
    ASSERT_EQ(MIDIReceived(source.source, &packets), noErr);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    const auto beforeDelivery = seconds(Clock::now());
    const auto zeroMessages = receive(session, 1);
    ASSERT_EQ(zeroMessages.size(), 1u);
    EXPECT_GE(zeroMessages[0].time, zeroBegin - 0.003);
    EXPECT_LT(zeroMessages[0].time, beforeDelivery - 0.05);
}
#endif
}
