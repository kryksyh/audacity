/*
* Audacity: A Digital Audio Editor
*/
#include <gtest/gtest.h>
#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLibrary>
#include <portaudio.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <chrono>
#include <condition_variable>
#include <future>
#include <mutex>
#include <iostream>
#include <thread>

#include "au3-midi-track/MidiTrack.h"
#include "au3-midi-track/MidiSequence.h"
#include "au3-playable-track/TrackAutomation.h"
#include "../internal/instrumentprocessor.h"
#include "../internal/midiplayablesequence.h"
#include "../internal/liveinstrumentsession.h"
#include "../internal/nativeeffectparameters.h"
#include "../internal/nativeeffectchain.h"
#include "au3-mixer/Mix.h"
#include "au3-audio-io/AudioIO.h"
#if AU_NATIVE_TEST_AUDIO_UNIT
#include "au3-audio-unit/AudioUnitEffectBase.h"
#include "au3-audio-unit/AudioUnitEffectsModule.h"
#include "au3-audio-unit/AudioUnitInstance.h"
#endif
#ifdef AU_NATIVE_TEST_INSTRUMENT_PATH
#include "au3-vst3/VST3EffectsModule.h"
#include "au3-vst3/VST3EffectBase.h"
#include "au3-vst3/VST3Instance.h"
#include "au3-vst3/VST3Wrapper.h"
#include <pluginterfaces/vst/ivsteditcontroller.h>
#include <public.sdk/source/vst/hosting/hostclasses.h>
#endif

namespace au::au3audio {
namespace {
double energy(const std::vector<float>& data)
{
    double sum = 0.0;
    for (float sample : data) {
        sum += sample * sample;
    }
    return sum;
}
}

TEST(NativeMidiTests, EventsUseSampleOffsetsAndHalfOpenBlockBoundaries)
{
    auto track = MidiTrack::Create();
    track->SetTempo(120.0);
    ASSERT_TRUE(track->SetClips({ { 1, "Notes", 2.0, 4.0, 3, { { 1, 0.5, 1.0, 60, 90 } }, {} } }));
    MidiSequence sequence(*track, 48000.0);
    std::vector<MidiEvent> events;
    sequence.AppendEvents(59900, 100, events);
    EXPECT_TRUE(events.empty());
    sequence.AppendEvents(60000, 24000, events);
    ASSERT_EQ(events.size(), 1u);
    EXPECT_EQ(events[0], (MidiEvent { 0, 0x92, 60, 90 }));
    events.clear();
    sequence.AppendEvents(84000, 512, events);
    ASSERT_EQ(events.size(), 1u);
    EXPECT_EQ(events[0], (MidiEvent { 0, 0x82, 60, 0 }));
}

TEST(NativeMidiTests, SeekingRestoresPedalHeldNotesAndClipEndReleasesThem)
{
    auto track = MidiTrack::Create();
    ASSERT_TRUE(track->SetClips({ { 1, "Pedal", 0.0, 4.0, 1, { { 1, 0.0, 1.0, 64, 110 } }, { { 0.0, 64, 127 } } } }));
    MidiSequence sequence(*track, 48000.0);
    std::vector<MidiEvent> events;
    sequence.Chase(48000, events);
    ASSERT_EQ(events.size(), 3u);
    EXPECT_EQ(events[0], (MidiEvent { 0, 0xb0, 64, 127 }));
    EXPECT_EQ(events[1], (MidiEvent { 0, 0x90, 64, 110 }));
    EXPECT_EQ(events[2], (MidiEvent { 0, 0x80, 64, 0 }));
    events.clear();
    sequence.Chase(96001, events);
    ASSERT_EQ(events.size(), 1u);
    EXPECT_EQ(events[0], (MidiEvent { 0, 0xb0, 64, 0 }));
}

TEST(NativeMidiTests, NoteOffPrecedesRetriggerAtTheSameSample)
{
    auto track = MidiTrack::Create();
    ASSERT_TRUE(track->SetClips({ { 1, "Repeat", 0.0, 4.0, 1,
                                    { { 1, 0.0, 1.0, 60, 90 }, { 2, 1.0, 1.0, 60, 100 } }, {} } }));
    MidiSequence sequence(*track, 48000.0);
    std::vector<MidiEvent> events;
    sequence.AppendEvents(24000, 1, events);
    ASSERT_EQ(events.size(), 2u);
    EXPECT_EQ(events[0].status, 0x80);
    EXPECT_EQ(events[1].status, 0x90);
}

TEST(NativeMidiTests, TrimmedSequenceClipsCrossingNotesAndChasesControllersBeforeBoundaryEvents)
{
    auto track = MidiTrack::Create();
    MidiClipData clip { 1, "Trim", 0, 8, 1,
                        { { 1, 0, 3, 60, 100 }, { 2, 5, 2, 65, 90 }, { 3, 7, 1, 70, 80 } },
                        { { 0, 74, 10 }, { 1, 74, 20 }, { 2, 74, 30 }, { 7, 74, 90 } } };
    clip.trimLeft = 2;
    clip.trimRight = 2;
    ASSERT_TRUE(track->SetClips({ clip }));
    MidiSequence sequence(*track, 48000);
    std::vector<MidiEvent> events;
    sequence.AppendEvents(0, 200000, events);
    ASSERT_EQ(events.size(), 7u);
    EXPECT_EQ(events[0], (MidiEvent { 48000, 0xb0, 74, 20 }));
    EXPECT_EQ(events[1], (MidiEvent { 48000, 0xb0, 74, 30 }));
    EXPECT_EQ(events[2], (MidiEvent { 48000, 0x90, 60, 100 }));
    EXPECT_EQ(events[3], (MidiEvent { 72000, 0x80, 60, 0 }));
    EXPECT_EQ(events[4], (MidiEvent { 120000, 0x90, 65, 90 }));
    EXPECT_EQ(events[5], (MidiEvent { 144000, 0x80, 65, 0 }));
    EXPECT_EQ(events[6], (MidiEvent { 144000, 0xb0, 64, 0 }));
    events.clear();
    sequence.Chase(48001, events);
    ASSERT_EQ(events.size(), 2u);
    EXPECT_EQ(events[0].data2, 30);
    EXPECT_EQ(events[1].data1, 60);
}

TEST(NativeInstrumentTests, NotesStartAtTheirSampleOffsetAndVelocityChangesTheAudio)
{
    auto instrument = createBuiltinInstrument({});
    instrument->prepare(48000.0, 512);
    std::vector<float> left(512), right(512);
    float* output[] { left.data(), right.data() };
    const std::array events { MidiEvent { 128, 0x90, 69, 127 } };
    instrument->process(output, 2, 512, events, {});
    EXPECT_TRUE(std::all_of(left.begin(), left.begin() + 128, [](float sample) { return sample == 0.0f; }));
    const double loud = energy(left);
    EXPECT_GT(loud, 0.001);
    EXPECT_EQ(left, right);
    instrument->reset();
    const std::array quiet { MidiEvent { 128, 0x90, 69, 32 } };
    instrument->process(output, 2, 512, quiet, {});
    EXPECT_NEAR(energy(left) / loud, std::pow(32.0 / 127.0, 2.0), 1e-5);
}

TEST(NativeInstrumentTests, SustainKeepsReleasedNotesAndAllSoundOffClearsThem)
{
    auto instrument = createBuiltinInstrument({});
    instrument->prepare(48000.0, 512);
    std::vector<float> output(512);
    float* pointers[] { output.data() };
    const std::array events { MidiEvent { 0, 0xb0, 64, 127 }, MidiEvent { 0, 0x90, 60, 100 }, MidiEvent { 64, 0x80, 60, 0 } };
    instrument->process(pointers, 1, 512, events, {});
    for (int block = 0; block < 50; ++block) {
        instrument->process(pointers, 1, 512, {}, {});
    }
    EXPECT_GT(energy(output), 0.01);
    const std::array stop { MidiEvent { 0, 0xb0, 120, 0 } };
    instrument->process(pointers, 1, 512, stop, {});
    EXPECT_EQ(energy(output), 0.0);
}

TEST(NativeInstrumentTests, OfflineChannelModeMessagesReleaseKeysAtTheirSampleOffset)
{
    for (const int mode : { 120, 123, 124, 125, 126, 127 }) {
        SCOPED_TRACE(mode);
        auto instrument = createBuiltinInstrument({});
        auto reference = createBuiltinInstrument({});
        instrument->prepare(48000, 512);
        reference->prepare(48000, 512);
        std::array<float, 512> actual {}, expected {};
        float* output[] { actual.data() };
        float* compare[] { expected.data() };
        const std::array events { MidiEvent { 0, 0x90, 60, 100 }, MidiEvent { 173, 0xb0, static_cast<uint8_t>(mode), 0 } };
        const std::array expectedEvents { MidiEvent { 0, 0x90, 60, 100 },
                                          mode == 120 ? MidiEvent { 173, 0xb0, 120, 0 } : MidiEvent { 173, 0x80, 60, 0 } };
        instrument->process(output, 1, 512, events, {});
        reference->process(compare, 1, 512, expectedEvents, {});
        EXPECT_EQ(actual, expected);
        EXPECT_TRUE(std::any_of(actual.begin(), actual.begin() + 173, [](auto x) { return x != 0; }));
        if (mode == 120) {
            EXPECT_TRUE(std::all_of(actual.begin() + 173, actual.end(), [](auto x) {
                return x == 0;
            }));
        }
        for (int block = 0; block < 50; ++block) {
            instrument->process(output, 1, 512, {}, {});
        }
        EXPECT_TRUE(std::all_of(actual.begin(), actual.end(), [](auto x) { return x == 0; }));
    }
}

TEST(NativeInstrumentTests, ResetControllersRestoresExpressionWithoutChangingChannelVolume)
{
    auto instrument = createBuiltinInstrument({});
    auto reference = createBuiltinInstrument({});
    instrument->prepare(48000, 512);
    reference->prepare(48000, 512);
    std::array<float, 512> actual {}, expected {};
    float* output[] { actual.data() };
    float* compare[] { expected.data() };
    const std::array input { MidiEvent { 0, 0xb0, 7, 64 }, MidiEvent { 0, 0xb0, 11, 32 },
                             MidiEvent { 0, 0x90, 60, 100 }, MidiEvent { 173, 0xb0, 121, 0 } };
    const std::array note { MidiEvent { 0, 0x90, 60, 100 } };
    instrument->process(output, 1, 512, input, {});
    reference->process(compare, 1, 512, note, {});
    for (size_t i = 0; i < actual.size(); ++i) {
        EXPECT_NEAR(actual[i], expected[i] * (64.0 / 127.0) * (i < 173 ? 32.0 / 127.0 : 1.0), 1e-7) << i;
    }
}

TEST(NativeMidiTests, SeekingPastControllerResetReleasesPedalNotesAndPreservesNonResettableControls)
{
    auto track = MidiTrack::Create();
    track->SetTempo(120);
    ASSERT_TRUE(track->SetClips({ { 1, "Controller reset", 0, 4, 1, { { 1, 0, 0.1, 60, 100 } },
                                    { { 0, 0, 3 }, { 0, 32, 4 }, { 0, 7, 64 }, { 0, 10, 37 }, { 0, 74, 53 }, { 0, 91, 23 },
                                        { 0, 1, 90 }, { 0, 11, 32 }, { 0, 64, 127 }, { 0.2, 121, 0 } } } }));
    MidiSequence sequence(*track, 48000);
    std::vector<MidiEvent> chased;
    sequence.Chase(7200, chased);
    EXPECT_FALSE(std::any_of(chased.begin(), chased.end(), [](const auto& e) { return (e.status & 0xf0) == 0x90; }));
    const auto value = [&](int cc) {
        const auto it = std::find_if(chased.begin(), chased.end(), [cc](const auto& e) { return e.status == 0xb0 && e.data1 == cc; });
        return it == chased.end() ? -1 : int(it->data2);
    };
    EXPECT_EQ(value(0), 3);
    EXPECT_EQ(value(32), 4);
    EXPECT_EQ(value(7), 64);
    EXPECT_EQ(value(10), 37);
    EXPECT_EQ(value(74), 53);
    EXPECT_EQ(value(91), 23);
    EXPECT_EQ(value(1), 0);
    EXPECT_EQ(value(11), 127);
    EXPECT_EQ(value(64), 0);
    for (int cc = 98; cc <= 101; ++cc) {
        EXPECT_EQ(value(cc), 127) << cc;
    }
}

TEST(NativeInstrumentTests, SavedParametersRestoreTheSameRenderedAudio)
{
    auto original = createBuiltinInstrument({});
    ASSERT_TRUE(original->setParameter("4", 0.6));
    ASSERT_TRUE(original->setParameter("5", 0.7));
    auto restored = createBuiltinInstrument(original->state());
    original->prepare(48000.0, 512);
    restored->prepare(48000.0, 512);
    std::vector<float> first(512), second(512);
    float* a[] { first.data() };
    float* b[] { second.data() };
    const std::array events { MidiEvent { 0, 0x90, 69, 100 } };
    original->process(a, 1, 512, events, {});
    restored->process(b, 1, 512, events, {});
    EXPECT_EQ(first, second);
    EXPECT_GT(energy(first), 0.01);
}

TEST(NativeInstrumentTests, AudacityMixerRendersMidiAndRepositionsForAnotherPass)
{
    auto track = MidiTrack::Create();
    ASSERT_TRUE(track->SetClips({ { 1, "Phrase", 0.0, 4.0, 1, { { 1, 0.0, 1.0, 69, 100 } }, {} } }));
    auto sequence = std::make_shared<MidiPlayableSequence>(track, createBuiltinInstrument({}), 48000.0);
    Mixer::Inputs inputs { Mixer::Input { sequence } };
    Mixer mixer(std::move(inputs), std::nullopt, true, Mixer::WarpOptions { static_cast<const AudacityProject*>(nullptr) }, 0.0, 2.0,
                2, 512, false, 48000.0, floatSample);
    ASSERT_EQ(mixer.Process(512), 512u);
    const auto* first = reinterpret_cast<const float*>(mixer.GetBuffer(0));
    std::vector<float> expected(first, first + 512);
    EXPECT_GT(energy(expected), 0.001);
    mixer.Reposition(1.5, true);
    ASSERT_EQ(mixer.Process(512), 512u);
    const auto* silence = reinterpret_cast<const float*>(mixer.GetBuffer(0));
    EXPECT_TRUE(std::all_of(silence, silence + 512, [](float value) { return value == 0.0f; }));
    mixer.Reposition(0.0, true);
    ASSERT_EQ(mixer.Process(512), 512u);
    const auto* repeated = reinterpret_cast<const float*>(mixer.GetBuffer(0));
    EXPECT_EQ(std::vector<float>(repeated, repeated + 512), expected);
}

TEST(NativeAutomationTests, CurvesInterpolateAndDisabledLanesUseTheManualValue)
{
    AutomationData data;
    ASSERT_TRUE(data.SetLane({ "volume", true, { { 1.0, 0.0, 0.0 }, { 3.0, 1.0, 0.0 } } }));
    EXPECT_DOUBLE_EQ(data.ValueAt("volume", 0.0, 0.9), 0.0);
    EXPECT_DOUBLE_EQ(data.ValueAt("volume", 2.0, 0.9), 0.5);
    EXPECT_DOUBLE_EQ(data.ValueAt("volume", 4.0, 0.9), 1.0);
    ASSERT_TRUE(data.SetLane({ "volume", true, { { 1.0, 0.0, 0.5 }, { 3.0, 1.0, 0.0 } } }));
    EXPECT_LT(data.ValueAt("volume", 2.0, 0.9), 0.5);
    ASSERT_TRUE(data.SetLane({ "volume", false, { { 1.0, 0.0, 0.0 } } }));
    EXPECT_DOUBLE_EQ(data.ValueAt("volume", 2.0, 0.9), 0.9);
}

namespace {
class LiveCallback final : public AudioIoCallback
{
public:
    LiveCallback()
    {
        mNumPlaybackChannels = 2;
        SetMixerOutputVol(0.5f);
    }

    void StopStream() override { SetLiveOutput({}); }
    void StopMonitoring() override { StopStream(); }
    void render(float* output, float* meter, size_t frames) { MixLiveOutput(output, meter, frames); }
};

class SynchronizationCallback final : public AudioIoCallback
{
public:
    SynchronizationCallback()
    {
        mAudioThreadAcknowledge.store(Acknowledge::eNone);
        mAudioThreadShouldCallSequenceBufferExchangeOnce.store(false);
        mAudioThreadSequenceBufferExchangeLoopRunning.store(false);
    }

    void StopStream() override {}
    void StopMonitoring() override {}
    void signal(Acknowledge state)
    {
        AcknowledgeAudioThread(state);
    }

    void waitStarted() { WaitForAudioThreadStarted(); }
    void waitStopped() { WaitForAudioThreadStopped(); }
    void requestPass() { ProcessOnceAndWait(); }
    void exclusive(const std::function<void()>& action) { WithAudioThreadStopped(action); }
    void setRunning() { mAudioThreadSequenceBufferExchangeLoopRunning.store(true); }
    bool running() { return mAudioThreadSequenceBufferExchangeLoopRunning.load(std::memory_order_acquire); }
    void setProcessing(bool active) { mAudioThreadSequenceBufferExchangeLoopActive.store(active); }
    void initializeSeek()
    {
        AudioIOStartStreamOptions options({}, 48000.0);
        mPlaybackSchedule.Init(0.0, 2.0, options, nullptr);
        mPlaybackSchedule.GetPolicy().Initialize(mPlaybackSchedule, 48000.0);
        mPlaybackSchedule.SetSequenceTime(0.5);
        mPlaybackSchedule.mTimeQueue.Init(64);
        mPlaybackSchedule.mTimeQueue.Prime(0.5);
        mStreamToken = 1;
        mSeek = 0.25;
        setProcessing(true);
        setRunning();
    }

    Acknowledge acknowledgement() const { return mAudioThreadAcknowledge.load(std::memory_order_acquire); }
    double position() const { return mPlaybackSchedule.GetSequenceTime(); }
    bool canEnterTransportBoundary()
    {
        if (mSuspendAudioThread.TryLock() != wxMUTEX_NO_ERROR) {
            return false;
        }
        mSuspendAudioThread.Unlock();
        return true;
    }

