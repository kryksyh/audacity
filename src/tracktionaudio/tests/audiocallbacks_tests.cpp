/*
* Audacity: A Digital Audio Editor
*/
#include "../internal/tracktionaudiocallbacks.h"
#include "../internal/audacityplugins.h"
#include "audio/internal/audiometer.h"
#include "audio/internal/itimer.h"
#include "au3wrap/internal/au3audiometer.h"

#include <gtest/gtest.h>

namespace au::tracktionaudio {
namespace {
class ManualTimer final : public ITimer
{
public:
    void setCallback(std::function<void()> callback) override { m_callback = std::move(callback); }
    void setInterval(const std::chrono::milliseconds&) override {}
    void setSingleShot(bool) override {}
    void start() override { m_running = true; }
    void stop() override { m_running = false; }
    void tick()
    {
        if (m_running && m_callback) {
            m_callback();
        }
    }

private:
    std::function<void()> m_callback;
    bool m_running = false;
};

class Meter final : public IMeterSender
{
public:
    void start(double rate) override { sampleRate = rate; ++starts; }
    void stop() override { ++stops; }
    void pushLevels(uint8_t, const LevelData&, const std::optional<TrackId>&) override {}
    void push(uint8_t channel, const InterleavedSampleData& data, const std::optional<TrackId>& track) override
    {
        channels.push_back(channel);
        samples.emplace_back();
        for (size_t frame = 0; frame < data.frames; ++frame) {
            samples.back().push_back(data.buffer[frame * data.nChannels]);
        }
        time = data.dacTime;
        times.push_back(data.dacTime);
        tracks.push_back(track ? std::make_optional(track->value) : std::nullopt);
    }

    void expectSamples(const std::vector<std::vector<float> >& expected) const
    {
        ASSERT_EQ(samples.size(), expected.size());
        for (size_t block = 0; block < expected.size(); ++block) {
            ASSERT_EQ(samples[block].size(), expected[block].size());
            for (size_t frame = 0; frame < expected[block].size(); ++frame) {
                EXPECT_FLOAT_EQ(samples[block][frame], expected[block][frame]) << block << ", " << frame;
            }
        }
    }