    void completePass(std::atomic<int>& result)
    {
        while (!mAudioThreadShouldCallSequenceBufferExchangeOnce.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        result.store(42, std::memory_order_relaxed);
        CompleteAudioThreadPass();
    }
};

class ConstantLiveSource : public LiveAudioSource
{
public:
    void Prepare(double) override {}
    void Render(float* output, size_t channels, size_t frames) noexcept override { std::fill_n(output, channels * frames, 0.25f); }
};
}

TEST(NativeLiveAudioTests, TransportWaitsObserveAcknowledgementsPublishedBeforeOrDuringTheWait)
{
    SynchronizationCallback callback;
    callback.signal(Acknowledge::eStart);
    callback.waitStarted();
    callback.signal(Acknowledge::eStop);
    callback.waitStopped();
    std::thread start([&] { callback.signal(Acknowledge::eStart); });
    callback.waitStarted();
    start.join();
    std::thread stop([&] { callback.signal(Acknowledge::eStop); });
    callback.waitStopped();
    stop.join();
    std::atomic<int> result { 0 };
    std::thread pass([&] { callback.completePass(result); });
    callback.requestPass();
    EXPECT_EQ(result.load(), 42);
    pass.join();
}

TEST(NativeLiveAudioTests, MainThreadRefreshStopsAndRestartsTheWorkerEvenWhenTheActionFails)
{
    SynchronizationCallback callback;
    callback.setRunning();
    const auto mainThread = std::this_thread::get_id();
    std::atomic<int> pauses { 0 };
    std::thread worker([&] {
        for (int pass = 0; pass < 2; ++pass) {
            while (callback.running()) { std::this_thread::yield();
            }
            pauses.fetch_add(1);
            callback.signal(Acknowledge::eStop);
            while (!callback.running()) { std::this_thread::yield();
            }
            callback.signal(Acknowledge::eStart);
        }
    });
    callback.exclusive([&] {
        EXPECT_EQ(std::this_thread::get_id(), mainThread);
        EXPECT_FALSE(callback.running());
        EXPECT_EQ(pauses.load(), 1);
    });
    EXPECT_TRUE(callback.running());
    EXPECT_THROW(callback.exclusive([&] {
        EXPECT_FALSE(callback.running());
        EXPECT_EQ(pauses.load(), 2);
        throw std::runtime_error("refresh failed");
    }), std::runtime_error);
    EXPECT_TRUE(callback.running());
    worker.join();
}

TEST(NativeLiveAudioTests, MainThreadRefreshLeavesAnIdleWorkerStopped)
{
    SynchronizationCallback callback;
    int calls = 0;
    callback.exclusive([&] { ++calls; });
    EXPECT_EQ(calls, 1);
    EXPECT_FALSE(callback.running());
}

TEST(NativeLiveAudioTests, SeekConsumesBothWorkerAcknowledgementsAndPrimesTheNewPositionBeforeRestarting)
{
    SynchronizationCallback callback;
    callback.initializeSeek();
    std::atomic<int> refilled { 0 };
    std::atomic<double> refillPosition { -1 };
    std::thread worker([&] {
        while (callback.running()) { std::this_thread::yield();
        }
        // Simulate a processor finishing the current block after the stop
        // request. A fixed 50 ms callback poll must not replace this handshake.
        std::this_thread::sleep_for(std::chrono::milliseconds(3));
        callback.setProcessing(false);
        callback.signal(Acknowledge::eStop);
        callback.completePass(refilled);
        refillPosition.store(callback.position());
        while (!callback.running()) { std::this_thread::yield();
        }
        callback.signal(Acknowledge::eStart);
    });
    const auto before = std::chrono::steady_clock::now();
    EXPECT_EQ(callback.CallbackDoSeek(), paContinue);
    const auto elapsed = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - before).count();
    worker.join();
    EXPECT_EQ(refilled.load(), 42);
    EXPECT_DOUBLE_EQ(refillPosition.load(), 0.75);
    EXPECT_DOUBLE_EQ(callback.position(), 0.75);
    EXPECT_TRUE(callback.running());
    EXPECT_EQ(callback.acknowledgement(), Acknowledge::eNone);
    std::cout << "Seek worker handshake elapsedMs=" << elapsed << '\n';
}

TEST(NativeLiveAudioTests, MainThreadRefreshSerializesItsActionWithTheSeekAndStopTransportBoundary)
{
    SynchronizationCallback callback;
    std::atomic<bool> entered { true };
    callback.exclusive([&] {
        std::thread seek([&] { entered = callback.canEnterTransportBoundary(); });
        seek.join();
    });
    EXPECT_FALSE(entered.load());
    EXPECT_TRUE(callback.canEnterTransportBoundary());
    EXPECT_THROW(callback.exclusive([] { throw std::runtime_error("refresh failed"); }), std::runtime_error);
    EXPECT_TRUE(callback.canEnterTransportBoundary());
}

TEST(NativeLiveAudioTests, LiveOutputMixesAtTheDeviceCallbackWithoutPlaybackQueueDelay)
{
    LiveCallback callback;
    callback.SetLiveOutput(std::make_shared<ConstantLiveSource>());
    std::array<float, 1024> output {}, meter {};
    callback.render(output.data(), meter.data(), 512);
    EXPECT_TRUE(std::all_of(output.begin(), output.end(), [](float value) { return value == 0.125f; }));
    EXPECT_EQ(output, meter);
    callback.SetLiveOutput({});
    output.fill(0.0f);
    callback.render(output.data(), output.data(), 512);
    EXPECT_TRUE(std::all_of(output.begin(), output.end(), [](float value) { return value == 0.0f; }));
}

TEST(NativeLiveAudioTests, MidiPlaybackMetersUseTheDocumentTrackId)
{
    auto track = MidiTrack::Create();
    const auto tracks = TrackList::Create(nullptr);
    tracks->Add(track);
    ASSERT_GE(static_cast<int64_t>(track->GetId()), 0);
    const auto sequence = std::make_shared<MidiPlayableSequence>(track, createBuiltinInstrument({}), 48000.0);
    AudioIoCallback::Track output(sequence);
    EXPECT_EQ(output.trackId(), static_cast<int64_t>(track->GetId()));
}

TEST(NativeLiveAudioTests, ReplacingTheSourceWaitsForItsCallbackAndDestroysItOnTheMainThread)
{
    struct State {
        std::atomic<bool> entered { false };
        std::atomic<bool> release { false };
        std::atomic<bool> destroyed { false };
        std::mutex mutex;
        std::condition_variable changed;
        std::thread::id destructionThread;
    } state;
    class BlockingSource final : public ConstantLiveSource
    {
    public:
        explicit BlockingSource(State& state)
            : m_state(state) {}
        ~BlockingSource() override { m_state.destructionThread = std::this_thread::get_id(); m_state.destroyed = true; }
        void Render(float* output, size_t channels, size_t frames) noexcept override
        {
            {
                std::lock_guard lock(m_state.mutex);
                m_state.entered = true;
            }
            m_state.changed.notify_all();
            while (!m_state.release.load()) {
                std::this_thread::yield();
            }
            ConstantLiveSource::Render(output, channels, frames);
        }

    private:
        State& m_state;
    };
    LiveCallback callback;
    auto source = std::make_shared<BlockingSource>(state);
    callback.SetLiveOutput(source);
    std::array<float, 1024> output {};
    std::thread audio([&] { callback.render(output.data(), output.data(), 512); });
    {
        std::unique_lock lock(state.mutex);
        if (!state.changed.wait_for(lock, std::chrono::seconds(1), [&] { return state.entered.load(); })) {
            state.release = true;
            audio.join();
            FAIL() << "The audio callback did not enter the live source.";
            return;
        }
    }
    source.reset();
    std::thread release([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        EXPECT_FALSE(state.destroyed.load());
        state.release = true;
    });
    callback.SetLiveOutput({});
    audio.join();
    release.join();
    EXPECT_TRUE(state.destroyed.load());
    EXPECT_EQ(state.destructionThread, std::this_thread::get_id());
}

TEST(NativeLiveAudioTests, KeyboardNotesReachTheSynthAndReleaseDoesNotDiscardTheNextNote)
{
    auto track = MidiTrack::Create();
    auto source = std::make_shared<LiveInstrumentSession>(track, createBuiltinInstrument({}));
    source->Prepare(48000.0);
    LiveCallback callback;
    callback.SetLiveOutput(source);
    std::array<float, 1024> output {};
    source->sendMidi(0x90, 69, 100);
    callback.render(output.data(), output.data(), 512);
    EXPECT_TRUE(std::any_of(output.begin(), output.end(), [](float value) { return value != 0.0f; }));
    source->releaseNotes();
    output.fill(0.0f);
    callback.render(output.data(), output.data(), 512);
    EXPECT_TRUE(std::all_of(output.begin(), output.end(), [](float value) { return value == 0.0f; }));
    source->releaseNotes();
    source->sendMidi(0x90, 72, 100);
    callback.render(output.data(), output.data(), 512);
    EXPECT_TRUE(std::any_of(output.begin(), output.end(), [](float value) { return value != 0.0f; }));
    callback.SetLiveOutput({});
    source->close();
}

namespace {
class SessionProbeInstrument final : public InstrumentProcessor
{
public:
    void prepare(double, size_t) override { ++prepares; }
    void reset() override { ++resets; }
    std::vector<audio::InstrumentParameter> parameters() const override { return { { "gain", "Gain", "", gain } }; }
    bool setParameter(const std::string&, double value) override { gain = value; return true; }
    std::string state() const override { return std::to_string(gain); }
    void process(float* const* output, size_t channels, size_t frames, std::span<const MidiEvent> midi,
                 const InstrumentProcessContext& context) override
    {
        events.assign(midi.begin(), midi.end());
        playing = context.playing;
        transport = context;
        processingThread = std::this_thread::get_id();
        for (size_t channel = 0; channel < channels; ++channel) {
            std::fill_n(output[channel], frames, gain);
        }
    }

    void restoreManualParameters() noexcept override { ++manualRestores; }
    bool restoreMidiController(int channel, int control) noexcept override { ++controllerRestores[channel][control]; return false; }
    std::array<std::array<int, 130>, 16> controllerRestores {};
    int prepares = 0, resets = 0, manualRestores = 0;
    double gain = 0.5;
    bool playing = false;
    InstrumentProcessContext transport;
    std::thread::id processingThread;
    std::vector<MidiEvent> events;
};
}

namespace {
class VoiceContinuityInstrument final : public InstrumentProcessor
{
public:
    size_t latency() const override { return cachedLatency; }
    bool latencyUpdatePending() const noexcept override { return unstableLatency || requestedLatency != cachedLatency; }
    bool requiresMainThreadLatencyRefresh() const noexcept override { return true; }
    bool refreshLatency() override
    {
        ++refreshes;
        refreshThread = std::this_thread::get_id();
        if (throwOnRefresh) {
            throw std::runtime_error("Fixture delay refresh failed.");
        }
        const bool changed = latencyUpdatePending();
        cachedLatency = requestedLatency;
        return changed;
    }

    void prepare(double, size_t) override { reset(); }
    void reset() override { held.fill(false); age = 0; ++resets; }
    std::vector<audio::InstrumentParameter> parameters() const override { return {}; }
    bool setParameter(const std::string&, double) override { return false; }
    std::string state() const override { return {}; }
    void process(float* const* output, size_t channels, size_t frames, std::span<const MidiEvent> events,
                 const InstrumentProcessContext& context) override
    {
        contexts.push_back(context);
        size_t index = 0;
        for (size_t sample = 0; sample < frames; ++sample) {
            while (index < events.size() && events[index].sample <= static_cast<int64_t>(sample)) {
                const auto& event = events[index++];
                const auto type = event.status & 0xf0;
                if (type == 0x90 && event.data2) {
                    held[event.data1] = true;
                    ++ons[event.data1];
                    if (event.data1 == 60) {
                        age = 0;
                    }
                } else if (type == 0x80 || type == 0x90) {
                    held[event.data1] = false;
                    ++offs[event.data1];
                }
            }
            const auto value = held[60] ? static_cast<float>(++age) : 0.0f;
            for (size_t channel = 0; channel < channels; ++channel) {
                output[channel][sample] = value;
            }
        }
    }

    std::array<bool, 128> held {};
    std::array<int, 128> ons {}, offs {};
    std::vector<InstrumentProcessContext> contexts;
    int age = 0, resets = 0;
    size_t cachedLatency = 0, requestedLatency = 0;
    bool unstableLatency = false;
    bool throwOnRefresh = false;
    std::thread::id refreshThread;
    unsigned refreshes = 0;
};
}

TEST(NativeLiveAudioTests, QuiescentLatencyPublicationDoesNotDrainQueuedKeysOrResetASoundingVoice)
{
    const auto track = MidiTrack::Create();
    const auto processor = std::make_shared<VoiceContinuityInstrument>();
    LiveInstrumentSession session(track, processor);
    session.prepare(48000.0, 512);
    session.sendMidi(0x90, 60, 110);
    std::array<float, 32> left {}, right {};
    float* output[] { left.data(), right.data() };
    session.process(output, 2, 32, {}, {});
    EXPECT_FLOAT_EQ(left.back(), 32.0f);
    const auto resets = processor->resets;
    session.sendMidi(0x90, 64, 90);
    processor->requestedLatency = 17;
    EXPECT_TRUE(session.latencyUpdatePending());
    EXPECT_EQ(session.latency(), 0u);
    EXPECT_TRUE(session.refreshLatency());
    EXPECT_EQ(session.latency(), 17u);
    EXPECT_FALSE(session.latencyUpdatePending());
    session.process(output, 2, 32, {}, {});
    EXPECT_FLOAT_EQ(left.front(), 33.0f);
    EXPECT_FLOAT_EQ(left.back(), 64.0f);
    EXPECT_EQ(processor->resets, resets);
    EXPECT_EQ(processor->ons[60], 1);
    EXPECT_EQ(processor->ons[64], 1);
}

namespace {
class DelayedPositionInstrument final : public InstrumentProcessor
{
public:
    void prepare(double, size_t) override { reset(); }
    void reset() override { history.fill(0); head = 0; }
    size_t latency() const override { return publishedDelay; }
    std::vector<audio::InstrumentParameter> parameters() const override { return {}; }
    bool setParameter(const std::string&, double) override { return false; }
    std::string state() const override { return {}; }
    void process(float* const* output, size_t channels, size_t frames, std::span<const MidiEvent> events,
                 const InstrumentProcessContext& context) override
    {
        calls.emplace_back(context.sample, frames);
        for (const auto& event : events) {
            if ((event.status & 0xf0) == 0x90 && event.data2) {
                ++ons[event.data1];
            }
            if ((event.status & 0xf0) == 0x80) {
                ++offs[event.data1];
            }
        }
        for (size_t i = 0; i < frames; ++i) {
            history[head] = static_cast<float>(context.sample + static_cast<int64_t>(i) + 1);
            const auto value = history[(head + history.size() - requestedDelay) % history.size()];
            for (size_t channel = 0; channel < channels; ++channel) {
                output[channel][i] = value;
            }
            head = (head + 1) % history.size();
        }
        publishedDelay = requestedDelay;
    }

    size_t requestedDelay = 0, publishedDelay = 0, head = 0;
    std::array<float, 64> history {};
    std::array<unsigned, 128> ons {}, offs {};
    std::vector<std::pair<int64_t, size_t> > calls;
};
}

TEST(NativeLiveAudioTests, ChangingDelayRetainsPendingPacketPositionsAndVisitsEveryMidiInputSampleExactlyOnce)
{
    const auto track = MidiTrack::Create();
    std::vector<MidiNoteData> notes;
    for (int i = 0; i < 16; ++i) {
        notes.push_back({ i + 1, i * 16.0 / 24000, 8.0 / 24000, 60, 80 });
    }
    ASSERT_TRUE(track->SetClips({ { 1, "Forward MIDI", 0, 1, 1, notes, {} } }));
    const auto processor = std::make_shared<DelayedPositionInstrument>();
    const auto session = std::make_shared<LiveInstrumentSession>(track, processor);
    session->beginTimeline();
    MidiPlayableSequence sequence(track, session, 48000);
    std::array<float, 64> left {}, right {};
    float* output[] { left.data(), right.data() };
    int64_t position = 0;
    for (const auto delay : { 0u, 7u, 3u, 15u, 0u }) {
        processor->requestedDelay = delay;
        if (position == 64) {
            session->sendMidi(0x90, 80, 95);
        }
        if (position == 192) {
            session->sendMidi(0x80, 80, 0);
        }
        ASSERT_TRUE(sequence.GetFloats(0, 2, output, position, left.size()));
        for (size_t frame = 0; frame < left.size(); ++frame) {
            const auto sample = position + static_cast<int64_t>(frame);
            const bool missing = (sample >= 128 && sample < 132) || (sample >= 256 && sample < 271);
            EXPECT_FLOAT_EQ(left[frame], missing ? 0.0f : static_cast<float>(sample + 1)) << sample;
            EXPECT_FLOAT_EQ(right[frame], left[frame]);
        }
        position += static_cast<int64_t>(left.size());
    }
    int64_t frontier = 0;
    for (const auto& [sample, frames] : processor->calls) {
        EXPECT_EQ(sample, frontier);
        frontier += static_cast<int64_t>(frames);
    }
    EXPECT_EQ(frontier, 335);
    EXPECT_EQ(processor->ons[60], 16u);
    EXPECT_EQ(processor->offs[60], 16u);
    EXPECT_EQ(processor->ons[80], 1u);
    EXPECT_EQ(processor->offs[80], 1u);
}

TEST(NativeLiveAudioTests, DelayedLoopWrapsProjectTimeWhileRenderingEachInputFrameAndHeldKeyOnlyOnce)
{
    auto track = MidiTrack::Create();
    ASSERT_TRUE(track->SetClips({ { 1, "Loop clock", 0, 1, 1,
                                    { { 1, 16.0 / 24000, 4.0 / 24000, 60, 80 }, { 2, 20.0 / 24000, 4.0 / 24000, 64, 80 },
                                        { 3, 24.0 / 24000, 32.0 / 24000, 67, 80 } }, {} } }));
    auto processor = std::make_shared<DelayedPositionInstrument>();
    processor->requestedDelay = 23;
    auto session = std::make_shared<LiveInstrumentSession>(track, processor);
    session->beginTimeline();
    MidiPlayableSequence sequence(track, session, 48000);
    sequence.SetPlaybackLoop(true, 16.0 / 48000, 24.0 / 48000);
    session->sendMidi(0x90, 80, 95);
    std::array<float, 8> left {}, right {};
    float* output[] { left.data(), right.data() };
    for (int loop = 0; loop < 32; ++loop) {
        ASSERT_TRUE(sequence.GetFloats(0, 2, output, 16, 8));
        for (size_t frame = 0; frame < 8; ++frame) {
            EXPECT_FLOAT_EQ(left[frame], 17.0f + frame) << loop;
        }
        EXPECT_EQ(left, right);
    }
    size_t total = 0;
    for (const auto& [sample, count] : processor->calls) {
        EXPECT_EQ(sample, 16 + static_cast<int64_t>(total % 8));
        EXPECT_LE(sample + static_cast<int64_t>(count), 24);
        total += count;
    }
    EXPECT_EQ(total, 256u + 23u);
    EXPECT_EQ(processor->ons[67], 0u);
    EXPECT_EQ(processor->ons[80], 1u);
    EXPECT_EQ(processor->offs[80], 0u);
    session->endTimeline();
    session->close();
}

TEST(NativeInstrumentTests, BuiltInInstrumentDelayRefreshDoesNotWaitForTheApplicationEventLoop)
{
    const auto processor = createBuiltinInstrument({});
    auto task = std::async(std::launch::async, [&] { return refreshInstrumentLatency(*processor); });
    const auto status = task.wait_for(std::chrono::seconds(1));
    // Drain an erroneously queued call before asserting, so a regression
    // reports a failure rather than hanging in the future's destructor.
    while (task.wait_for(std::chrono::milliseconds(1)) != std::future_status::ready) {
        QCoreApplication::processEvents();
    }
    EXPECT_EQ(status, std::future_status::ready);
    EXPECT_FALSE(task.get());
}

TEST(NativeInstrumentTests, NativeDelayControlFailureReturnsToItsWaitingWorkerFromTheApplicationThread)
{
    VoiceContinuityInstrument processor;
    processor.throwOnRefresh = true;
    const auto mainThread = std::this_thread::get_id();
    auto task = std::async(std::launch::async, [&] {
        EXPECT_THROW(refreshInstrumentLatency(processor), std::runtime_error);
    });
    while (task.wait_for(std::chrono::milliseconds(1)) != std::future_status::ready) {
        QCoreApplication::processEvents();
    }
    task.get();
    EXPECT_EQ(processor.refreshThread, mainThread);
    EXPECT_EQ(processor.refreshes, 1u);
}

TEST(NativeInstrumentTests, AnOfflineInstrumentWhoseDelayNeverSettlesFailsBeforeConsumingAnyNotes)
{
    const auto track = MidiTrack::Create();
    ASSERT_TRUE(track->SetClips({ { 1, "Clip", 0.0, 8.0, 1, { { 1, 0.0, 4.0, 60, 100 } }, {} } }));
    const auto processor = std::make_shared<VoiceContinuityInstrument>();
    processor->unstableLatency = true;
    MidiPlayableSequence sequence(track, processor, 48000.0);
    std::array<float, 32> left {}, right {};
    float* output[] { left.data(), right.data() };
    EXPECT_THROW(sequence.GetFloats(0, 2, output, 0, 32), std::runtime_error);
    EXPECT_GT(processor->refreshes, 0u);
    EXPECT_LT(processor->refreshes, 100u);
    EXPECT_EQ(processor->ons[60], 0);
    processor->unstableLatency = false;
    ASSERT_TRUE(sequence.GetFloats(0, 2, output, 0, 32));
    ASSERT_EQ(processor->contexts.size(), 3u);
    EXPECT_EQ(processor->contexts[1].continuousSample, 0);
    EXPECT_EQ(processor->contexts[2].continuousSample, 0);
    EXPECT_FLOAT_EQ(left.front(), 1.0f);
    EXPECT_FLOAT_EQ(left.back(), 32.0f);
}

TEST(NativeLiveAudioTests, MonitorTimelineMonitorHandoffPreservesHeldVoicePhaseAndTheAudioUnitRenderClock)
{
    const auto track = MidiTrack::Create();
    ASSERT_TRUE(track->SetClips({ { 1, "Clip", 0.0, 8.0, 1, { { 1, 0.0, 4.0, 64, 100 } }, {} } }));
    const auto processor = std::make_shared<VoiceContinuityInstrument>();
    const auto session = std::make_shared<LiveInstrumentSession>(track, processor);
    session->prepare(48000.0, 512);
    session->sendMidi(0x90, 60, 110);
    std::array<float, 32> left {}, right {};
    float* output[] { left.data(), right.data() };
    session->process(output, 2, 32, {}, { 0, 120.0, 4, 4, false });
    EXPECT_FLOAT_EQ(left.front(), 1.0f);
    EXPECT_FLOAT_EQ(left.back(), 32.0f);
    const auto resets = processor->resets;
    session->beginTimeline();
    MidiPlayableSequence sequence(track, session, 48000.0);
    std::thread worker([&] { EXPECT_TRUE(sequence.GetFloats(0, 2, output, 48000, 32)); });
    worker.join();
    EXPECT_FLOAT_EQ(left.front(), 33.0f);
    EXPECT_FLOAT_EQ(left.back(), 64.0f);
    EXPECT_TRUE(processor->held[64]);
    session->endTimeline();
    session->Prepare(48000.0);
    session->process(output, 2, 32, {}, { 0, 120.0, 4, 4, false });
    EXPECT_FLOAT_EQ(left.front(), 65.0f);
    EXPECT_FLOAT_EQ(left.back(), 96.0f);
    EXPECT_FALSE(processor->held[64]);
    EXPECT_EQ(processor->resets, resets);
    EXPECT_EQ(processor->ons[60], 1);
    EXPECT_EQ(processor->offs[60], 0);
    EXPECT_EQ(processor->offs[64], 1);
    ASSERT_EQ(processor->contexts.size(), 3u);
    EXPECT_EQ(processor->contexts[0].renderSample, 0);
    EXPECT_EQ(processor->contexts[1].renderSample, 32);
    EXPECT_EQ(processor->contexts[2].renderSample, 64);
    EXPECT_EQ(processor->contexts[1].sample, 48000);
    EXPECT_EQ(processor->contexts[1].continuousSample, 48000);
    session->sendMidi(0x80, 60, 0);
    session->process(output, 2, 32, {}, {});
    EXPECT_TRUE(std::all_of(left.begin(), left.end(), [](auto sample) { return sample == 0.0f; }));
}

TEST(NativeLiveAudioTests, SustainedAudioMatchesAnUninterruptedInstrumentAcrossMonitorTimelineHandoffs)
{
    const auto track = MidiTrack::Create();
    const auto session = std::make_shared<LiveInstrumentSession>(track, createBuiltinInstrument({}));
    const auto reference = createBuiltinInstrument({});
    session->prepare(48000.0, 512);
    reference->prepare(48000.0, 512);
    const std::array input { MidiEvent { 0, 0xb0, 64, 127 }, MidiEvent { 0, 0x90, 60, 110 }, MidiEvent { 0, 0x80, 60, 0 } };
    for (const auto& event : input) {
        session->sendMidi(event.status, event.data1, event.data2);
    }
    std::array<float, 32> left {}, right {}, expectedLeft {}, expectedRight {};
    float* output[] { left.data(), right.data() };
    float* expected[] { expectedLeft.data(), expectedRight.data() };
    session->process(output, 2, 32, {}, {});
    reference->process(expected, 2, 32, input, {});
    EXPECT_EQ(left, expectedLeft);
    session->beginTimeline();
    MidiPlayableSequence sequence(track, session, 48000.0, true, false);
    ASSERT_TRUE(sequence.GetFloats(0, 2, output, 0, 32));
    reference->process(expected, 2, 32, {}, {});
    EXPECT_EQ(left, expectedLeft);
    EXPECT_EQ(right, expectedRight);
    session->endTimeline();
    session->Prepare(48000.0);
    session->process(output, 2, 32, {}, {});
    reference->process(expected, 2, 32, {}, {});
    EXPECT_EQ(left, expectedLeft);
    EXPECT_EQ(right, expectedRight);
    session->sendMidi(0xb0, 64, 0);
    session->process(output, 2, 32, {}, {});
    const std::array pedalUp { MidiEvent { 0, 0xb0, 64, 0 } };
    reference->process(expected, 2, 32, pedalUp, {});
    ASSERT_EQ(left, expectedLeft);
    // Once the pedal is up, no held-input state remains to protect the voice.
    // Its release tail must still survive another complete ownership transfer.
    session->beginTimeline();
    MidiPlayableSequence releaseSequence(track, session, 48000.0, true, false);
    ASSERT_TRUE(releaseSequence.GetFloats(0, 2, output, 128, 32));
    reference->process(expected, 2, 32, {}, {});
    ASSERT_EQ(left, expectedLeft);
    session->endTimeline();
    session->Prepare(48000.0);
    session->process(output, 2, 32, {}, {});
    reference->process(expected, 2, 32, {}, {});
    ASSERT_EQ(left, expectedLeft);
    // Pedal-up starts the synth's release envelope; it is not a hard panic.
    for (int block = 0; block < 6001 && std::any_of(left.begin(), left.end(), [](auto sample) { return sample != 0.0f; }); ++block) {
        session->process(output, 2, 32, {}, {});
        reference->process(expected, 2, 32, {}, {});
        ASSERT_EQ(left, expectedLeft);
    }
    EXPECT_TRUE(std::all_of(left.begin(), left.end(), [](auto sample) { return sample == 0.0f; }));
}

TEST(NativeLiveAudioTests, HandoffsDoNotReloadTheLiveProgramWhenOnlyTimelineModulationChanged)
{
    const auto processor = std::make_shared<SessionProbeInstrument>();
    LiveInstrumentSession session(MidiTrack::Create(), processor);
    session.prepare(48000.0, 512);
    session.sendMidi(0xc0, 9, 0);
    session.sendMidi(0x90, 60, 100);
    std::array<float, 32> left {}, right {};
    float* output[] { left.data(), right.data() };
    session.process(output, 2, 32, {}, {});
    session.beginTimeline();
    session.prepare(48000.0, 512);
    const std::array modulation { MidiEvent { 0, 0xb0, 1, 63 } };
    session.process(output, 2, 32, modulation, {});
    EXPECT_FALSE(std::any_of(processor->events.begin(), processor->events.end(), [](const auto& event) {
        return (event.status & 0xf0) == 0xc0 || (event.status & 0xf0) == 0x90;
    }));
    session.endTimeline();
    session.prepare(48000.0, 512);
    session.process(output, 2, 32, {}, {});
    EXPECT_FALSE(std::any_of(processor->events.begin(), processor->events.end(), [](const auto& event) {
        return (event.status & 0xf0) == 0xc0 || (event.status & 0xf0) == 0x90;
    }));
    EXPECT_EQ(processor->resets, 0);
}

TEST(NativeLiveAudioTests, TimelineLoopsAndSeeksDoNotRetriggerAnAlreadyHeldKeyboardVoice)
{
    const auto track = MidiTrack::Create();
    ASSERT_TRUE(track->SetClips({ { 1, "Clip", 0.0, 8.0, 1, { { 1, 0.0, 4.0, 64, 100 } }, {} } }));
    const auto processor = std::make_shared<VoiceContinuityInstrument>();
    const auto session = std::make_shared<LiveInstrumentSession>(track, processor);
    const auto sequence = std::make_shared<MidiPlayableSequence>(track, session, 48000.0);
    session->beginTimeline();
    session->sendMidi(0x90, 60, 110);
    std::array<float, 32> left {}, right {};
    float* output[] { left.data(), right.data() };
    ASSERT_TRUE(sequence->GetFloats(0, 2, output, 0, 32));
    EXPECT_FLOAT_EQ(left.front(), 1.0f);
    EXPECT_FLOAT_EQ(left.back(), 32.0f);
    const auto resets = processor->resets;
    ASSERT_TRUE(sequence->GetFloats(0, 2, output, 0, 32)); // loop to start
    EXPECT_FLOAT_EQ(left.front(), 33.0f);
    EXPECT_FLOAT_EQ(left.back(), 64.0f);
    ASSERT_TRUE(sequence->GetFloats(0, 2, output, 48000, 32)); // seek into held clip note
    EXPECT_FLOAT_EQ(left.front(), 65.0f);
    EXPECT_FLOAT_EQ(left.back(), 96.0f);
    EXPECT_EQ(processor->resets, resets);
    EXPECT_EQ(processor->ons[60], 1);
    EXPECT_EQ(processor->offs[60], 0);
    EXPECT_EQ(processor->ons[64], 3);
    EXPECT_EQ(processor->offs[64], 2);
    session->sendMidi(0x80, 60, 0);
    ASSERT_TRUE(sequence->GetFloats(0, 2, output, 48032, 32));
    EXPECT_TRUE(std::all_of(left.begin(), left.end(), [](auto value) { return value == 0.0f; }));
    ASSERT_TRUE(sequence->GetFloats(0, 2, output, 144000, 32));
    EXPECT_GT(processor->resets, resets); // no live voice: normal hard reset
    EXPECT_FALSE(processor->held[64]);
}

TEST(NativeLiveAudioTests, OverlappingKeyboardAndTimelineKeysReleaseOnlyWhenBothAreUp)
{
    const auto track = MidiTrack::Create();
    const auto processor = std::make_shared<VoiceContinuityInstrument>();
    const auto session = std::make_shared<LiveInstrumentSession>(track, processor);
    session->prepare(48000.0, 512);
    std::array<float, 32> left {}, right {};
    float* output[] { left.data(), right.data() };
    const std::array on { MidiEvent { 0, 0x90, 60, 100 } };
    const std::array off { MidiEvent { 0, 0x80, 60, 0 } };
    session->sendMidi(0x90, 60, 100);
    session->process(output, 2, 32, on, {});
    EXPECT_EQ(processor->ons[60], 1);
    session->process(output, 2, 32, off, {});
    EXPECT_TRUE(processor->held[60]);
    EXPECT_EQ(processor->offs[60], 0);
    session->sendMidi(0x80, 60, 0);
    session->process(output, 2, 32, {}, {});
    EXPECT_FALSE(processor->held[60]);
    EXPECT_EQ(processor->offs[60], 1);
    session->process(output, 2, 32, on, {});
    session->sendMidi(0x90, 60, 100);
    session->process(output, 2, 32, {}, {});
    session->sendMidi(0x80, 60, 0);
    session->process(output, 2, 32, {}, {});
    EXPECT_TRUE(processor->held[60]);
    session->process(output, 2, 32, off, {});
    EXPECT_FALSE(processor->held[60]);
    EXPECT_EQ(processor->ons[60], 2);
    EXPECT_EQ(processor->offs[60], 2);
}

TEST(NativeLiveAudioTests, OrdinaryNoteAndPedalSeeksRestoreOnlyTheAffectedControllerParameters)
{
    auto processor = std::make_shared<SessionProbeInstrument>();
    LiveInstrumentSession session(MidiTrack::Create(), processor);
    session.prepare(48000, 512);
    session.beginTimeline();
    session.sendMidi(0x90, 60, 100);
    std::array<float, 32> samples {};
    float* output[] { samples.data() };
    session.process(output, 1, 32, {}, {});
    session.resetTimeline();
    session.process(output, 1, 32, {}, {});
    EXPECT_EQ(processor->manualRestores, 0);
    EXPECT_EQ(processor->controllerRestores[0][64], 0);
    const std::array pedal { MidiEvent { 3, 0xb0, 64, 127 } };
    session.process(output, 1, 32, pedal, {});
    session.resetTimeline();
    session.process(output, 1, 32, {}, {});
    EXPECT_EQ(processor->manualRestores, 0);
    EXPECT_EQ(processor->controllerRestores[0][64], 1);
    for (int cc = 0; cc < 130; ++cc) {
        if (cc != 64) {
            EXPECT_EQ(processor->controllerRestores[0][cc], 0) << cc;
        }
    }
}

TEST(NativeLiveAudioTests, SeekingRestoresPitchAndExpressionDefaultsWithoutInterruptingLiveSustain)
{
    for (const bool explicitLiveControls : { false, true }) {
        SCOPED_TRACE(explicitLiveControls);
        LiveInstrumentSession session(MidiTrack::Create(), createBuiltinInstrument({}));
        auto reference = createBuiltinInstrument({});
        session.prepare(48000, 512);
        reference->prepare(48000, 512);
        session.beginTimeline();
        std::array<float, 512> actual {}, expected {};
        float* output[] { actual.data() };
        float* compare[] { expected.data() };
        std::vector<MidiEvent> live;
        if (explicitLiveControls) {
            live = { { 0, 0xe0, 0, 80 }, { 0, 0xb0, 11, 80 } };
        }
        live.insert(live.end(), { { 0, 0xb0, 64, 127 }, { 0, 0x90, 60, 100 }, { 0, 0x80, 60, 0 } });
        for (const auto& e : live) {
            session.sendMidi(e.status, e.data1, e.data2);
        }
        session.process(output, 1, 512, {}, {});
        reference->process(compare, 1, 512, live, {});
        ASSERT_EQ(actual, expected);
        const std::array timeline { MidiEvent { 64, 0xe0, 0, 96 }, MidiEvent { 64, 0xb0, 11, 32 } };
        session.process(output, 1, 512, timeline, {});
        reference->process(compare, 1, 512, timeline, {});
        ASSERT_EQ(actual, expected);
        session.resetTimeline();
        const std::array restored { MidiEvent { 0, 0xe0, 0, static_cast<uint8_t>(explicitLiveControls ? 80 : 64) },
                                    MidiEvent { 0, 0xb0, 11, static_cast<uint8_t>(explicitLiveControls ? 80 : 127) } };
        session.process(output, 1, 512, {}, {});
        reference->process(compare, 1, 512, restored, {});
        EXPECT_EQ(actual, expected);
    }
}

TEST(NativeLiveAudioTests, SeekingPastTimelineSustainRetainsTheReleasedKeyboardVoicesTail)
{
    LiveInstrumentSession session(MidiTrack::Create(), createBuiltinInstrument({}));
    auto reference = createBuiltinInstrument({});
    session.prepare(48000, 512);
    reference->prepare(48000, 512);
    session.beginTimeline();
    std::array<float, 512> actual {}, expected {};
    float* output[] { actual.data() };
    float* compare[] { expected.data() };
    session.sendMidi(0x90, 60, 100);
    const std::array pedal { MidiEvent { 0, 0xb0, 64, 127 } };
    const std::array initial { MidiEvent { 0, 0x90, 60, 100 }, MidiEvent { 0, 0xb0, 64, 127 } };
    session.process(output, 1, 512, pedal, {});
    reference->process(compare, 1, 512, initial, {});
    session.sendMidi(0x80, 60, 0);
    const std::array off { MidiEvent { 0, 0x80, 60, 0 } };
    session.process(output, 1, 512, {}, {});
    reference->process(compare, 1, 512, off, {});
    ASSERT_EQ(actual, expected);
    session.resetTimeline();
    const std::array up { MidiEvent { 0, 0xb0, 64, 0 } };
    session.process(output, 1, 512, {}, {});
    reference->process(compare, 1, 512, up, {});
    EXPECT_EQ(actual, expected);
    EXPECT_GT(energy(std::vector<float>(actual.begin(), actual.end())), 0.01);
}

TEST(NativeLiveAudioTests, TimelineControllerResetKeepsTheLiveSustainPedalAndVoicePhase)
{
    LiveInstrumentSession session(MidiTrack::Create(), createBuiltinInstrument({}));
    auto reference = createBuiltinInstrument({});
    session.prepare(48000, 512);
    reference->prepare(48000, 512);
    session.beginTimeline();
    std::array<float, 512> actual {}, expected {};
    float* output[] { actual.data() };
    float* compare[] { expected.data() };
    const std::array live { MidiEvent { 0, 0xb0, 64, 127 }, MidiEvent { 0, 0x90, 60, 100 }, MidiEvent { 0, 0x80, 60, 0 } };
    for (const auto& e : live) {
        session.sendMidi(e.status, e.data1, e.data2);
    }
    session.process(output, 1, 512, {}, {});
    reference->process(compare, 1, 512, live, {});
    ASSERT_EQ(actual, expected);
    const std::array reset { MidiEvent { 173, 0xb0, 121, 0 } };
    session.process(output, 1, 512, reset, {});
    reference->process(compare, 1, 512, {}, {});
    EXPECT_EQ(actual, expected);
}

TEST(NativeLiveAudioTests, LiveControllerResetKeepsTheTimelineSustainPedalAndVoicePhase)
{
    LiveInstrumentSession session(MidiTrack::Create(), createBuiltinInstrument({}));
    auto reference = createBuiltinInstrument({});
    session.prepare(48000, 512);
    reference->prepare(48000, 512);
    session.beginTimeline();
    std::array<float, 512> actual {}, expected {};
    float* output[] { actual.data() };
    float* compare[] { expected.data() };
    const std::array timeline { MidiEvent { 0, 0xb0, 64, 127 }, MidiEvent { 0, 0x90, 60, 100 }, MidiEvent { 0, 0x80, 60, 0 } };
    session.process(output, 1, 512, timeline, {});
    reference->process(compare, 1, 512, timeline, {});
    ASSERT_EQ(actual, expected);
    session.sendMidi(0xb0, 121, 0);
    session.process(output, 1, 512, {}, {});
    reference->process(compare, 1, 512, {}, {});
    EXPECT_EQ(actual, expected);
}

TEST(NativeLiveAudioTests, TimelineChannelModesReleaseOnlyTheirOwnKeysAndKeepLiveVoicePhase)
{
    for (const int mode : { 120, 123, 124, 125, 126, 127 }) {
        for (const bool sharedPitch : { false, true }) {
            SCOPED_TRACE(testing::Message() << "mode=" << mode << " shared pitch=" << sharedPitch);
            auto track = MidiTrack::Create();
            LiveInstrumentSession session(track, createBuiltinInstrument({}));
            auto reference = createBuiltinInstrument({});
            session.prepare(48000, 512);
            reference->prepare(48000, 512);
            session.beginTimeline();
            std::array<float, 512> actual {}, expected {};
            float* output[] { actual.data() };
            float* compare[] { expected.data() };
            session.sendMidi(0x90, 60, 100);
            const auto pitch = static_cast<uint8_t>(sharedPitch ? 60 : 64);
            const std::array timeline { MidiEvent { 0, 0x90, pitch, 100 } };
            const std::array initial { MidiEvent { 0, 0x90, 60, 100 }, MidiEvent { 0, 0x90, 64, 100 } };
            session.process(output, 1, 512, timeline, {});
            reference->process(compare, 1, 512, std::span(initial.data(), sharedPitch ? 1 : 2), {});
            ASSERT_EQ(actual, expected);
            const std::array command { MidiEvent { 173, 0xb0, static_cast<uint8_t>(mode), 0 } };
            const std::array release { MidiEvent { 173, 0x80, 64, 0 } };
            session.process(output, 1, 512, command, {});
            reference->process(compare, 1, 512, sharedPitch ? std::span<const MidiEvent> {} : std::span(release), {});
            EXPECT_EQ(actual, expected); // Exact audio, including the live oscillator's phase.
            // A following keyboard release must remain effective and must not
            // be swallowed by a stale timeline key left by the channel mode.
            session.sendMidi(0x80, 60, 0);
            const std::array releaseLive { MidiEvent { 0, 0x80, 60, 0 } };
            session.process(output, 1, 512, {}, {});
            reference->process(compare, 1, 512, releaseLive, {});
            EXPECT_EQ(actual, expected);
        }
    }
}

TEST(NativeLiveAudioTests, TimelineChannelModesCannotDropLiveSustainOrReleaseStillHeldKeysWhenThePedalRises)
{
    for (const int mode : { 120, 123, 124, 125, 126, 127 }) {
        SCOPED_TRACE(mode);
        LiveInstrumentSession session(MidiTrack::Create(), createBuiltinInstrument({}));
        auto reference = createBuiltinInstrument({});
        session.prepare(48000, 512);
        reference->prepare(48000, 512);
        session.beginTimeline();
        std::array<float, 512> actual {}, expected {};
        float* output[] { actual.data() };
        float* compare[] { expected.data() };
        const std::array live { MidiEvent { 0, 0xb0, 64, 127 }, MidiEvent { 0, 0x90, 60, 100 },
                                MidiEvent { 0, 0x90, 62, 90 }, MidiEvent { 0, 0x80, 62, 0 } };
        for (const auto& event : live) {
            session.sendMidi(event.status, event.data1, event.data2);
        }
        session.process(output, 1, 512, {}, {});
        reference->process(compare, 1, 512, live, {});
        ASSERT_EQ(actual, expected);
        const std::array command { MidiEvent { 173, 0xb0, static_cast<uint8_t>(mode), 0 } };
        session.process(output, 1, 512, command, {});
        reference->process(compare, 1, 512, {}, {});
        EXPECT_EQ(actual, expected);
        session.sendMidi(0xb0, 64, 0);
        const std::array pedalUp { MidiEvent { 0, 0xb0, 64, 0 } };
        session.process(output, 1, 512, {}, {});
        reference->process(compare, 1, 512, pedalUp, {});
        EXPECT_EQ(actual, expected);
        // Only the sustained D releases; C remains physically held throughout.
        for (int i = 0; i < 50; ++i) {
            session.process(output, 1, 512, {}, {});
            reference->process(compare, 1, 512, {}, {});
            ASSERT_EQ(actual, expected);
        }
    }
}

TEST(NativeMidiTests, SeekingAfterChannelModeNoteOffDoesNotResurrectItsKeys)
{
    for (const int mode : { 120, 123, 124, 125, 126, 127 }) {
        for (const bool sustain : { false, true }) {
            SCOPED_TRACE(testing::Message() << "mode=" << mode << " sustain=" << sustain);
            auto track = MidiTrack::Create();
            track->SetTempo(120);
            std::vector<MidiControllerData> controllers { { 1, mode, 0 } };
            if (sustain) {
                controllers.insert(controllers.begin(), { 0, 64, 127 });
            }
            ASSERT_TRUE(track->SetClips({ { 1, "Channel modes", 0, 4, 1, { { 1, 0, 4, 60, 100 } }, controllers } }));
            MidiSequence sequence(*track, 48000);
            std::vector<MidiEvent> chased;
            sequence.Chase(48000, chased);
            const auto ons = std::count_if(chased.begin(), chased.end(), [](const auto& event) { return event.status == 0x90; });
            const auto offs = std::count_if(chased.begin(), chased.end(), [](const auto& event) { return event.status == 0x80; });
            EXPECT_EQ(ons, sustain && mode != 120 ? 1 : 0);
            EXPECT_EQ(offs, ons); // Sustained voices are chased without held keys.
        }
    }
}

TEST(NativeLiveAudioTests, TimelineAllSoundOffOnAnotherChannelKeepsItsHardSilenceSemantics)
{
    LiveInstrumentSession session(MidiTrack::Create(), createBuiltinInstrument({}));
    auto reference = createBuiltinInstrument({});
    session.prepare(48000, 512);
    reference->prepare(48000, 512);
    session.beginTimeline();
    std::array<float, 512> actual {}, expected {};
    float* output[] { actual.data() };
    float* compare[] { expected.data() };
    session.sendMidi(0x90, 60, 100);
    const std::array timeline { MidiEvent { 0, 0x91, 64, 100 } };
    const std::array initial { MidiEvent { 0, 0x90, 60, 100 }, MidiEvent { 0, 0x91, 64, 100 } };
    session.process(output, 1, 512, timeline, {});
    reference->process(compare, 1, 512, initial, {});
    ASSERT_EQ(actual, expected);
    const std::array stop { MidiEvent { 173, 0xb1, 120, 0 } };
    session.process(output, 1, 512, stop, {});
    reference->process(compare, 1, 512, stop, {});
    EXPECT_EQ(actual, expected);
    // A seek may reset the document's state but must not retrigger the live key.
    session.resetTimeline();
    session.process(output, 1, 512, {}, {});
    reference->process(compare, 1, 512, {}, {});
    EXPECT_EQ(actual, expected);
}

TEST(NativeLiveAudioTests, MidiTimelineSuppliesItsTempoAndProjectMeterToTheInstrument)
{
    const auto track = MidiTrack::Create();
    track->SetTempo(93.0);
    const auto probe = std::make_shared<SessionProbeInstrument>();
    MidiPlayableSequence sequence(track, probe, 48000.0, true, false, 7, 8);
    std::array<float, 32> left {}, right {};
    float* output[] { left.data(), right.data() };
    ASSERT_TRUE(sequence.GetFloats(0, 2, output, 48000, 32));
    EXPECT_EQ(probe->transport.sample, 48000);
    EXPECT_DOUBLE_EQ(probe->transport.tempo, 93.0);
    EXPECT_EQ(probe->transport.numerator, 7);
    EXPECT_EQ(probe->transport.denominator, 8);
    EXPECT_TRUE(probe->transport.playing);
    EXPECT_FALSE(probe->transport.cycling);
    sequence.SetPlaybackLoop(true, 0.5, 1.5);
    ASSERT_TRUE(sequence.GetFloats(0, 2, output, 48032, 32));
    EXPECT_TRUE(probe->transport.cycling);
    EXPECT_DOUBLE_EQ(probe->transport.cycleStart, 0.5);
    EXPECT_DOUBLE_EQ(probe->transport.cycleEnd, 1.5);
    sequence.SetPlaybackLoop(false, 0.75, 2.0);
    ASSERT_TRUE(sequence.GetFloats(0, 2, output, 48064, 32));
    EXPECT_FALSE(probe->transport.cycling);
    EXPECT_DOUBLE_EQ(probe->transport.cycleStart, 0.75);
    EXPECT_DOUBLE_EQ(probe->transport.cycleEnd, 2.0);
    // MIDI event positions and plugin tempo use the same immutable snapshot.
    track->SetTempo(110.0);
    ASSERT_TRUE(sequence.GetFloats(0, 2, output, 48096, 32));
    EXPECT_DOUBLE_EQ(probe->transport.tempo, 93.0);
}

TEST(NativeLiveAudioTests, UnboundedRecordingDoesNotConvertInactiveLoopBoundsToSamples)
{
    const auto track = MidiTrack::Create();
    const auto probe = std::make_shared<SessionProbeInstrument>();
    MidiPlayableSequence sequence(track, probe, 48000.0, true, false);
    std::array<float, 32> left {}, right {};
    float* output[] { left.data(), right.data() };
    // Original AudioIO passes DBL_MAX as the open-ended recording range,
    // including when a MIDI track is an overdub's playback source.
    const auto end = std::numeric_limits<double>::max();
    ASSERT_NO_THROW(sequence.SetPlaybackLoop(false, 0, end));
    ASSERT_TRUE(sequence.GetFloats(0, 2, output, 0, 32));
    EXPECT_FALSE(probe->transport.cycling);
    EXPECT_EQ(probe->transport.sample, 0);
    ASSERT_TRUE(sequence.GetFloats(0, 2, output, 32, 32));
    EXPECT_EQ(probe->transport.sample, 32);
    EXPECT_THROW(sequence.SetPlaybackLoop(true, 0, end), std::out_of_range);
    ASSERT_NO_THROW(sequence.SetPlaybackLoop(true, 0, 1));
    ASSERT_NO_THROW(sequence.SetPlaybackLoop(false, 0, end));
    ASSERT_TRUE(sequence.GetFloats(0, 2, output, 64, 32));
    EXPECT_FALSE(probe->transport.cycling);
    EXPECT_EQ(probe->transport.sample, 64);
}

TEST(NativeLiveAudioTests, EditorTimelineAndMonitoringReuseOneProcessorWithoutReinitializingIt)
{
    const auto track = MidiTrack::Create();
    auto processor = std::make_shared<SessionProbeInstrument>();
    auto session = std::make_shared<LiveInstrumentSession>(track, processor);
    session->Prepare(48000.0);
    LiveCallback callback;
    callback.SetLiveOutput(session);
    std::array<float, 1024> output {};
    session->sendMidi(0x90, 69, 94);
    callback.render(output.data(), output.data(), 512);
    EXPECT_FALSE(processor->playing);
    EXPECT_TRUE(std::find(processor->events.begin(), processor->events.end(), MidiEvent { 0, 0x90, 69, 94 }) != processor->events.end());
    callback.SetLiveOutput({});
    session->beginTimeline();
    session->setParameter("gain", 0.25);
    const auto sequence = std::make_shared<MidiPlayableSequence>(track, session, 48000.0);
    std::thread worker([&] {
        std::array<float, 512> left {}, right {};
        float* buffers[] { left.data(), right.data() };
        EXPECT_TRUE(sequence->GetFloats(0, 2, buffers, 0, 512));
        EXPECT_TRUE(std::all_of(left.begin(), left.end(), [](float sample) { return sample == 0.25f; }));
    });
    worker.join();
    EXPECT_TRUE(processor->playing);
    EXPECT_NE(processor->processingThread, std::this_thread::get_id());
    EXPECT_EQ(processor->prepares, 1);
    EXPECT_FALSE(std::find(processor->events.begin(), processor->events.end(), MidiEvent { 0, 0x90, 69, 94 }) != processor->events.end());
    session->endTimeline();
    session->Prepare(48000.0);
    callback.SetLiveOutput(session);
    output.fill(0.0f);
    callback.render(output.data(), output.data(), 512);
    EXPECT_FALSE(processor->playing);
    EXPECT_EQ(processor->processingThread, std::this_thread::get_id());
    EXPECT_EQ(processor->prepares, 1);
    EXPECT_EQ(processor->resets, 0);
    callback.SetLiveOutput({});
}

TEST(NativeLiveAudioTests, InputOnlyTimelineSourceDoesNotStartItsClipsAndStillReceivesKeyboardNotes)
{
    const auto track = MidiTrack::Create();
    ASSERT_TRUE(track->SetClips({ { 1, "Clip", 0.0, 4.0, 1, { { 1, 0.0, 1.0, 60, 100 } }, {} } }));
    const auto processor = std::make_shared<SessionProbeInstrument>();
    const auto session = std::make_shared<LiveInstrumentSession>(track, processor);
    session->beginTimeline();
    MidiPlayableSequence sequence(track, session, 48000.0, true, false);
    session->sendMidi(0x90, 72, 90);
    std::array<float, 512> left {}, right {};
    float* buffers[] { left.data(), right.data() };
    ASSERT_TRUE(sequence.GetFloats(0, 2, buffers, 0, 512));
    EXPECT_TRUE(std::find(processor->events.begin(), processor->events.end(), MidiEvent { 0, 0x90, 72, 90 }) != processor->events.end());
    EXPECT_FALSE(std::any_of(processor->events.begin(), processor->events.end(), [](const MidiEvent& event) {
        return event.status == 0x90 && event.data1 == 60;
    }));
    session->endTimeline();
}

TEST(NativeLiveAudioTests, ClosingAnEditorReleasesItsInputWithoutSendingAllNotesOffToTheTimeline)
{
    const auto track = MidiTrack::Create();
    const auto processor = std::make_shared<SessionProbeInstrument>();
    const auto session = std::make_shared<LiveInstrumentSession>(track, processor);
    session->prepare(48000.0, 512);
    session->beginTimeline();
    std::array<float, 32> left {}, right {};
    float* buffers[] { left.data(), right.data() };
    session->sendMidi(0x90, 69, 100);
    session->process(buffers, 2, 32, {}, {});
    session->close();
    const std::array timeline { MidiEvent { 8, 0x90, 60, 100 } };
    session->process(buffers, 2, 32, timeline, {});
    EXPECT_FALSE(session->active());
    EXPECT_TRUE(std::find(processor->events.begin(), processor->events.end(), MidiEvent { 0, 0x80, 69, 0 }) != processor->events.end());
    EXPECT_EQ(processor->events.back(), timeline[0]);
    EXPECT_FALSE(std::any_of(processor->events.begin(), processor->events.end(), [](const MidiEvent& event) {
        return (event.status & 0xf0) == 0xb0 && (event.data1 == 120 || event.data1 == 123);
    }));
    session->endTimeline();
}

TEST(NativeLiveAudioTests, SustainProgramPitchAndPressureAreChasedWhenTheAudioFormatChanges)
{
    const auto track = MidiTrack::Create();
    const auto processor = std::make_shared<SessionProbeInstrument>();
    const auto session = std::make_shared<LiveInstrumentSession>(track, processor);
    session->prepare(48000.0, 512);
    std::array<float, 32> left {}, right {};
    float* output[] { left.data(), right.data() };
    for (const auto event : { MidiEvent { 0, 0xb2, 0, 4 }, MidiEvent { 0, 0xc2, 9, 0 }, MidiEvent { 0, 0xe2, 12, 90 },
                              MidiEvent { 0, 0xd2, 70, 0 }, MidiEvent { 0, 0xb2, 64, 127 }, MidiEvent { 0, 0x92, 60, 93 },
                              MidiEvent { 0, 0xa2, 60, 64 }, MidiEvent { 0, 0x82, 60, 0 } }) {
        session->sendMidi(event.status, event.data1, event.data2);
    }
    session->process(output, 2, 32, {}, {});
    session->beginTimeline();
    session->prepare(44100.0, 512);
    session->process(output, 2, 32, {}, {});
    const auto& events = processor->events;
    const auto find = [&](MidiEvent event) { return std::find(events.begin(), events.end(), event); };
    EXPECT_NE(find({ 0, 0xb2, 0, 4 }), events.end());
    EXPECT_NE(find({ 0, 0xc2, 9, 0 }), events.end());
    EXPECT_LT(find({ 0, 0xb2, 0, 4 }), find({ 0, 0xc2, 9, 0 }));
    EXPECT_NE(find({ 0, 0xe2, 12, 90 }), events.end());
    EXPECT_NE(find({ 0, 0xd2, 70, 0 }), events.end());
    EXPECT_NE(find({ 0, 0xb2, 64, 127 }), events.end());
    EXPECT_NE(find({ 0, 0x92, 60, 93 }), events.end());
    EXPECT_NE(find({ 0, 0xa2, 60, 64 }), events.end());
    EXPECT_NE(find({ 0, 0x82, 60, 0 }), events.end());
    session->sendMidi(0xb2, 64, 0);
    session->process(output, 2, 32, {}, {});
    session->reset();
    session->process(output, 2, 32, {}, {});
    EXPECT_EQ(find({ 0, 0x92, 60, 93 }), events.end());
    session->endTimeline();
}

TEST(NativeLiveAudioTests, InstrumentAutomationDoesNotCreateManualEditorChanges)
{
    const auto instrument = createBuiltinInstrument({});
    instrument->prepare(48000.0, 512);
    EXPECT_TRUE(instrument->setAutomatedParameter("0", 0.25));
    EXPECT_FALSE(instrument->takeChanges());
    EXPECT_TRUE(instrument->setParameter("0", 0.5));
    EXPECT_TRUE(instrument->takeChanges());
}

TEST(NativeLiveAudioTests, InstrumentAutomationChangesSoundWithoutChangingTheSavedManualBaseline)
{
    auto instrument = createBuiltinInstrument({});
    instrument->prepare(48000.0, 512);
    ASSERT_TRUE(instrument->setParameter("0", 0.8));
    instrument->takeChanges();
    const auto saved = instrument->state();
    ASSERT_TRUE(instrument->setAutomatedParameter("0", 0.2));
    auto reference = createBuiltinInstrument("{\"0\":0.2}");
    reference->prepare(48000.0, 512);
    std::array<float, 512> left {}, right {}, expectedLeft {}, expectedRight {};
    float* output[] { left.data(), right.data() };
    float* expected[] { expectedLeft.data(), expectedRight.data() };
    const std::array events { MidiEvent { 0, 0x90, 60, 127 } };
    instrument->process(output, 2, 512, events, {});
    reference->process(expected, 2, 512, events, {});
    EXPECT_EQ(left, expectedLeft);
    EXPECT_EQ(instrument->state(), saved);
    EXPECT_DOUBLE_EQ(instrument->parameters()[0].value, 0.8);
    EXPECT_FALSE(instrument->takeChanges());
    instrument->restoreManualParameters();
    ASSERT_TRUE(reference->setParameter("0", 0.8));
    instrument->process(output, 2, 512, {}, { 512 });
    reference->process(expected, 2, 512, {}, { 512 });
    EXPECT_EQ(left, expectedLeft);
}

#if AU_NATIVE_TEST_AUDIO_UNIT
namespace {
std::atomic<unsigned> disposedTestUnits { 0 };
std::atomic<bool> testUnitDisposedOffMain { false };
AudioComponentPlugInInterface* disposalTestFactory(const AudioComponentDescription*)
{
    return new AudioComponentPlugInInterface {
        [](void*, AudioComponentInstance) -> OSStatus { return noErr; },
        [](void* self) -> OSStatus {
            if (CFRunLoopGetCurrent() != CFRunLoopGetMain()) {
                testUnitDisposedOffMain = true;
            }
            delete static_cast<AudioComponentPlugInInterface*>(self);
            ++disposedTestUnits;
            return noErr;
        },
        [](SInt16) -> AudioComponentMethod { return nullptr; },
        nullptr
    };
}
}

TEST(NativeAudioUnitInstrumentTests, DisposalCompletesOnMainAndMarshalsOnlyBackgroundDestruction)
{
    // Process-local component: verify actual AudioComponentInstance disposal
    // without requiring an installed third-party plugin or a hardware device.
    const AudioComponentDescription description { kAudioUnitType_MusicDevice, 'nQad', 'AuQA', 0, 0 };
    const auto component = AudioComponentRegister(&description, CFSTR("Audacity: Disposal Fixture"), 1, disposalTestFactory);
    ASSERT_NE(component, nullptr);
    AudioUnitWrapper::Parameters parameters;
    const auto before = disposedTestUnits.load();
    testUnitDisposedOffMain = false;
    auto unit = std::make_unique<AudioUnitWrapper>(component, &parameters);
    ASSERT_TRUE(unit->CreateAudioUnit());
    unit.reset();
    // Validation may exit as soon as the wrapper dies. Main-thread cleanup
    // must finish before plugin static destructors can run at process exit.
    EXPECT_EQ(disposedTestUnits.load(), before + 1);
    unit = std::make_unique<AudioUnitWrapper>(component, &parameters);
    ASSERT_TRUE(unit->CreateAudioUnit());
    std::thread destroy([owned = std::move(unit)]() mutable { owned.reset(); });
    destroy.join();
    EXPECT_EQ(disposedTestUnits.load(), before + 1);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (disposedTestUnits.load() != before + 2 && std::chrono::steady_clock::now() < deadline) {
        CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.01, true);
    }
    EXPECT_EQ(disposedTestUnits.load(), before + 2);
    EXPECT_FALSE(testUnitDisposedOffMain.load());
}