    double sampleRate = 0.0;
    int starts = 0;
    int stops = 0;
    TimePoint time;
    std::vector<uint8_t> channels;
    std::vector<std::vector<float> > samples;
    std::vector<TimePoint> times;
    std::vector<std::optional<int64_t> > tracks;
};
}

TEST(AudioCallbacksTests, PreservesTheDeviceClockAndLastSampleTime)
{
    TracktionAudioCallbacks callback;
    auto controls = std::make_shared<AudacityPlaybackControls>();
    callback.setPlaybackControls(controls);
    callback.prepare(48000.0, 256, 0, 2);
    const auto time = AudioDeviceCallback::TimePoint(std::chrono::seconds(12));
    float left[256] {}, right[256] {};
    float* output[] { left, right };
    callback.processOutput(output, 2, 256, time);
    EXPECT_FALSE(callback.playbackEndTime());
    controls->endSampleOffset.store(235);
    callback.processOutput(output, 2, 256, time + std::chrono::seconds(1));
    callback.processOutput(output, 2, 256, time + std::chrono::seconds(2));
    ASSERT_TRUE(callback.playbackEndTime());
    EXPECT_NEAR(std::chrono::duration<double>(*callback.playbackEndTime() - time).count(), 1.0 + 235.0 / 48000.0, 1e-9);
    for (int block = 0; block < 3; ++block) {
        const auto info = callback.consumeCallbackInfo();
        ASSERT_TRUE(info);
        EXPECT_EQ(info->numSamples, 256);
        EXPECT_EQ(info->dacTime, time + std::chrono::seconds(block));
    }
    EXPECT_FALSE(callback.consumeCallbackInfo());
    callback.setPlaybackControls(controls);
    EXPECT_FALSE(callback.playbackEndTime());
    callback.stop();
}

TEST(AudioCallbacksTests, IdleDeviceCallbacksDoNotRetainClockOrMeterData)
{
    TracktionAudioCallbacks callback;
    float samples[4] {};
    float* output[] { samples };
    const float* input[] { samples };
    const auto time = AudioDeviceCallback::TimePoint(std::chrono::seconds(12));
    callback.prepare(48000.0, 4, 1, 1);
    callback.processOutput(output, 1, 4, time);
    ASSERT_TRUE(callback.consumeCallbackInfo());
    callback.stop();
    const auto meter = std::make_shared<Meter>();
    callback.setMeters(meter, meter);
    callback.processInput(input, 1, 4, time);
    callback.processOutput(output, 1, 4, time);
    EXPECT_TRUE(meter->samples.empty());
    EXPECT_FALSE(callback.consumeCallbackInfo());
    callback.prepare(48000.0, 4, 1, 1);
    callback.processInput(input, 1, 4, time + std::chrono::seconds(1));
    callback.processOutput(output, 1, 4, time + std::chrono::seconds(1));
    EXPECT_EQ(meter->samples.size(), 2u);
    const auto info = callback.consumeCallbackInfo();
    ASSERT_TRUE(info);
    EXPECT_EQ(info->dacTime, time + std::chrono::seconds(1));
    callback.stop();
}

TEST(AudioCallbacksTests, SendsEachPhysicalChannelToTheProjectMeters)
{
    TracktionAudioCallbacks callback;
    const auto inputMeter = std::make_shared<Meter>();
    const auto outputMeter = std::make_shared<Meter>();
    callback.setMeters(inputMeter, outputMeter);
    callback.prepare(48000.0, 4, 1, 2);
    const auto time = AudioDeviceCallback::TimePoint(std::chrono::seconds(12));
    float left[] { 0.125f, 0.25f, -0.125f, -0.25f };
    float right[] { 0.5f, -0.5f, 0.75f, -0.75f };
    const float* input[] { left };
    float* output[] { left, right };
    callback.processInput(input, 1, 4, time);
    callback.processOutput(output, 2, 4, time + std::chrono::seconds(1));
    EXPECT_DOUBLE_EQ(inputMeter->sampleRate, 48000.0);
    EXPECT_EQ(inputMeter->channels, (std::vector<uint8_t> { 0 }));
    EXPECT_EQ(outputMeter->channels, (std::vector<uint8_t> { 0, 1 }));
    EXPECT_EQ(inputMeter->samples, (std::vector<std::vector<float> > { { 0.125f, 0.25f, -0.125f, -0.25f } }));
    EXPECT_EQ(outputMeter->samples, (std::vector<std::vector<float> > {
            { 0.125f, 0.25f, -0.125f, -0.25f }, { 0.5f, -0.5f, 0.75f, -0.75f }
        }));
    EXPECT_EQ(inputMeter->time, time);
    EXPECT_EQ(outputMeter->time, time + std::chrono::seconds(1));
    const auto replacement = std::make_shared<Meter>();
    callback.setMeters(inputMeter, replacement);
    EXPECT_EQ(inputMeter->starts, 1);
    EXPECT_EQ(inputMeter->stops, 0);
    EXPECT_EQ(outputMeter->stops, 1);
    EXPECT_EQ(replacement->starts, 1);
    callback.stop();
    EXPECT_EQ(inputMeter->stops, 1);
    EXPECT_EQ(replacement->stops, 1);
    EXPECT_FALSE(callback.consumeCallbackInfo());
}

TEST(AudioCallbacksTests, InputMetersFollowTheTracktionInputGain)
{
    TracktionAudioCallbacks callback;
    const auto meter = std::make_shared<Meter>();
    callback.setMeters(meter, {});
    callback.prepare(48000.0, 4, 1, 0);
    const float source[] { 0.125f, 0.25f, -0.125f, -0.25f };
    const float* input[] { source };
    const auto time = AudioDeviceCallback::TimePoint(std::chrono::seconds(12));
    const std::pair<float, float> cases[] { { 0.5f, 0.5f }, { 0.0f, 0.0f }, { 2.0f, 2.0f }, { 1.0001f, 1.0f }, { 1.0f, 1.0f } };
    for (const auto& [gain, expectedGain] : cases) {
        SCOPED_TRACE(gain);
        callback.setInputGain(gain);
        callback.processInput(input, 1, 4, time);
        ASSERT_EQ(meter->samples.back().size(), 4u);
        for (int frame = 0; frame < 4; ++frame) {
            EXPECT_NEAR(meter->samples.back()[frame], source[frame] * expectedGain, 1e-6f);
        }
        EXPECT_EQ(meter->time, time);
    }
    EXPECT_FLOAT_EQ(source[1], 0.25f);
    callback.stop();
}

TEST(AudioCallbacksTests, RecordingMetersUseTheRecordingTrackIdsAndChannelOrder)
{
    TracktionAudioCallbacks callback;
    const auto meter = std::make_shared<Meter>();
    callback.setMeters(meter, {});
    callback.setCaptureTracks({ { 17, 1 }, { 23, 1 } });
    callback.setInputGain(0.5f);
    callback.prepare(48000.0, 2, 2, 0);
    const float left[] { 0.25f, -0.5f };
    const float right[] { 0.125f, -0.25f };
    const float* input[] { left, right };
    const auto time = AudioDeviceCallback::TimePoint(std::chrono::seconds(12));
    callback.processInput(input, 2, 2, time);
    EXPECT_EQ(meter->tracks, (std::vector<std::optional<int64_t> > { 17, 23, std::nullopt, std::nullopt }));
    EXPECT_EQ(meter->channels, (std::vector<uint8_t> { 0, 0, 0, 1 }));
    meter->expectSamples({ { 0.125f, -0.25f }, { 0.0625f, -0.125f }, { 0.125f, -0.25f }, { 0.0625f, -0.125f } });
    callback.stop();
}

TEST(AudioCallbacksTests, RecordingMetersMatchMonoAndStereoRecordingConversions)
{
    TracktionAudioCallbacks callback;
    const auto monoMeter = std::make_shared<Meter>();
    callback.setMeters(monoMeter, {});
    callback.setCaptureTracks({ { 17, 1 } });
    callback.prepare(48000.0, 2, 2, 0);
    const float left[] { 0.5f, -0.5f };
    const float right[] { 0.25f, 0.0f };
    const float* stereo[] { left, right };
    const auto time = AudioDeviceCallback::TimePoint(std::chrono::seconds(12));
    callback.processInput(stereo, 2, 2, time);
    EXPECT_EQ(monoMeter->samples, (std::vector<std::vector<float> > { { 0.375f, -0.25f }, { 0.5f, -0.5f }, { 0.25f, 0.0f } }));
    EXPECT_EQ(monoMeter->tracks, (std::vector<std::optional<int64_t> > { 17, std::nullopt, std::nullopt }));
    callback.stop();

    const auto stereoMeter = std::make_shared<Meter>();
    callback.setMeters(stereoMeter, {});
    callback.setCaptureTracks({ { 23, 2 } });
    callback.prepare(48000.0, 2, 1, 0);
    const float* mono[] { left };
    callback.processInput(mono, 1, 2, time);
    EXPECT_EQ(stereoMeter->tracks, (std::vector<std::optional<int64_t> > { 23, 23, std::nullopt }));
    EXPECT_EQ(stereoMeter->channels, (std::vector<uint8_t> { 0, 1, 0 }));
    EXPECT_EQ(stereoMeter->samples, (std::vector<std::vector<float> > { { 0.5f, -0.5f }, { 0.5f, -0.5f }, { 0.5f, -0.5f } }));
    callback.stop();
}

TEST(AudioCallbacksTests, MultichannelRecordingMetersKeepTracksAndTheTwoMainColumns)
{
    TracktionAudioCallbacks callback;
    const auto meter = std::make_shared<Meter>();
    callback.setMeters(meter, {});
    callback.setCaptureTracks({ { 17, 1 }, { 23, 1 }, { 29, 1 }, { 31, 1 } });
    callback.prepare(48000.0, 2, 4, 0);
    const float first[] { 0.5f, 0.25f };
    const float second[] { 0.25f, -0.5f };
    const float third[] { -0.75f, -0.125f };
    const float fourth[] { -0.5f, 0.125f };
    const float* input[] { first, second, third, fourth };
    callback.processInput(input, 4, 2, AudioDeviceCallback::TimePoint(std::chrono::seconds(12)));
    EXPECT_EQ(meter->tracks, (std::vector<std::optional<int64_t> > { 17, 23, 29, 31, std::nullopt, std::nullopt }));
    EXPECT_EQ(meter->channels, (std::vector<uint8_t> { 0, 0, 0, 0, 0, 1 }));
    EXPECT_EQ(meter->samples, (std::vector<std::vector<float> > {
            { 0.5f, 0.25f }, { 0.25f, -0.5f }, { -0.75f, -0.125f }, { -0.5f, 0.125f }, { -0.75f, 0.25f }, { -0.5f, -0.5f }
        }));
    callback.stop();
}

TEST(AudioCallbacksTests, InputMeterChunksKeepTheClockAndResetBetweenStreams)
{
    TracktionAudioCallbacks callback;
    const auto meter = std::make_shared<Meter>();
    callback.setMeters(meter, {});
    callback.setInputGain(0.5f);
    callback.setCaptureTracks({ { 17, 1 } });
    callback.prepare(48000.0, 2, 1, 0);
    const float samples[] { 0.125f, 0.25f, -0.125f, -0.25f, 0.5f };
    const float* input[] { samples };
    const auto time = AudioDeviceCallback::TimePoint(std::chrono::seconds(12));
    callback.processInput(input, 1, 5, time);
    meter->expectSamples({ { 0.0625f, 0.125f }, { 0.0625f, 0.125f }, { -0.0625f, -0.125f }, { -0.0625f, -0.125f }, { 0.25f }, { 0.25f } });
    ASSERT_EQ(meter->times.size(), 6u);
    for (int block = 0; block < 3; ++block) {
        const auto expected = time + std::chrono::duration_cast<AudioDeviceCallback::TimePoint::duration>(
            std::chrono::duration<double>(block * 2.0 / 48000.0));
        EXPECT_EQ(meter->times[block * 2], expected);
        EXPECT_EQ(meter->times[block * 2 + 1], expected);
    }
    callback.stop();
    const auto replacement = std::make_shared<Meter>();
    callback.setMeters(replacement, {});
    callback.prepare(48000.0, 2, 1, 0);
    callback.processInput(input, 1, 2, time);
    EXPECT_EQ(replacement->tracks, (std::vector<std::optional<int64_t> > { std::nullopt }));
    EXPECT_EQ(replacement->samples, (std::vector<std::vector<float> > { { 0.125f, 0.25f } }));
    callback.stop();
}

TEST(AudioCallbacksTests, MeasuredLevelsReachTheExistingAudacityMeterQueue)
{
    auto timer = std::make_unique<ManualTimer>();
    auto* playingTimer = timer.get();
    au3::Au3AudioMeter meter(std::make_unique<audio::AudioMeter>(std::move(timer), std::make_unique<ManualTimer>()));
    std::vector<audio::MeterSignal> received;
    meter.dataChanged(audio::IAudioMeter::TrackId { 17 }).onReceive(nullptr, [&](uint8_t channel, const audio::MeterSignal& signal) {
        EXPECT_EQ(channel, 0);
        received.push_back(signal);
    });
    meter.start(48000.0);
    const auto time = std::chrono::steady_clock::now() - std::chrono::seconds(1);
    meter.pushLevels(0, { 0.75f, 0.25f, 256, time }, IMeterSender::TrackId { 17 });
    playingTimer->tick();
    ASSERT_EQ(received.size(), 1u);
    EXPECT_NEAR(received.back().peak.amplitude, 0.75f, 1e-6f);
    EXPECT_NEAR(received.back().rms.amplitude, 0.25f, 1e-6f);
    meter.pushLevels(0, { 2.0f, 1.5f, 256, time }, IMeterSender::TrackId { 17 });
    playingTimer->tick();
    ASSERT_EQ(received.size(), 2u);
    EXPECT_FLOAT_EQ(received.back().peak.amplitude, 1.0f);
    EXPECT_FLOAT_EQ(received.back().rms.amplitude, 1.0f);
    meter.stop();
    meter.start(48000.0);
    const float samples[] { 0.5f, -0.5f };
    meter.push(0, { samples, 2, 1, time }, IMeterSender::TrackId { 17 });
    playingTimer->tick();
    ASSERT_EQ(received.size(), 3u);
    EXPECT_NEAR(received.back().peak.amplitude, 0.5f, 1e-6f);
    EXPECT_NEAR(received.back().rms.amplitude, 0.5f, 1e-6f);
    meter.stop();
}
}