TEST(NativeAudioUnitInstrumentTests, AppleMusicDeviceReceivesMidiAndRendersAudioWithoutOpeningAnAudioDevice)
{
    auto instrument = createAudioUnitInstrument("appl/aumu/dls /Apple: DLSMusicDevice", {});
    instrument->prepare(48000.0, 512);
    std::vector<float> left(512), right(512);
    float* output[] { left.data(), right.data() };
    const std::array events { MidiEvent { 128, 0x90, 60, 100 } };
    instrument->process(output, 2, 512, events, {});
    EXPECT_TRUE(std::all_of(left.begin(), left.begin() + 128, [](float sample) { return sample == 0.0f; }));
    double total = energy(left);
    for (int block = 1; block < 40; ++block) {
        instrument->process(output, 2, 512, {}, { block* 512 });
        total += energy(left);
    }
    EXPECT_GT(total, 0.001);
}

TEST(NativeAudioUnitInstrumentTests, StateKeepsParametersChangedBeforePlayback)
{
    auto original = createAudioUnitInstrument("appl/aumu/dls /Apple: DLSMusicDevice", {});
    const auto parameters = original->parameters();
    ASSERT_FALSE(parameters.empty());
    ASSERT_TRUE(original->setParameter(parameters.front().id, 0.37));
    auto restored = createAudioUnitInstrument("appl/aumu/dls /Apple: DLSMusicDevice", original->state());
    const auto values = restored->parameters();
    ASSERT_EQ(values.size(), parameters.size());
    EXPECT_NEAR(values.front().value, 0.37, 1e-6);
    restored->prepare(48000.0, 512);
}

TEST(NativeAudioUnitInstrumentTests, AutomationDoesNotMarkTheNativeInstrumentAsManuallyEdited)
{
    const auto instrument = createAudioUnitInstrument("appl/aumu/dls /Apple: DLSMusicDevice", {});
    const auto parameters = instrument->parameters();
    ASSERT_FALSE(parameters.empty());
    instrument->takeChanges();
    EXPECT_TRUE(instrument->setAutomatedParameter(parameters.front().id, 0.25));
    EXPECT_FALSE(instrument->takeChanges());
    EXPECT_TRUE(instrument->setParameter(parameters.front().id, 0.5));
    EXPECT_TRUE(instrument->takeChanges());
}

TEST(NativeAudioUnitInstrumentTests, AutomatedDspValuesDoNotReplaceManualParametersInNativeSavedState)
{
    auto instrument = createAudioUnitInstrument("appl/aumu/dls /Apple: DLSMusicDevice", {});
    const auto id = instrument->parameters().front().id;
    instrument->prepare(48000.0, 512);
    ASSERT_TRUE(instrument->setParameter(id, 0.37));
    instrument->takeChanges();
    ASSERT_TRUE(instrument->setAutomatedParameter(id, 0.25));
    std::array<float, 512> left {}, right {};
    float* output[] { left.data(), right.data() };
    instrument->process(output, 2, 512, {}, {});
    for (int i = 0; i < 6; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        QCoreApplication::processEvents();
    }
    EXPECT_FALSE(instrument->takeChanges());
    EXPECT_NEAR(instrument->parameters().front().value, 0.37, 1e-6);
    auto restored = createAudioUnitInstrument("appl/aumu/dls /Apple: DLSMusicDevice", instrument->state());
    EXPECT_NEAR(restored->parameters().front().value, 0.37, 1e-6);
}

TEST(NativeAudioUnitInstrumentTests, StoppedProjectPositionDoesNotRepeatTheAudioUnitRenderTimestamp)
{
    auto instrument = createAudioUnitInstrument("appl/aumu/dls /Apple: DLSMusicDevice", {});
    instrument->prepare(48000.0, 512);
    std::vector<float> left(512), right(512), previous;
    float* output[] { left.data(), right.data() };
    const std::array events { MidiEvent { 0, 0x90, 60, 100 } };
    InstrumentProcessContext context { 60000, 93.0, 7, 8, false };
    context.continuousSample = 60000;
    double total = 0.0;
    bool changed = false;
    for (int block = 0; block < 40; ++block) {
        context.renderSample = block * 512;
        instrument->process(output, 2, 512, block ? std::span<const MidiEvent> {} : std::span<const MidiEvent>(events), context);
        total += energy(left);
        if (!previous.empty() && left != previous) {
            changed = true;
        }
        previous = left;
    }
    EXPECT_GT(total, 0.001);
    EXPECT_TRUE(changed);
}

TEST(NativeAudioUnitInstrumentTests, MusicDeviceDiscoveryClassifiesTheComponentAsAnInstrument)
{
    AudioUnitEffectsModule provider;
    TranslatableString error;
    int instruments = 0;
    PluginID id;
    ASSERT_EQ(provider.DiscoverPluginsAtPath("appl/aumu/dls /Apple: DLSMusicDevice", error,
                                             [&](PluginProvider*, ComponentInterface* component) -> const PluginID& {
        const auto* effect = dynamic_cast<EffectDefinitionInterface*>(component);
        EXPECT_NE(effect, nullptr);
        if (effect && effect->GetType() == EffectTypeInstrument) {
            ++instruments;
        }
        return id;
    }), 1u);
    EXPECT_EQ(instruments, 1);
}

TEST(NativeAudioUnitInstrumentTests, NativeEffectHostCallbacksReceiveTempoMeterAndTransportChanges)
{
    AudioUnitEffectsModule provider;
    auto component = provider.LoadPlugin("appl/aufx/lpas/Apple: AULowpass");
    auto* effect = dynamic_cast<AudioUnitEffectBase*>(component.get());
    ASSERT_NE(effect, nullptr);
    auto settings = effect->MakeSettings();
    const auto instance = effect->MakeInstance();
    instance->SetBlockSize(64);
    ASSERT_TRUE(instance->RealtimeInitialize(settings, 48000.0, 64));
    ASSERT_TRUE(instance->RealtimeAddProcessor(settings, nullptr, 2, 48000.0));
    ASSERT_TRUE(instance->RealtimeResume());
    const auto unit = std::dynamic_pointer_cast<AudioUnitInstance>(instance);
    ASSERT_TRUE(unit);
    HostCallbackInfo callbacks {};
    ASSERT_EQ(unit->GetFixedSizeProperty(kAudioUnitProperty_HostCallbacks, callbacks), noErr);
    ASSERT_TRUE(callbacks.beatAndTempoProc && callbacks.musicalTimeLocationProc && callbacks.transportStateProc);
    instance->SetRealtimeProcessingContext(0, { -0.25, 48000.0, 93.0, 7, 8, 0.0, false });
    Float64 beat = 0.0, tempo = 0.0, downbeat = 0.0, sample = 0.0;
    Float32 numerator = 0.0;
    UInt32 denominator = 0, nextBeat = 0;
    Boolean playing = true, changed = false, cycling = true;
    EXPECT_EQ(callbacks.beatAndTempoProc(callbacks.hostUserData, &beat, &tempo), noErr);
    EXPECT_DOUBLE_EQ(tempo, 93.0);
    EXPECT_DOUBLE_EQ(beat, -0.3875);
    EXPECT_EQ(callbacks.musicalTimeLocationProc(callbacks.hostUserData, &nextBeat, &numerator, &denominator, &downbeat), noErr);
    EXPECT_FLOAT_EQ(numerator, 7.0f);
    EXPECT_EQ(denominator, 8u);
    EXPECT_DOUBLE_EQ(downbeat, -3.5);
    EXPECT_EQ(nextBeat, 12000u);
    EXPECT_EQ(callbacks.transportStateProc(callbacks.hostUserData, &playing, &changed, &sample, &cycling, nullptr, nullptr), noErr);
    EXPECT_FALSE(playing);
    EXPECT_TRUE(changed);
    EXPECT_FALSE(cycling);
    EXPECT_DOUBLE_EQ(sample, -12000.0);
    std::array<float, 64> silence {}, left {}, right {};
    const float* in[] { silence.data(), silence.data() };
    float* out[] { left.data(), right.data() };
    EffectInstance::MessagePackage package { settings, nullptr };
    ASSERT_TRUE(instance->RealtimeProcessStart(package));
    ASSERT_EQ(instance->RealtimeProcess(0, settings, in, out, 64), 64u);
    ASSERT_TRUE(instance->RealtimeProcessEnd(settings));
    instance->SetRealtimeProcessingContext(0, { -0.25, 48000.0, 93.0, 7, 8, 0.0, false });
    callbacks.transportStateProc(callbacks.hostUserData, &playing, &changed, nullptr, nullptr, nullptr, nullptr);
    EXPECT_FALSE(changed);
    instance->SetRealtimeProcessingContext(0, { 2.0, 48000.0, 93.0, 7, 8, 1.0, true });
    callbacks.transportStateProc(callbacks.hostUserData, &playing, &changed, &sample, nullptr, nullptr, nullptr);
    EXPECT_TRUE(playing);
    EXPECT_TRUE(changed);
    EXPECT_DOUBLE_EQ(sample, 96000.0);
    Float64 cycleStart = 0.0, cycleEnd = 0.0;
    instance->SetRealtimeProcessingContext(0, { 2.0, 48000.0, 93.0, 7, 8, 1.0, true, true, 0.5, 1.5 });
    callbacks.transportStateProc(callbacks.hostUserData, &playing, &changed, &sample, &cycling, &cycleStart, &cycleEnd);
    EXPECT_TRUE(cycling);
    EXPECT_TRUE(changed);
    EXPECT_DOUBLE_EQ(cycleStart, 0.775);
    EXPECT_DOUBLE_EQ(cycleEnd, 2.325);
    ASSERT_TRUE(instance->RealtimeFinalize(settings));
}

TEST(NativeAudioUnitInstrumentTests, LatencyEventsAreDeferredUntilTheMainThreadRefreshAndUnchangedDelayKeepsItsRevision)
{
    AudioUnitEffectsModule provider;
    auto component = provider.LoadPlugin("appl/aufx/lpas/Apple: AULowpass");
    auto* effect = dynamic_cast<AudioUnitEffectBase*>(component.get());
    ASSERT_NE(effect, nullptr);
    auto settings = effect->MakeSettings();
    const auto instance = effect->MakeInstance();
    ASSERT_TRUE(instance->RealtimeInitialize(settings, 48000.0, 64));
    ASSERT_TRUE(instance->RealtimeAddProcessor(settings, nullptr, 2, 48000.0));
    ASSERT_TRUE(instance->RealtimeResume());
    const auto unit = std::dynamic_pointer_cast<AudioUnitInstance>(instance);
    ASSERT_TRUE(unit);
    const auto revision = instance->RealtimeLatencyRevision();
    const auto delay = instance->GetLatency(settings, 48000.0);
    AudioUnitEvent event {};
    event.mEventType = kAudioUnitEvent_PropertyChange;
    event.mArgument.mProperty.mPropertyID = kAudioUnitProperty_Latency;
    std::thread notification([&] { unit->EventListener(&event, 0.0f); });
    notification.join();
    EXPECT_TRUE(instance->RealtimeLatencyUpdatePending());
    EXPECT_EQ(instance->RealtimeLatencyRevision(), revision);
    EXPECT_EQ(instance->GetLatency(settings, 48000.0), delay);
    EXPECT_FALSE(instance->RefreshRealtimeLatency());
    EXPECT_FALSE(instance->RealtimeLatencyUpdatePending());
    EXPECT_EQ(instance->RealtimeLatencyRevision(), revision);
    ASSERT_TRUE(instance->RealtimeFinalize(settings));
}

TEST(NativeAudioUnitInstrumentTests, EffectAutomationReadsCurrentEffectSettingsWithoutChangingTheAudioUnit)
{
    AudioUnitEffectsModule provider;
    auto component = provider.LoadPlugin("appl/aufx/lpas/Apple: AULowpass");
    auto* effect = dynamic_cast<AudioUnitEffectBase*>(component.get());
    ASSERT_NE(effect, nullptr);
    auto settings = effect->MakeSettings();
    const auto original = nativeEffectParameters(*effect, settings);
    ASSERT_FALSE(original.empty());
    const auto id = static_cast<AudioUnitParameterID>(std::stoul(original.front().id));
    AudioUnitWrapper::ParameterInfo info(effect->GetAudioUnit(), id);
    auto& value = AudioUnitWrapper::GetSettings(settings).values.at(id);
    ASSERT_TRUE(value);
    value->second = info.mInfo.minValue + 0.37f * (info.mInfo.maxValue - info.mInfo.minValue);
    AudioUnitParameterValue before = 0.0f, after = 0.0f;
    AudioUnitGetParameter(effect->GetAudioUnit(), id, kAudioUnitScope_Global, 0, &before);
    const auto parameters = nativeEffectParameters(*effect, settings);
    AudioUnitGetParameter(effect->GetAudioUnit(), id, kAudioUnitScope_Global, 0, &after);
    ASSERT_FALSE(parameters.empty());
    EXPECT_EQ(parameters.front().id, original.front().id);
    EXPECT_NEAR(parameters.front().value, 0.37, 1e-6);
    EXPECT_FLOAT_EQ(before, after);
}
#endif

#ifdef AU_NATIVE_TEST_INSTRUMENT_PATH
namespace {
std::shared_ptr<InstrumentProcessor> vstInstrument(const std::string& state = {})
{
    return createVstInstrument(std::string(AU_NATIVE_TEST_INSTRUMENT_PATH) + ";F5A81297A6724B218E434FA1A4050C01", state);
}

std::shared_ptr<InstrumentProcessor> restartVstInstrument(const std::string& state = {})
{
    return createVstInstrument(std::string(AU_NATIVE_TEST_INSTRUMENT_PATH) + ";F5A81297A6724B218E434FA1A4050C04", state);
}
}

TEST(NativeVstInstrumentTests, LiveMidiUpdatesTheNativeControllerAndSavesItsVisibleKnobs)
{
    QLibrary fixture(QString::fromUtf8(AU_NATIVE_TEST_INSTRUMENT_BINARY));
    ASSERT_TRUE(fixture.load());
    const auto updates = reinterpret_cast<uint64_t (*)()>(fixture.resolve("nativeFixtureMidiControllerUpdates"));
    const auto offMain = reinterpret_cast<uint64_t (*)()>(fixture.resolve("nativeFixtureOffMainMidiControllerUpdates"));
    ASSERT_TRUE(updates && offMain);
    auto instrument = vstInstrument();
    instrument->prepare(48000, 64);
    instrument->takeChanges();
    const auto before = updates(), beforeOffMain = offMain();
    std::array<float, 64> samples {};
    float* output[] { samples.data() };
    const std::array events { MidiEvent { 0, 0x90, 60, 127 }, MidiEvent { 16, 0xb0, 41, 25 },
                              MidiEvent { 32, 0xb0, 41, 102 } };
    std::thread audio([&] { instrument->process(output, 1, 64, events, {}); });
    audio.join();
    EXPECT_EQ(updates(), before); // No native controller calls on the audio thread.
    EXPECT_NEAR(samples[20], 25.0 / 127, 1e-6);
    EXPECT_NEAR(samples[40], 102.0 / 127, 1e-6);
    const std::array input {
        audio::InstrumentMidiMessage { 0xb0, 41, 25, 0, audio::InstrumentMidiMessage::Source::MidiDevice },
        audio::InstrumentMidiMessage { 0xb0, 41, 102, 0, audio::InstrumentMidiMessage::Source::MidiDevice }
    };
    instrument->syncLiveMidiControllers(input);
    EXPECT_EQ(updates() - before, input.size());
    EXPECT_EQ(offMain(), beforeOffMain);
    EXPECT_TRUE(instrument->takeChanges());
    EXPECT_NEAR(instrument->parameters()[0].value, 102.0 / 127, 1e-6);
    auto restored = vstInstrument(instrument->state());
    EXPECT_NEAR(restored->parameters()[0].value, 102.0 / 127, 1e-6);
    instrument->syncLiveMidiControllers({});
    EXPECT_FALSE(instrument->takeChanges());
}

TEST(NativeVstInstrumentTests, InstalledInstrumentLiveControllerBridge)
{
    // Optional integration test for an installed instrument. Supply JSON with
    // path, parameter, and cc; the normal suite uses only the owned fixture.
    const auto settings = QJsonDocument::fromJson(qgetenv("AU_NATIVE_TEST_MIDI_CONTROLLER_PLUGIN")).object();
    if (settings.isEmpty()) {
        GTEST_SKIP() << "No installed controller instrument requested";
    }
    const auto path = settings["path"].toString().toStdString();
    auto instrument = createVstInstrument(path, {});
    const auto value = [&](const auto& processor, const std::string& id) {
        for (const auto& parameter : processor->parameters()) {
            if (parameter.id == id) {
                return parameter.value;
            }
        }
        return -1.0;
    };
    const auto controls = settings.contains("controls") ? settings["controls"].toArray() : QJsonArray { settings };
    for (const auto& control : controls) {
        const auto id = control.toObject()["parameter"].toString().toStdString();
        SCOPED_TRACE(id);
        ASSERT_GE(value(instrument, id), 0.0);
        const std::array input { audio::InstrumentMidiMessage { 0xb0, control.toObject()["cc"].toInt(), 25, 0,
                                                                audio::InstrumentMidiMessage::Source::MidiDevice } };
        instrument->syncLiveMidiControllers(input);
        EXPECT_NEAR(value(instrument, id), 25.0 / 127, 1e-5);
    }
    EXPECT_TRUE(instrument->takeChanges());
    auto restored = createVstInstrument(path, instrument->state());
    for (const auto& control : controls) {
        const auto id = control.toObject()["parameter"].toString().toStdString();
        SCOPED_TRACE(id);
        EXPECT_NEAR(value(restored, id), 25.0 / 127, 1e-5);
    }
}

TEST(NativeVstInstrumentTests, ProgramChangesUseChannelUnitsAndSampleOffsetsWithoutReplacingTheManualPreset)
{
    QLibrary fixture(QString::fromUtf8(AU_NATIVE_TEST_INSTRUMENT_BINARY));
    ASSERT_TRUE(fixture.load());
    const auto offMain = reinterpret_cast<uint64_t (*)()>(fixture.resolve("nativeFixtureOffMainProgramQueries"));
    ASSERT_NE(offMain, nullptr);
    const auto before = offMain();
    const auto path = std::string(AU_NATIVE_TEST_INSTRUMENT_PATH) + ";F5A81297A6724B218E434FA1A4050C05";
    auto instrument = createVstInstrument(path, R"({"parameters":{"10":0.5,"11":0,"12":0.3333333333333333}})");
    const auto parameters = instrument->parameters();
    EXPECT_EQ(std::count_if(parameters.begin(), parameters.end(), [](const auto& p) { return p.id == "10"; }), 0);
    instrument->prepare(48000, 64);
    const std::array events { MidiEvent { 0, 0x90, 60, 127 }, MidiEvent { 8, 0xc0, 2, 0 },
                              MidiEvent { 16, 0xc1, 0, 0 }, MidiEvent { 24, 0xc2, 3, 0 }, MidiEvent { 32, 0xc3, 0, 0 },
                              MidiEvent { 40, 0xc4, 0, 0 }, MidiEvent { 48, 0xc0, 127, 0 }, MidiEvent { 56, 0xc1, 1, 0 } };
    std::array<std::array<float, 64>, 2> samples {};
    float* output[] { samples[0].data(), samples[1].data() };
    auto task = std::async(std::launch::async, [&] { instrument->process(output, 2, 64, events, {}); });
    task.get();
    EXPECT_EQ(offMain(), before);
    for (size_t frame = 0; frame < 64; ++frame) {
        const auto root = frame < 24 ? 10.0 / 3 : 10.0;
        EXPECT_NEAR(samples[0][frame], 1.0 + (frame < 8 ? 0.5 : 1.0) + root, 1e-6) << frame;
        EXPECT_NEAR(samples[1][frame], 1.0 + root, 1e-6) << frame;
    }
    EXPECT_FALSE(instrument->takeChanges());
    const auto state = instrument->state();
    const auto saved = QJsonDocument::fromJson(QByteArray::fromStdString(state)).object()["parameters"].toObject();
    ASSERT_TRUE(saved.contains("10"));
    EXPECT_DOUBLE_EQ(saved["10"].toDouble(), 0.5);
    EXPECT_DOUBLE_EQ(saved["12"].toDouble(), 1.0 / 3);
    ASSERT_TRUE(instrument->restoreMidiProgram(0));
    ASSERT_TRUE(instrument->restoreMidiProgram(2));
    EXPECT_FALSE(instrument->restoreMidiProgram(3));
    instrument->process(output, 2, 64, {}, {});
    EXPECT_NEAR(samples[0].back(), 1.5 + 10.0 / 3, 1e-6);
    EXPECT_NEAR(samples[1].back(), 1.0 + 10.0 / 3, 1e-6);
    auto reopened = createVstInstrument(path, state);
    reopened->prepare(48000, 64);
    const std::array note { MidiEvent { 0, 0x90, 60, 127 } };
    reopened->process(output, 2, 64, note, {});
    EXPECT_NEAR(samples[0].back(), 1.5 + 10.0 / 3, 1e-6);
    EXPECT_NEAR(samples[1].back(), 1.0 + 10.0 / 3, 1e-6);
}

TEST(NativeVstInstrumentTests, RootProgramParameterWorksWithoutUnitInfoAndIgnoresProgramsOutsideItsList)
{
    auto instrument = createVstInstrument(std::string(AU_NATIVE_TEST_INSTRUMENT_PATH) + ";F5A81297A6724B218E434FA1A4050C07", {});
    instrument->prepare(48000, 64);
    std::array<float, 64> samples {};
    float* output[] { samples.data() };
    const std::array events { MidiEvent { 0, 0x90, 60, 127 }, MidiEvent { 8, 0xcf, 2, 0 },
                              MidiEvent { 20, 0xcf, 127, 0 }, MidiEvent { 32, 0xcf, 0, 0 } };
    instrument->process(output, 1, 64, events, {});
    for (size_t frame = 0; frame < samples.size(); ++frame) {
        EXPECT_NEAR(samples[frame], 1.0 + (frame < 8 ? 10.0 / 3 : frame < 32 ? 20.0 / 3 : 0), 1e-6) << frame;
    }
    EXPECT_FALSE(instrument->takeChanges());
    ASSERT_TRUE(instrument->restoreMidiProgram(15));
    instrument->process(output, 1, 64, {}, {});
    EXPECT_NEAR(samples.back(), 1.0 + 10.0 / 3, 1e-6);
}

TEST(NativeVstInstrumentTests, SeekingRestoresManualOrLiveProgramsWithoutRetriggeringHeldKeyboardNotes)
{
    for (const int live : { -1, 0 }) {
        SCOPED_TRACE(live);
        auto track = MidiTrack::Create();
        auto processor = createVstInstrument(std::string(AU_NATIVE_TEST_INSTRUMENT_PATH) + ";F5A81297A6724B218E434FA1A4050C05", {});
        auto session = std::make_shared<LiveInstrumentSession>(track, processor);
        session->prepare(48000, 64);
        session->beginTimeline();
        if (live >= 0) {
            session->sendMidi(0xc0, live, 0);
        }
        session->sendMidi(0x90, 60, 127);
        std::array<float, 64> samples {};
        float* output[] { samples.data() };
        session->process(output, 1, 64, {}, {});
        EXPECT_NEAR(samples.back(), 1.0 + (live < 0 ? 0.5 : 0) + 10.0 / 3, 1e-6);
        const std::array timeline { MidiEvent { 0, 0xc0, 2, 0 } };
        session->process(output, 1, 64, timeline, {});
        EXPECT_NEAR(samples.back(), 2.0 + 10.0 / 3, 1e-6);
        session->resetTimeline();
        session->process(output, 1, 64, {}, {});
        for (const auto sample : samples) {
            EXPECT_NEAR(sample, 1.0 + (live < 0 ? 0.5 : 0) + 10.0 / 3, 1e-6);
        }
        EXPECT_FALSE(session->takeChanges());
        session->endTimeline();
        session->close();
    }
}

TEST(NativeVstInstrumentTests, TransportHandoffKeepsALiveProgramWhenTheTimelineDidNotOverrideIt)
{
    auto processor = createVstInstrument(std::string(AU_NATIVE_TEST_INSTRUMENT_PATH) + ";F5A81297A6724B218E434FA1A4050C05", {});
    LiveInstrumentSession session(MidiTrack::Create(), processor);
    session.prepare(48000, 64);
    session.sendMidi(0xc0, 0, 0);
    session.sendMidi(0x90, 60, 127);
    std::array<float, 64> samples {};
    float* output[] { samples.data() };
    session.process(output, 1, 64, {}, {});
    EXPECT_NEAR(samples.back(), 1.0 + 10.0 / 3, 1e-6);
    session.beginTimeline();
    session.prepare(48000, 64);
    const std::array modulation { MidiEvent { 0, 0xb0, 1, 63 } };
    session.process(output, 1, 64, modulation, {});
    session.endTimeline();
    session.prepare(48000, 64);
    session.process(output, 1, 64, {}, {});
    for (const auto sample : samples) {
        EXPECT_NEAR(sample, 1.0 + 10.0 / 3, 1e-6);
    }
    EXPECT_FALSE(session.takeChanges());
    session.close();
}

TEST(NativeVstInstrumentTests, PrivateTimelineResetRestoresProgramBaselinesBeforeASelectionWithoutProgramEvents)
{
    auto instrument = createVstInstrument(std::string(AU_NATIVE_TEST_INSTRUMENT_PATH) + ";F5A81297A6724B218E434FA1A4050C05", {});
    instrument->prepare(48000, 64);
    std::array<float, 64> samples {};
    float* output[] { samples.data() };
    const std::array initial { MidiEvent { 0, 0x90, 60, 127 }, MidiEvent { 0, 0xc0, 2, 0 } };
    instrument->process(output, 1, 64, initial, {});
    EXPECT_NEAR(samples.back(), 2.0 + 10.0 / 3, 1e-6);
    instrument->resetTimeline();
    const std::array note { MidiEvent { 0, 0x90, 60, 127 } };
    instrument->process(output, 1, 64, note, {});
    for (const auto sample : samples) {
        EXPECT_NEAR(sample, 1.5 + 10.0 / 3, 1e-6);
    }
    EXPECT_FALSE(instrument->takeChanges());
}

TEST(NativeVstInstrumentTests, NativeDelayedShortLoopsKeepExactAudioWithoutReplayingOutOfLoopMidiOrPreroll)
{
    QLibrary fixture(QString::fromUtf8(AU_NATIVE_TEST_INSTRUMENT_BINARY));
    ASSERT_TRUE(fixture.load());
    const auto rendered = reinterpret_cast<uint64_t (*)()>(fixture.resolve("nativeFixtureProcessedFrames"));
    const auto ons = reinterpret_cast<uint64_t (*)()>(fixture.resolve("nativeFixtureNoteOns"));
    ASSERT_NE(rendered, nullptr);
    ASSERT_NE(ons, nullptr);
    auto track = MidiTrack::Create();
    ASSERT_TRUE(track->SetClips({ { 1, "SDK loop", 0, 1, 1,
                                    { { 1, 16.0 / 24000, 4.0 / 24000, 60, 64 }, { 2, 20.0 / 24000, 4.0 / 24000, 64, 96 },
                                        { 3, 24.0 / 24000, 64.0 / 24000, 67, 127 } }, {} } }));
    auto processor = vstInstrument();
    ASSERT_TRUE(processor->setParameter("2", 23.0 / 31));
    processor->takeChanges();
    auto sequence = std::make_shared<MidiPlayableSequence>(track, processor, 48000);
    sequence->SetPlaybackLoop(true, 16.0 / 48000, 24.0 / 48000);
    Mixer mixer({ Mixer::Input { sequence } }, std::nullopt, true,
                Mixer::WarpOptions { static_cast<const AudacityProject*>(nullptr) }, 16.0 / 48000, 24.0 / 48000,
                2, 8, false, 48000, floatSample);
    const auto beforeFrames = rendered(), beforeOns = ons();
    for (int loop = 0; loop < 32; ++loop) {
        ASSERT_EQ(mixer.Process(8), 8u);
        const auto* samples = reinterpret_cast<const float*>(mixer.GetBuffer(0));
        for (int frame = 0; frame < 8; ++frame) {
            EXPECT_NEAR(samples[frame], 0.5f * (frame < 4 ? 64 : 96) / 127.0f, 1e-6) << loop << ":" << frame;
        }
        mixer.SetTimesAndSpeed(16.0 / 48000, 24.0 / 48000, 1, true);
    }
    EXPECT_EQ(rendered() - beforeFrames, 512u + 256u + 23u);
    EXPECT_GE(ons() - beforeOns, 64u);
    EXPECT_LE(ons() - beforeOns, 72u);
    EXPECT_FALSE(processor->takeChanges());
}

TEST(NativeVstInstrumentTests, ReactivationResumesHeldAndPedalReleasedVoicesAndKeepsTheManualPreset)
{
    auto instrument = restartVstInstrument(R"({"parameters":{"0":0.6}})");
    instrument->prepare(48000, 64);
    std::array<float, 64> samples {};
    float* output[] { samples.data() };
    const std::array held { MidiEvent { 0, 0xb0, 64, 127 }, MidiEvent { 0, 0x90, 60, 127 },
                            MidiEvent { 0, 0x90, 64, 64 }, MidiEvent { 20, 0x80, 60, 0 } };
    instrument->process(output, 1, 64, held, {});
    ASSERT_NEAR(samples.back(), 0.6 * (1 + 64.0 / 127), 1e-6);
    const std::array overrideGain { MidiEvent { 0, 0xb0, 41, 95 } };
    instrument->process(output, 1, 64, overrideGain, {});
    ASSERT_NEAR(samples.back(), 95.0 / 127 * (1 + 64.0 / 127), 1e-6);
    ASSERT_TRUE(instrument->setParameter("2", 7.0 / 31));
    instrument->takeChanges();
    ASSERT_TRUE(instrument->latencyUpdatePending());
    ASSERT_TRUE(instrument->refreshLatency());
    const std::array next { MidiEvent { 24, 0xb0, 11, 32 }, MidiEvent { 31, 0x90, 67, 127 } };
    instrument->process(output, 1, 64, next, {});
    for (size_t frame = 0; frame < samples.size(); ++frame) {
        const auto voices = frame < 38 ? 1 + 64.0 / 127 : 2 + 64.0 / 127;
        const auto gain = frame < 31 ? 95.0 / 127 : 32.0 / 127;
        EXPECT_NEAR(samples[frame], frame < 7 ? 0.0 : voices* gain, 1e-6) << frame;
    }
    const std::array releasePedal { MidiEvent { 12, 0xb0, 64, 0 } };
    instrument->process(output, 1, 64, releasePedal, {});
    for (size_t frame = 0; frame < samples.size(); ++frame) {
        EXPECT_NEAR(samples[frame], (frame < 19 ? 2 + 64.0 / 127 : 1 + 64.0 / 127) * 32.0 / 127, 1e-6) << frame;
    }
    const std::array releaseKeys { MidiEvent { 10, 0x80, 64, 0 }, MidiEvent { 20, 0x80, 67, 0 } };
    instrument->process(output, 1, 64, releaseKeys, {});
    ASSERT_NEAR(samples.back(), 0.0, 1e-6);
    ASSERT_TRUE(instrument->setParameter("2", 15.0 / 31));
    instrument->takeChanges();
    ASSERT_TRUE(instrument->refreshLatency());
    instrument->process(output, 1, 64, {}, {});
    for (const auto sample : samples) {
        EXPECT_NEAR(sample, 0.0, 1e-6);
    }
    const auto saved = QJsonDocument::fromJson(QByteArray::fromStdString(instrument->state())).object()["parameters"].toObject();
    EXPECT_DOUBLE_EQ(saved["0"].toDouble(), 0.6);
    EXPECT_DOUBLE_EQ(saved["1"].toDouble(), 0.0);
    EXPECT_FALSE(instrument->takeChanges());
}

TEST(NativeVstInstrumentTests, RestartAppliesNewControlEditsBeforeFreshMidiAndAlsoRecoversAnUnchangedDelay)
{
    QLibrary fixture(QString::fromUtf8(AU_NATIVE_TEST_INSTRUMENT_BINARY));
    ASSERT_TRUE(fixture.load());
    const auto frames = reinterpret_cast<uint64_t (*)()>(fixture.resolve("nativeFixtureProcessedFrames"));
    const auto ons = reinterpret_cast<uint64_t (*)()>(fixture.resolve("nativeFixtureNoteOns"));
    ASSERT_TRUE(frames && ons);
    auto instrument = restartVstInstrument(R"({"parameters":{"0":0.6}})");
    instrument->prepare(48000, 64);
    std::array<float, 64> samples {};
    float* output[] { samples.data() };
    const std::array input { MidiEvent { 0, 0x90, 60, 127 }, MidiEvent { 10, 0xb0, 41, 95 } };
    instrument->process(output, 1, 64, input, {});
    ASSERT_TRUE(instrument->setParameter("2", 7.0 / 31));
    instrument->takeChanges();
    ASSERT_TRUE(instrument->refreshLatency());
    ASSERT_TRUE(instrument->setAutomatedParameter("0", 0.25));
    const auto beforeFrames = frames(), beforeOns = ons();
    const std::array fresh { MidiEvent { 17, 0xb0, 41, 64 } };
    instrument->process(output, 1, 64, fresh, {});
    EXPECT_EQ(frames() - beforeFrames, 64u); // No discarded warm-up audio.
    EXPECT_EQ(ons() - beforeOns, 1u);
    for (size_t frame = 0; frame < 64; ++frame) {
        EXPECT_NEAR(samples[frame], frame < 7 ? 0.0 : frame < 24 ? 0.25 : 64.0 / 127, 1e-6) << frame;
    }
    EXPECT_FALSE(instrument->takeChanges());
    auto saved = QJsonDocument::fromJson(QByteArray::fromStdString(instrument->state())).object()["parameters"].toObject();
    EXPECT_DOUBLE_EQ(saved["0"].toDouble(), 0.6);
    // Return to the cached delay before servicing the request. The native
    // restart still clears voices, even though RefreshLatency returns false.
    ASSERT_TRUE(instrument->setParameter("0", 0.33));
    ASSERT_TRUE(instrument->setParameter("2", 15.0 / 31));
    ASSERT_TRUE(instrument->setParameter("2", 7.0 / 31));
    instrument->takeChanges();
    ASSERT_TRUE(instrument->latencyUpdatePending());
    EXPECT_FALSE(instrument->refreshLatency());
    instrument->process(output, 1, 64, {}, {});
    for (size_t frame = 0; frame < 64; ++frame) {
        EXPECT_NEAR(samples[frame], frame < 7 ? 0.0 : 0.33, 1e-6) << frame;
    }
    const auto resumedOns = ons();
    EXPECT_FALSE(instrument->refreshLatency());
    instrument->process(output, 1, 64, {}, {});
    EXPECT_EQ(ons(), resumedOns);
    for (const auto sample : samples) {
        EXPECT_NEAR(sample, 0.33, 1e-6);
    }
}

TEST(NativeVstInstrumentTests, ResetDuringReactivationCancelsOldNotesAndStillRestoresTheRebuiltControls)
{
    auto instrument = restartVstInstrument(R"({"parameters":{"0":0.6}})");
    instrument->prepare(48000, 64);
    std::array<float, 64> samples {};
    float* output[] { samples.data() };
    const std::array note { MidiEvent { 0, 0x90, 60, 127 } };
    instrument->process(output, 1, 64, note, {});
    ASSERT_TRUE(instrument->setParameter("2", 7.0 / 31));
    ASSERT_TRUE(instrument->refreshLatency());
    instrument->reset();
    instrument->process(output, 1, 64, {}, {});
    for (const auto sample : samples) {
        EXPECT_FLOAT_EQ(sample, 0.0f);
    }
    const std::array next { MidiEvent { 0, 0x90, 64, 127 } };
    instrument->process(output, 1, 64, next, {});
    for (size_t frame = 0; frame < 64; ++frame) {
        EXPECT_NEAR(samples[frame], frame < 7 ? 0.0 : 0.6, 1e-6) << frame;
    }
}

TEST(NativeVstInstrumentTests, OfflineProbeRestoresThePresetAfterTheNativeRestartRebuildsItsDsp)
{
    auto track = MidiTrack::Create();
    ASSERT_TRUE(track->SetClips({ { 1, "Phrase", 0, 1, 1,
                                    { { 1, 100.0 / 24000, 200.0 / 24000, 60, 127 } }, {} } }));
    auto instrument = restartVstInstrument(R"({"parameters":{"0":0.6,"2":0.22580645161290322}})");
    auto sequence = std::make_shared<MidiPlayableSequence>(track, instrument, 48000);
    Mixer mixer({ Mixer::Input { sequence } }, std::nullopt, true,
                Mixer::WarpOptions { static_cast<const AudacityProject*>(nullptr) }, 0, 1, 2, 512, false, 48000, floatSample);
    ASSERT_EQ(mixer.Process(512), 512u);
    const auto* samples = reinterpret_cast<const float*>(mixer.GetBuffer(0));
    for (size_t frame = 0; frame < 512; ++frame) {
        EXPECT_NEAR(samples[frame], frame >= 100 && frame < 300 ? 0.6 : 0.0, 1e-6) << frame;
    }
}

TEST(NativeVstInstrumentTests, RecoveryAndFreshPressureEventsFitWithoutDroppingTheNativeChase)
{
    QLibrary fixture(QString::fromUtf8(AU_NATIVE_TEST_INSTRUMENT_BINARY));
    ASSERT_TRUE(fixture.load());
    const auto pressures = reinterpret_cast<uint64_t (*)()>(fixture.resolve("nativeFixturePressureEvents"));
    const auto pressureValues = reinterpret_cast<uint64_t (*)()>(fixture.resolve("nativeFixturePressureValues"));
    ASSERT_TRUE(pressures && pressureValues);
    auto instrument = restartVstInstrument();
    instrument->prepare(48000, 64);
    std::array<float, 64> samples {};
    float* output[] { samples.data() };
    std::vector<MidiEvent> held { { 0, 0xb0, 64, 127 } };
    for (uint8_t channel = 0; channel < 16; ++channel) {
        for (uint8_t note = 0; note < 126; ++note) {
            held.push_back({ 0, static_cast<uint8_t>(0x90 | channel), note, 127 });
            held.push_back({ 1, static_cast<uint8_t>(0xa0 | channel), note, 80 });
            if (note % 2 == 0) {
                held.push_back({ 2, static_cast<uint8_t>(0x80 | channel), note, 0 });
            }
        }
    }
    instrument->process(output, 1, 64, held, {});
    ASSERT_FLOAT_EQ(samples.back(), 1008.0f);
    ASSERT_TRUE(instrument->setParameter("2", 7.0 / 31));
    ASSERT_TRUE(instrument->refreshLatency());
    std::vector<MidiEvent> fresh;
    for (uint8_t value = 90; value < 94; ++value) {
        for (uint8_t channel = 0; channel < 16; ++channel) {
            for (uint8_t note = 0; note < 126; ++note) {
                fresh.push_back({ value - 73, static_cast<uint8_t>(0xa0 | channel), note, value });
            }
        }
    }
    const auto before = pressures(), valuesBefore = pressureValues();
    instrument->process(output, 1, 64, fresh, {});
    EXPECT_EQ(pressures() - before, 2016u + fresh.size());
    EXPECT_EQ(pressureValues() - valuesBefore, 2016u * (80u + 90u + 91u + 92u + 93u));
    for (size_t frame = 0; frame < 64; ++frame) {
        EXPECT_FLOAT_EQ(samples[frame], frame < 7 ? 0.0f : 1008.0f) << frame;
    }
}

TEST(NativeVstInstrumentTests, NativePluginReceivesSampleOffsetsAndVelocity)
{
    auto instrument = vstInstrument();
    instrument->prepare(48000.0, 512);
    std::vector<float> left(512), right(512);
    float* output[] { left.data(), right.data() };
    const std::array events { MidiEvent { 40, 0x92, 60, 64 }, MidiEvent { 100, 0x82, 60, 0 } };
    instrument->process(output, 2, 512, events, {});
    for (size_t index = 0; index < left.size(); ++index) {
        EXPECT_NEAR(left[index], index >= 40 && index < 100 ? 0.5 * 64.0 / 127.0 : 0.0, 1e-6) << index;
    }
    EXPECT_EQ(left, right);
}

TEST(NativeVstInstrumentTests, ContinuousAutomatedDelayKeepsWorkerMixerAudioAlignedAndNativeControlOnMain)
{
    QLibrary fixture(QString::fromUtf8(AU_NATIVE_TEST_INSTRUMENT_BINARY));
    ASSERT_TRUE(fixture.load());
    const auto offMain = reinterpret_cast<uint64_t (*)()>(fixture.resolve("nativeFixtureOffMainLatencyQueries"));
    ASSERT_NE(offMain, nullptr);
    const auto before = offMain();
    for (const auto& [rate, block] : { std::pair { 48000.0, size_t { 4096 } }, { 44100.0, size_t { 73 } } }) {
        const auto makeMixer = [&](bool changing) {
            auto track = MidiTrack::Create();
            std::vector<MidiNoteData> notes;
            for (int sample = 0; sample < 5120; sample += 64) {
                notes.push_back({ sample / 64 + 1, sample / 24000.0, 64.0 / 24000, 60, 32 + (sample / 64 % 8) * 8 });
            }
            EXPECT_TRUE(track->SetClips({ { 1, "Native delay transitions", 0.0, 5120.0 / 24000, 1, notes, {} } }));
            if (changing) {
                EXPECT_TRUE(TrackAutomation::Get(*track).SetLane({ "instrument/2", true,
                                                                   { { 0, 7.0 / 31 }, { 1023.0 / 48000, 7.0 / 31 },
                                                                       { 1024.0 / 48000, 23.0 / 31 },
                                                                       { 2047.0 / 48000, 23.0 / 31 }, { 2048.0 / 48000, 3.0 / 31 },
                                                                       { 3071.0 / 48000, 3.0 / 31 }, { 3072.0 / 48000, 15.0 / 31 },
                                                                       { 4095.0 / 48000, 15.0 / 31 }, { 4096.0 / 48000, 0 } } }));
            }
            auto sequence = std::make_shared<MidiPlayableSequence>(track, restartVstInstrument(), 48000);
            return std::make_unique<Mixer>(std::vector<Mixer::Input> { { sequence } }, std::nullopt, true,
                                           Mixer::WarpOptions { static_cast<const AudacityProject*>(nullptr) }, 0.0, 5120.0 / 48000,
                                           2, block, false, rate, floatSample);
        };
        auto changing = makeMixer(true), dry = makeMixer(false);
        const auto collect = [&](Mixer& mixer) {
            std::vector<float> result;
            while (const auto count = mixer.Process(block)) {
                const auto* samples = reinterpret_cast<const float*>(mixer.GetBuffer(0));
                result.insert(result.end(), samples, samples + count);
            }
            return result;
        };
        auto worker = std::async(std::launch::async, [&] { return std::pair { collect(*changing), collect(*dry) }; });
        while (worker.wait_for(std::chrono::milliseconds(1)) != std::future_status::ready) {
            QCoreApplication::processEvents();
        }
        const auto [actual, reference] = worker.get();
        ASSERT_EQ(actual.size(), reference.size());
        size_t compared = 0, mismatches = 0;
        std::array<size_t, 5> mismatchRegions {};
        double maximum = 0;
        for (size_t frame = 0; frame < actual.size(); ++frame) {
            const auto input = frame * 48000.0 / rate;
            bool transition = false;
            for (const auto change : { 1024, 2048, 3072, 4096 }) {
                transition |= std::abs(input - change) <= 256;
            }
            if (transition) {
                continue;
            }
            ++compared;
            const auto error = std::abs(actual[frame] - reference[frame]);
            maximum = std::max(maximum, static_cast<double>(error));
            if (error > 1e-6) {
                if (mismatches < 12) {
                    std::cout << "VST delayed frame=" << frame << " input=" << input
                              << " actual=" << actual[frame] << " reference=" << reference[frame] << '\n';
                }
                ++mismatches;
                ++mismatchRegions[std::min<size_t>(4, static_cast<size_t>(input / 1024))];
            }
        }
        if (mismatches) {
            for (size_t i = 0; i < mismatchRegions.size(); ++i) {
                std::cout << "VST delay region " << i << " mismatches=" << mismatchRegions[i] << '\n';
            }
        }
        EXPECT_GT(compared, 2500u);
        EXPECT_EQ(mismatches, 0u) << "maximum stable-region error=" << maximum << ", output rate=" << rate;
    }
    EXPECT_EQ(offMain(), before);
}

TEST(NativeVstInstrumentTests, ReadingInstrumentLatencyNeverCallsTheNativeProcessorFromTheReaderThread)
{
    QLibrary fixture(QString::fromUtf8(AU_NATIVE_TEST_INSTRUMENT_BINARY));
    ASSERT_TRUE(fixture.load()) << fixture.errorString().toStdString();
    const auto queries = reinterpret_cast<uint64_t (*)()>(fixture.resolve("nativeFixtureLatencyQueries"));
    ASSERT_NE(queries, nullptr);
    auto instrument = vstInstrument();
    instrument->prepare(48000.0, 64);
    const auto before = queries();
    std::thread reader([&] {
        for (unsigned i = 0; i < 10000; ++i) {
            EXPECT_EQ(instrument->latency(), 0u);
        }
    });
    reader.join();
    EXPECT_EQ(queries(), before);
}

TEST(NativeVstInstrumentTests, ExplicitOfflineDelayMeasurementAlsoHandlesAMissingNativeNotification)
{
    VST3EffectsModule provider;
    auto component = provider.LoadPlugin(wxString::FromUTF8(std::string(AU_NATIVE_TEST_INSTRUMENT_PATH)
                                                            + ";F5A81297A6724B218E434FA1A4050C01"));
    auto* effect = dynamic_cast<VST3EffectBase*>(component.get());
    ASSERT_NE(effect, nullptr);
    auto settings = effect->MakeSettings();
    const auto instance = effect->MakeInstance();
    instance->SetBlockSize(64);
    ASSERT_TRUE(instance->RealtimeInitialize(settings, 48000.0, 64));
    const auto finalize = finally([&] { instance->RealtimeFinalize(settings); });
    const auto vst = std::dynamic_pointer_cast<VST3Instance>(instance);
    ASSERT_TRUE(vst);
    Steinberg::FUnknownPtr<Steinberg::Vst::IConnectionPoint> connection(vst->effectComponent().get());
    ASSERT_TRUE(connection);
    auto message = Steinberg::owned(new Steinberg::Vst::HostMessage);
    message->setMessageID("latency");
    ASSERT_EQ(message->getAttributes()->setInt("samples", 17), Steinberg::kResultOk);
    ASSERT_EQ(connection->notify(message), Steinberg::kResultOk);
    auto& wrapper = vst->GetWrapper();
    EXPECT_FALSE(wrapper.LatencyUpdatePending());
    EXPECT_EQ(wrapper.CachedLatencySamples(), 0);
    EXPECT_FALSE(wrapper.RefreshLatency()); // Live effect path waits for an explicit request.
    EXPECT_EQ(wrapper.CachedLatencySamples(), 0);
    EXPECT_TRUE(wrapper.RefreshLatency(true)); // Private offline measurement.
    EXPECT_EQ(wrapper.CachedLatencySamples(), 17);
    EXPECT_FALSE(wrapper.RefreshLatency(true));
}

TEST(NativeVstInstrumentTests, InstrumentDelayNotificationStaysPendingUntilTheQuiescentNativeRefresh)
{
    auto instrument = vstInstrument();
    instrument->prepare(48000.0, 64);
    std::array<float, 64> left {}, right {};
    float* output[] { left.data(), right.data() };
    for (const auto delay : { 7u, 31u, 3u, 0u }) {
        SCOPED_TRACE(delay);
        const auto previous = instrument->latency();
        ASSERT_TRUE(instrument->setAutomatedParameter("2", delay / 31.0));
        instrument->process(output, 2, 64, {}, {});
        EXPECT_TRUE(instrument->latencyUpdatePending());
        EXPECT_EQ(instrument->latency(), previous);
        EXPECT_TRUE(instrument->refreshLatency());
        EXPECT_EQ(instrument->latency(), delay);
        EXPECT_FALSE(instrument->latencyUpdatePending());
        EXPECT_FALSE(instrument->refreshLatency());
        EXPECT_FALSE(instrument->takeChanges());
    }
}

TEST(NativeVstInstrumentTests, OfflineMixerSettlesNativeInstrumentDelayBeforeItsFirstNoteAndAfterReseeking)
{
    auto track = MidiTrack::Create();
    ASSERT_TRUE(track->SetClips({ { 1, "Phrase", 0.0, 1.0, 1,
                                    { { 1, 100.0 / 24000, 200.0 / 24000, 60, 127 } }, {} } }));
    auto instrument = vstInstrument();
    auto sequence = std::make_shared<MidiPlayableSequence>(track, instrument, 48000.0);
    Mixer mixer({ Mixer::Input { sequence } }, std::nullopt, true,
                Mixer::WarpOptions { static_cast<const AudacityProject*>(nullptr) }, 0.0, 1.0, 2, 512, false, 48000.0, floatSample);
    for (const auto delay : { 7u, 31u, 0u, 15u }) {
        SCOPED_TRACE(delay);
        ASSERT_TRUE(instrument->setParameter("2", delay / 31.0));
        mixer.Reposition(0.0, true);
        ASSERT_EQ(mixer.Process(512), 512u);
        const auto* actual = reinterpret_cast<const float*>(mixer.GetBuffer(0));
        for (size_t frame = 0; frame < 512; ++frame) {
            EXPECT_FLOAT_EQ(actual[frame], frame >= 100 && frame < 300 ? 0.5f : 0.0f) << frame;
        }
        EXPECT_EQ(instrument->latency(), delay);
        EXPECT_FALSE(instrument->latencyUpdatePending());
    }
}

TEST(NativeVstInstrumentTests, MidiEventBridgeDefaultsNeverOverwriteManualPresetValuesDuringPreparationOrRestore)
{
    // Older projects also saved virtual MIDI-event parameters in this map.
    // They must not become unsolicited controller messages on every start.
    auto instrument = vstInstrument(R"({"parameters":{"0":0.6,"3":0.0,"4":1.0}})");
    const auto parameters = instrument->parameters();
    EXPECT_EQ(parameters.size(), 3u);
    EXPECT_TRUE(std::none_of(parameters.begin(), parameters.end(), [](const auto& parameter) {
        return parameter.id == "3" || parameter.id == "4";
    }));
    std::array<float, 64> left {}, right {};
    float* output[] { left.data(), right.data() };
    const std::array note { MidiEvent { 0, 0x90, 60, 127 } };
    instrument->prepare(48000.0, 64);
    instrument->process(output, 2, 64, note, {});
    EXPECT_NEAR(left[32], 0.6f, 1e-6);
    instrument->state();
    instrument->prepare(48000.0, 128);
    instrument->process(output, 2, 64, note, {});
    EXPECT_NEAR(left[32], 0.6f, 1e-6);
    const std::array controller { MidiEvent { 17, 0xb0, 41, 95 } };
    instrument->process(output, 2, 64, controller, {});
    EXPECT_NEAR(left[16], 0.6f, 1e-6);
    EXPECT_NEAR(left[17], 95.0f / 127.0f, 1e-6);
    const auto saved = instrument->state();
    const auto values = QJsonDocument::fromJson(QByteArray::fromStdString(saved)).object()["parameters"].toObject();
    EXPECT_FALSE(values.contains("3"));
    EXPECT_FALSE(values.contains("4"));
    EXPECT_DOUBLE_EQ(values["0"].toDouble(), 0.6);
    auto restored = vstInstrument(saved);
    restored->prepare(48000.0, 64);
    restored->process(output, 2, 64, note, {});
    EXPECT_NEAR(left[32], 0.6f, 1e-6);
}

TEST(NativeVstInstrumentTests, ReconfiguringAnInstrumentDoesNotReloadItsLastSavedNativePreset)
{
    QLibrary fixture(QString::fromUtf8(AU_NATIVE_TEST_INSTRUMENT_BINARY));
    ASSERT_TRUE(fixture.load());
    const auto loads = reinterpret_cast<uint64_t (*)()>(fixture.resolve("nativeFixtureProcessorStateLoads"));
    ASSERT_NE(loads, nullptr);
    auto instrument = vstInstrument();
    instrument->prepare(48000, 64);
    ASSERT_TRUE(instrument->setParameter("0", 0.37));
    std::array<float, 64> left {}, right {};
    float* output[] { left.data(), right.data() };
    const std::array note { MidiEvent { 0, 0x90, 60, 127 } };
    instrument->process(output, 2, 64, note, {});
    const auto saved = instrument->state();
    const auto beforeReconfigure = loads();
    // A device rate/block-size change reconfigures this same processor. A
    // previous save is not a request to restore a whole preset on that object.
    instrument->prepare(96000, 128);
    EXPECT_EQ(loads(), beforeReconfigure);
    instrument->process(output, 2, 64, note, {});
    EXPECT_NEAR(left[32], 0.37f, 1e-6);
    auto restored = vstInstrument(saved);
    EXPECT_EQ(loads(), beforeReconfigure + 1);
    const auto beforePrepare = loads();
    restored->prepare(48000, 64);
    EXPECT_EQ(loads(), beforePrepare);
    restored->process(output, 2, 64, note, {});
    EXPECT_NEAR(left[32], 0.37f, 1e-6);
}

TEST(NativeVstInstrumentTests, OriginalEffectProcessorsReceiveIndependentTempoMeterPositionAndTransportState)
{
    VST3EffectsModule provider;
    auto component = provider.LoadPlugin(wxString::FromUTF8(std::string(AU_NATIVE_TEST_INSTRUMENT_PATH)
                                                            + ";F5A81297A6724B218E434FA1A4050C01"));
    auto* effect = dynamic_cast<VST3EffectBase*>(component.get());
    ASSERT_NE(effect, nullptr);
    auto settings = effect->MakeSettings();
    const auto instance = effect->MakeInstance();
    instance->SetBlockSize(64);
    ASSERT_TRUE(instance->RealtimeInitialize(settings, 48000.0, 64));
    ASSERT_TRUE(instance->RealtimeAddProcessor(settings, nullptr, 2, 48000.0));
    ASSERT_TRUE(instance->RealtimeAddProcessor(settings, nullptr, 2, 48000.0));
    ASSERT_TRUE(instance->RealtimeResume());
    std::array<float, 64> left {}, right {};
    float* output[] { left.data(), right.data() };
    EffectInstance::MessagePackage package { settings, nullptr };
    ASSERT_TRUE(instance->RealtimeProcessStart(package));
    for (size_t group = 0; group < 2; ++group) {
        instance->SetRealtimeProcessingContext(group, { group == 0 ? -0.25 : 2.0, 48000.0, 93.0, 7, 8, 1.0, group != 0,
                                                        group != 0, 0.5, 1.5 });
        ASSERT_EQ(instance->RealtimeProcess(group, settings, nullptr, output, left.size()), left.size());
        const double expected[] { 93.0, 7.0, 8.0, group == 0 ? -12000.0 : 96000.0,
                                  group == 0 ? -0.3875 : 3.1, group == 0 ? -3.5 : 0.0, 48000.0, group == 0 ? 0.0 : 1.0,
                                  group == 0 ? 0.0 : 1.0, 0.775, 2.325, 1.0 };
        for (size_t i = 0; i < left.size(); ++i) {
            EXPECT_NEAR(left[i], expected[i % std::size(expected)], 1e-6) << group << ":" << i;
            EXPECT_EQ(left[i], right[i]);
        }
    }
    ASSERT_TRUE(instance->RealtimeProcessEnd(settings));
    ASSERT_TRUE(instance->RealtimeFinalize(settings));
}

TEST(NativeVstInstrumentTests, InstrumentReceivesNativeCycleBoundariesAndBarPositionInQuarterNotes)
{
    auto instrument = vstInstrument();
    instrument->prepare(48000.0, 64);
    std::array<float, 64> left {}, right {};
    float* output[] { left.data(), right.data() };
    const std::array events { MidiEvent { 0, 0x90, 127, 127 } };
    instrument->process(output, 2, 64, events, { 120000, 93.0, 7, 8, true, true, 0.5, 1.5 });
    const double expected[] { 93.0, 7.0, 8.0, 120000.0, 3.875, 3.5, 48000.0, 1.0, 1.0, 0.775, 2.325, 1.0 };
    for (size_t i = 0; i < left.size(); ++i) {
        EXPECT_NEAR(left[i], expected[i % std::size(expected)], 1e-6) << i;
        EXPECT_EQ(left[i], right[i]);
    }
    instrument->process(output, 2, 64, events, { 0, 93.0, 7, 8, false, false, 0.0, 0.0 });
    EXPECT_FLOAT_EQ(left[7], 0.0f);
    EXPECT_FLOAT_EQ(left[8], 0.0f);
    EXPECT_FLOAT_EQ(left[9], 0.0f);
    EXPECT_FLOAT_EQ(left[10], 0.0f);
}

TEST(NativeVstInstrumentTests, NativeProjectAndUnloopedClockPositionsRemainDistinct)
{
    auto instrument = vstInstrument();
    instrument->prepare(48000.0, 64);
    std::array<float, 64> left {}, right {};
    float* output[] { left.data(), right.data() };
    const std::array events { MidiEvent { 0, 0x90, 126, 127 } };
    InstrumentProcessContext context { 60000, 120.0, 4, 4, false };
    context.continuousSample = 0;
    instrument->process(output, 2, 64, events, context);
    EXPECT_FLOAT_EQ(left[0], 60000.0f);
    EXPECT_FLOAT_EQ(left[1], 60000.0f); // VST's unlooped project clock is stopped too.
    EXPECT_FLOAT_EQ(left[2], 1.0f);
    EXPECT_FLOAT_EQ(left[3], 0.0f);
    context.playing = context.cycling = true;
    context.sample = 24000;
    context.continuousSample = 60064;
    instrument->process(output, 2, 64, events, context);
    EXPECT_FLOAT_EQ(left[0], 24000.0f);
    EXPECT_FLOAT_EQ(left[1], 60064.0f);
    EXPECT_FLOAT_EQ(left[2], 1.0f);
    EXPECT_FLOAT_EQ(left[3], 1.0f);
}

TEST(NativeVstInstrumentTests, AutomationQueriesTheOriginalNativeEffectController)
{
    VST3EffectsModule provider;
    auto component = provider.LoadPlugin(wxString::FromUTF8(std::string(AU_NATIVE_TEST_INSTRUMENT_PATH)
                                                            + ";F5A81297A6724B218E434FA1A4050C01"));
    auto* effect = dynamic_cast<VST3EffectBase*>(component.get());
    ASSERT_NE(effect, nullptr);
    const auto parameters = nativeEffectParameters(*effect, effect->MakeSettings());
    ASSERT_EQ(parameters.size(), 3u);
    EXPECT_EQ(parameters.front().id, "0");
    EXPECT_EQ(parameters.front().name, "Gain");
    EXPECT_DOUBLE_EQ(parameters.front().value, 0.5);
}

TEST(NativeVstInstrumentTests, ControllerLatencyRestartIsDeferredAndPublishedOnceAfterReactivation)
{
    VST3EffectsModule provider;
    auto component = provider.LoadPlugin(wxString::FromUTF8(std::string(AU_NATIVE_TEST_INSTRUMENT_PATH)
                                                            + ";F5A81297A6724B218E434FA1A4050C01"));
    auto* effect = dynamic_cast<VST3EffectBase*>(component.get());
    ASSERT_NE(effect, nullptr);
    auto settings = effect->MakeSettings();
    const auto instance = effect->MakeInstance();
    instance->SetBlockSize(64);
    ASSERT_TRUE(instance->RealtimeInitialize(settings, 48000.0, 64));
    ASSERT_TRUE(instance->RealtimeAddProcessor(settings, nullptr, 2, 48000.0));
    ASSERT_TRUE(instance->RealtimeResume());
    const auto vst = std::dynamic_pointer_cast<VST3Instance>(instance);
    ASSERT_TRUE(vst);
    const auto controller = vst->vstEditController();
    ASSERT_TRUE(controller);
    EXPECT_EQ(instance->GetLatency(settings, 48000.0), 0);
    EXPECT_FALSE(instance->RealtimeLatencyUpdatePending());
    ASSERT_EQ(controller->setParamNormalized(2, 7.0 / 31.0), Steinberg::kResultOk);
    EXPECT_TRUE(instance->RealtimeLatencyUpdatePending());
    EXPECT_EQ(instance->GetLatency(settings, 48000.0), 0);
    EXPECT_EQ(instance->RealtimeLatencyRevision(), 0u);
    ASSERT_TRUE(instance->RefreshRealtimeLatency());
    EXPECT_EQ(instance->GetLatency(settings, 48000.0), 7);
    EXPECT_EQ(instance->RealtimeLatencyRevision(), 1u);
    EXPECT_FALSE(instance->RealtimeLatencyUpdatePending());
    EXPECT_FALSE(instance->RefreshRealtimeLatency());
    EXPECT_EQ(instance->RealtimeLatencyRevision(), 1u);
    ASSERT_EQ(controller->setParamNormalized(2, 11.0 / 31.0), Steinberg::kResultOk);
    ASSERT_TRUE(instance->RefreshRealtimeLatency());
    EXPECT_EQ(instance->GetLatency(settings, 48000.0), 11);
    EXPECT_EQ(instance->RealtimeLatencyRevision(), 2u);
    std::array<float, 64> left {}, right {};
    float* output[] { left.data(), right.data() };
    EffectInstance::MessagePackage package { settings, nullptr };
    ASSERT_TRUE(instance->RealtimeProcessStart(package));
    EXPECT_EQ(instance->RealtimeProcess(0, settings, nullptr, output, 64), 64u);
    ASSERT_TRUE(instance->RealtimeProcessEnd(settings));
    ASSERT_TRUE(instance->RealtimeFinalize(settings));
}

TEST(NativeVstInstrumentTests, LatencyRestartRaisedWhileProcessingResumesRemainsPendingForTheNextRefresh)
{
    VST3EffectsModule provider;
    auto component = provider.LoadPlugin(wxString::FromUTF8(std::string(AU_NATIVE_TEST_INSTRUMENT_PATH)
                                                            + ";F5A81297A6724B218E434FA1A4050C01"));
    auto* effect = dynamic_cast<VST3EffectBase*>(component.get());
    ASSERT_NE(effect, nullptr);
    auto settings = effect->MakeSettings();
    const auto instance = effect->MakeInstance();
    instance->SetBlockSize(64);
    ASSERT_TRUE(instance->RealtimeInitialize(settings, 48000.0, 64));
    ASSERT_TRUE(instance->RealtimeAddProcessor(settings, nullptr, 2, 48000.0));
    ASSERT_TRUE(instance->RealtimeResume());
    const auto vst = std::dynamic_pointer_cast<VST3Instance>(instance);
    ASSERT_TRUE(vst);
    const auto controller = vst->vstEditController();
    ASSERT_TRUE(controller);
    Steinberg::FUnknownPtr<Steinberg::Vst::IConnectionPoint> connection(controller.get());
    ASSERT_TRUE(connection);
    uint64_t revision = 0;
    for (const int next : { 11, 19 }) {
        SCOPED_TRACE(next);
        // Real SDK connection messages arm a single notification inside the
        // fixture's next setProcessing(true), after the host measured delay 7.
        auto message = Steinberg::owned(new Steinberg::Vst::HostMessage);
        message->setMessageID("arm-latency-on-resume");
        ASSERT_EQ(message->getAttributes()->setInt("samples", next), Steinberg::kResultOk);
        ASSERT_EQ(connection->notify(message), Steinberg::kResultOk);
        ASSERT_EQ(controller->setParamNormalized(2, 7.0 / 31), Steinberg::kResultOk);
        ASSERT_TRUE(instance->RefreshRealtimeLatency());
        EXPECT_EQ(instance->GetLatency(settings, 48000), 7);
        EXPECT_EQ(instance->RealtimeLatencyRevision(), ++revision);
        ASSERT_TRUE(instance->RealtimeLatencyUpdatePending());
        ASSERT_TRUE(instance->RefreshRealtimeLatency());
        EXPECT_EQ(instance->GetLatency(settings, 48000), next);
        EXPECT_EQ(instance->RealtimeLatencyRevision(), ++revision);
        EXPECT_FALSE(instance->RealtimeLatencyUpdatePending());
        EXPECT_FALSE(instance->RefreshRealtimeLatency());
    }
    ASSERT_TRUE(instance->RealtimeFinalize(settings));
}

TEST(NativeVstInstrumentTests, ReactivationLatencyNotificationIsMeasuredWithoutLeavingARepeatRestart)
{
    VST3EffectsModule provider;
    auto component = provider.LoadPlugin(wxString::FromUTF8(std::string(AU_NATIVE_TEST_INSTRUMENT_PATH)
                                                            + ";F5A81297A6724B218E434FA1A4050C01"));
    auto* effect = dynamic_cast<VST3EffectBase*>(component.get());
    ASSERT_NE(effect, nullptr);
    auto settings = effect->MakeSettings();
    const auto instance = effect->MakeInstance();
    instance->SetBlockSize(64);
    ASSERT_TRUE(instance->RealtimeInitialize(settings, 48000.0, 64));
    ASSERT_TRUE(instance->RealtimeAddProcessor(settings, nullptr, 2, 48000.0));
    ASSERT_TRUE(instance->RealtimeResume());
    const auto vst = std::dynamic_pointer_cast<VST3Instance>(instance);
    ASSERT_TRUE(vst);
    const auto controller = vst->vstEditController();
    ASSERT_TRUE(controller);
    Steinberg::FUnknownPtr<Steinberg::Vst::IConnectionPoint> connection(controller.get());
    ASSERT_TRUE(connection);
    auto message = Steinberg::owned(new Steinberg::Vst::HostMessage);
    message->setMessageID("arm-latency-on-activate");
    ASSERT_EQ(message->getAttributes()->setInt("samples", 11), Steinberg::kResultOk);
    ASSERT_EQ(connection->notify(message), Steinberg::kResultOk);
    ASSERT_EQ(controller->setParamNormalized(2, 7.0 / 31), Steinberg::kResultOk);
    ASSERT_TRUE(instance->RefreshRealtimeLatency());
    EXPECT_EQ(instance->GetLatency(settings, 48000), 11);
    EXPECT_EQ(instance->RealtimeLatencyRevision(), 1u);
    EXPECT_FALSE(instance->RealtimeLatencyUpdatePending());
    EXPECT_FALSE(instance->RefreshRealtimeLatency());
    ASSERT_TRUE(instance->RealtimeFinalize(settings));
}

TEST(NativeVstInstrumentTests, RefreshedNativeEffectDelayMatchesEveryOutputSampleAfterIncreasingAndDecreasingLatency)
{
    VST3EffectsModule provider;
    auto component = provider.LoadPlugin(wxString::FromUTF8(std::string(AU_NATIVE_TEST_INSTRUMENT_PATH)
                                                            + ";F5A81297A6724B218E434FA1A4050C03"));
    auto* effect = dynamic_cast<VST3EffectBase*>(component.get());
    ASSERT_NE(effect, nullptr);
    EXPECT_EQ(effect->GetType(), EffectTypeProcess);
    auto settings = effect->MakeSettings();
    const auto instance = effect->MakeInstance();
    instance->SetBlockSize(64);
    ASSERT_TRUE(instance->RealtimeInitialize(settings, 48000, 64));
    ASSERT_TRUE(instance->RealtimeAddProcessor(settings, nullptr, 2, 48000));
    ASSERT_TRUE(instance->RealtimeResume());
    const auto vst = std::dynamic_pointer_cast<VST3Instance>(instance);
    ASSERT_TRUE(vst);
    const auto controller = vst->vstEditController();
    ASSERT_TRUE(controller);
    std::array<float, 64> left {}, right {}, actualLeft {}, actualRight {};
    for (size_t i = 0; i < left.size(); ++i) {
        left[i] = static_cast<float>(i + 1) / 128;
        right[i] = -left[i];
    }
    const float* input[] { left.data(), right.data() };
    float* output[] { actualLeft.data(), actualRight.data() };
    uint64_t revision = 0;
    for (const int delay : { 7, 31, 3, 0 }) {
        SCOPED_TRACE(delay);
        ASSERT_EQ(controller->setParamNormalized(2, delay / 31.0), Steinberg::kResultOk);
        ASSERT_TRUE(instance->RealtimeLatencyUpdatePending());
        ASSERT_TRUE(instance->RefreshRealtimeLatency());
        EXPECT_EQ(instance->RealtimeLatencyRevision(), ++revision);
        ASSERT_EQ(instance->GetLatency(settings, 48000), delay);
        for (int block = 0; block < 2; ++block) {
            EffectInstance::MessagePackage package { settings, nullptr };
            ASSERT_TRUE(instance->RealtimeProcessStart(package));
            ASSERT_EQ(instance->RealtimeProcess(0, settings, input, output, left.size()), left.size());
            ASSERT_TRUE(instance->RealtimeProcessEnd(settings));
            for (size_t frame = 0; frame < left.size(); ++frame) {
                const auto expected = block == 0 && frame < static_cast<size_t>(delay)
                                      ? 0.0f : left[(frame + left.size() - delay) % left.size()] * 0.5f;
                ASSERT_FLOAT_EQ(actualLeft[frame], expected) << frame;
                ASSERT_FLOAT_EQ(actualRight[frame], -expected) << frame;
            }
        }
        EXPECT_FALSE(instance->RealtimeLatencyUpdatePending());
    }
    ASSERT_TRUE(instance->RealtimeFinalize(settings));
}

TEST(NativeVstInstrumentTests, OfflineChainMeasuresTheAutomatedDelayPublishedByTheRealVstProcessor)
{
    VST3EffectsModule provider;
    auto component = provider.LoadPlugin(wxString::FromUTF8(std::string(AU_NATIVE_TEST_INSTRUMENT_PATH)
                                                            + ";F5A81297A6724B218E434FA1A4050C03"));
    auto* effect = dynamic_cast<VST3EffectBase*>(component.get());
    ASSERT_NE(effect, nullptr);
    AutomationData automation;
    ASSERT_TRUE(automation.SetLane({ "2", true, { { 0, 7.0 / 31 } } }));
    NativeEffectChain chain({ { effect, effect->MakeSettings(), automation } }, 48000, 64);
    ASSERT_EQ(chain.latency(), 63u + 7u);
    std::array<float, 512> left {}, right {};
    left.fill(0.25f);
    right.fill(-0.5f);
    chain.process({ left.data(), right.data() }, left.size(), 0);
    for (size_t frame = 0; frame < left.size(); ++frame) {
        ASSERT_FLOAT_EQ(left[frame], frame < chain.latency() ? 0 : 0.125f) << frame;
        ASSERT_FLOAT_EQ(right[frame], frame < chain.latency() ? 0 : -0.25f) << frame;
    }
}

TEST(NativeVstInstrumentTests, OfflineChannelModesReleaseNativeKeysWithoutAnOptionalAllNotesOffParameter)
{
    for (const int mode : { 120, 123, 124, 125, 126, 127 }) {
        SCOPED_TRACE(mode);
        auto instrument = vstInstrument();
        instrument->prepare(48000, 512);
        std::array<float, 512> samples {};
        float* output[] { samples.data() };
        const std::array events { MidiEvent { 0, 0x90, 60, 127 }, MidiEvent { 173, 0xb0, static_cast<uint8_t>(mode), 0 } };
        instrument->process(output, 1, 512, events, {});
        for (size_t sample = 0; sample < samples.size(); ++sample) {
            EXPECT_FLOAT_EQ(samples[sample], sample < 173 ? 0.5f : 0.0f) << sample;
        }
    }
}

TEST(NativeVstInstrumentTests, SeekingRestoresVirtualCcKnobsWithoutCuttingSustainOrOverwritingNewAutomation)
{
    for (const int controller : { 11, 41 }) {
        for (const bool automate : { false, true }) {
            SCOPED_TRACE(testing::Message() << "controller=" << controller << " automation=" << automate);
            auto processor = vstInstrument(R"({"parameters":{"0":0.6}})");
            LiveInstrumentSession session(MidiTrack::Create(), processor);
            session.prepare(48000, 512);
            session.beginTimeline();
            session.sendMidi(0xb0, 64, 127);
            session.sendMidi(0x90, 60, 127);
            session.sendMidi(0x80, 60, 0);
            std::array<float, 512> samples {};
            float* output[] { samples.data() };
            session.process(output, 1, 512, {}, {});
            ASSERT_NEAR(samples[256], 0.6, 1e-6);
            const std::array cc { MidiEvent { 173, 0xb0, static_cast<uint8_t>(controller), 95 } };
            session.process(output, 1, 512, cc, {});
            EXPECT_NEAR(samples[172], 0.6, 1e-6);
            EXPECT_NEAR(samples[173], 95.0 / 127, 1e-6);
            session.resetTimeline();
            if (automate) {
                ASSERT_TRUE(session.setAutomatedParameter("0", 0.25));
            }
            session.process(output, 1, 512, {}, {});
            for (float sample : samples) {
                EXPECT_NEAR(sample, automate ? 0.25 : 0.6, 1e-6);
            }
            const auto saved = QJsonDocument::fromJson(QByteArray::fromStdString(session.state())).object();
            EXPECT_DOUBLE_EQ(saved["parameters"].toObject()["0"].toDouble(), 0.6);
        }
    }
}

TEST(NativeVstInstrumentTests, FailedControllerMappingQueriesCannotTurnUnmappedMidiIntoGainChanges)
{
    auto instrument = vstInstrument(R"({"parameters":{"0":0.6}})");
    instrument->prepare(48000, 512);
    std::array<float, 512> samples {};
    float* output[] { samples.data() };
    const std::array input { MidiEvent { 0, 0x90, 60, 127 }, MidiEvent { 100, 0xb0, 1, 0 },
                             MidiEvent { 200, 0xe0, 0, 0 }, MidiEvent { 300, 0xd0, 0, 0 } };
    instrument->process(output, 1, 512, input, {});
    for (float sample : samples) {
        EXPECT_NEAR(sample, 0.6, 1e-6);
    }
}

TEST(NativeVstInstrumentTests, ControllerResetLiftsThePedalWithoutAnOptionalResetControllerMapping)
{
    auto instrument = vstInstrument();
    instrument->prepare(48000, 512);
    std::array<float, 512> samples {};
    float* output[] { samples.data() };
    const std::array input { MidiEvent { 0, 0xb0, 64, 127 }, MidiEvent { 40, 0x90, 60, 127 },
                             MidiEvent { 100, 0x80, 60, 0 }, MidiEvent { 173, 0xb0, 121, 0 } };
    instrument->process(output, 1, 512, input, {});
    for (size_t i = 0; i < samples.size(); ++i) {
        EXPECT_FLOAT_EQ(samples[i], i >= 40 && i < 173 ? 0.5f : 0.0f) << i;
    }
}

TEST(NativeVstInstrumentTests, ControllerMappingHoldsNotesUntilThePedalIsReleased)
{
    auto instrument = vstInstrument();
    instrument->prepare(48000.0, 512);
    std::vector<float> buffer(512);
    float* output[] { buffer.data() };
    const std::array events { MidiEvent { 0, 0xb0, 64, 127 }, MidiEvent { 40, 0x90, 60, 100 },
                              MidiEvent { 100, 0x80, 60, 0 }, MidiEvent { 400, 0xb0, 64, 0 } };
    instrument->process(output, 1, 512, events, {});
    EXPECT_GT(buffer[200], 0.0f);
    EXPECT_GT(buffer[399], 0.0f);
    EXPECT_EQ(buffer[400], 0.0f);
}

TEST(NativeVstInstrumentTests, SavedPluginStateRestoresParametersAndRenderedAudio)
{
    auto original = vstInstrument();
    original->prepare(48000.0, 512);
    ASSERT_TRUE(original->setParameter("0", 0.25));
    std::vector<float> buffer(512);
    float* output[] { buffer.data() };
    const std::array events { MidiEvent { 0, 0x90, 60, 127 } };
    original->process(output, 1, 512, events, {});
    EXPECT_FLOAT_EQ(buffer[100], 0.25f);
    auto restored = vstInstrument(original->state());
    restored->prepare(48000.0, 512);
    restored->process(output, 1, 512, events, {});
    EXPECT_FLOAT_EQ(buffer[100], 0.25f);
}

TEST(NativeVstInstrumentTests, ParameterChangesMadeBeforePlaybackSurviveSaveAndReopen)
{
    auto original = vstInstrument();
    ASSERT_TRUE(original->setParameter("0", 0.37));
    auto restored = vstInstrument(original->state());
    restored->prepare(48000.0, 512);
    std::vector<float> buffer(512);
    float* output[] { buffer.data() };
    const std::array events { MidiEvent { 0, 0x90, 60, 127 } };
    restored->process(output, 1, 512, events, {});
    EXPECT_FLOAT_EQ(buffer[100], 0.37f);
}

TEST(NativeVstInstrumentTests, ClosingHeldKeyboardNotesDoesNotRequireAnAllNotesOffParameter)
{
    auto track = MidiTrack::Create();
    auto source = std::make_shared<LiveInstrumentSession>(track, vstInstrument());
    source->Prepare(48000.0);
    std::array<float, 1024> output {};
    source->sendMidi(0x90, 60, 100);
    source->Render(output.data(), 2, 512);
    EXPECT_GT(output[100], 0.0f);
    source->releaseNotes();
    source->Render(output.data(), 2, 512);
    EXPECT_TRUE(std::all_of(output.begin(), output.end(), [](float value) { return value == 0.0f; }));
}
TEST(NativeVstInstrumentTests, TheSameNativeProcessorReceivesEditorChangesInMonitoringAndTimelinePlayback)
{
    const auto track = MidiTrack::Create();
    const auto session = std::make_shared<LiveInstrumentSession>(track, vstInstrument());
    session->Prepare(48000.0);
    ASSERT_TRUE(session->setParameter("0", 0.25));
    session->sendMidi(0x90, 60, 127);
    std::array<float, 1024> live {};
    session->Render(live.data(), 2, 512);
    EXPECT_FLOAT_EQ(live[100], 0.25f);
    session->beginTimeline();
    MidiPlayableSequence sequence(track, session, 48000.0, true, false);
    std::array<float, 512> left {}, right {};
    float* buffers[] { left.data(), right.data() };
    ASSERT_TRUE(sequence.GetFloats(0, 2, buffers, 0, 512));
    EXPECT_FLOAT_EQ(left[100], 0.25f);
    ASSERT_TRUE(session->setParameter("0", 0.75));
    ASSERT_TRUE(sequence.GetFloats(0, 2, buffers, 512, 512));
    EXPECT_FLOAT_EQ(left[100], 0.75f);
    session->endTimeline();
    session->Prepare(48000.0);
    session->Render(live.data(), 2, 512);
    EXPECT_FLOAT_EQ(live[100], 0.75f);
    session->takeChanges();
    ASSERT_TRUE(session->setAutomatedParameter("0", 0.37));
    EXPECT_FALSE(session->takeChanges());
}

TEST(NativeVstInstrumentTests, AutomatedNativeStateRestoresManualParametersAndReadBypassKeepsSoundingNotes)
{
    auto instrument = vstInstrument();
    instrument->prepare(48000.0, 512);
    ASSERT_TRUE(instrument->setParameter("0", 0.6));
    instrument->takeChanges();
    ASSERT_TRUE(instrument->setAutomatedParameter("0", 0.2));
    std::array<float, 512> left {}, right {};
    float* output[] { left.data(), right.data() };
    const std::array events { MidiEvent { 0, 0x90, 60, 127 } };
    instrument->process(output, 2, 512, events, {});
    EXPECT_FLOAT_EQ(left[100], 0.2f);
    EXPECT_DOUBLE_EQ(instrument->parameters()[0].value, 0.6);
    EXPECT_FALSE(instrument->takeChanges());
    auto restored = createVstInstrument(std::string(AU_NATIVE_TEST_INSTRUMENT_PATH) + ";F5A81297A6724B218E434FA1A4050C01",
                                        instrument->state());
    restored->prepare(48000.0, 512);
    restored->process(output, 2, 512, events, {});
    EXPECT_FLOAT_EQ(left[100], 0.6f);
    instrument->restoreManualParameters();
    instrument->process(output, 2, 512, {}, { 512 });
    EXPECT_FLOAT_EQ(left[100], 0.6f);
}

#endif
}
