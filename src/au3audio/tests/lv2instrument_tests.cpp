/*
* Audacity: A Digital Audio Editor
*/
#include <gtest/gtest.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdlib>
#include <thread>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QUrl>
#include <QApplication>
#include <QPointer>
#include <QSlider>
#include <QPushButton>
#include <QWindow>
#include <wx/log.h>

#include "../internal/instrumentprocessor.h"
#include "../internal/midiplayablesequence.h"
#include "../internal/liveinstrumentsession.h"
#include "../internal/nativeeffectparameters.h"
#include "../internal/nativeeffectchain.h"
#include "au3-mixer/Mix.h"
#include "au3-playable-track/TrackAutomation.h"
#include "au3-lv2/LoadLV2.h"
#include "au3-lv2/LV2Instance.h"
#include "au3-module-manager/PluginManager.h"
#include "au3-utility/MemoryX.h"
#include <lv2/atom/util.h>

namespace au::au3audio {
namespace {
class LoopInstrumentProbe final : public InstrumentProcessor
{
public:
    explicit LoopInstrumentProbe(std::shared_ptr<InstrumentProcessor> processor)
        : processor(std::move(processor)) {}
    void prepare(double rate, size_t frames) override { processor->prepare(rate, frames); }
    void reset() override { ++resets; processor->reset(); }
    size_t latency() const override { return processor->latency(); }
    std::vector<audio::InstrumentParameter> parameters() const override { return processor->parameters(); }
    bool setParameter(const std::string& id, double value) override { return processor->setParameter(id, value); }
    std::string state() const override { return processor->state(); }
    bool takeChanges() override { return processor->takeChanges(); }
    void restoreManualParameters() noexcept override { processor->restoreManualParameters(); }
    void process(float* const* output, size_t channels, size_t frames, std::span<const MidiEvent> events,
                 const InstrumentProcessContext& context) override
    {
        rendered += frames;
        for (const auto& event : events) {
            if ((event.status & 0xf0) == 0x90 && event.data2) {
                ++ons[event.data1];
            }
        }
        processor->process(output, channels, frames, events, context);
    }

    std::shared_ptr<InstrumentProcessor> processor;
    std::array<size_t, 128> ons {};
    size_t resets = 0, rendered = 0;
};

std::string fixturePath(bool fixed = false)
{
    return std::string(fixed ? "urn:audacity:test:fixed-instrument@" : "urn:audacity:test:native-instrument@")
           + QUrl::fromLocalFile(QString::fromUtf8(AU_NATIVE_TEST_LV2_PATH)).toEncoded().toStdString();
}
}

TEST(NativeLv2DiscoveryDeathTest, PluginSearchPathsRemainLocalToTheWorld)
{
    ASSERT_EXIT({
            QDir plugins(QString::fromUtf8(AU_NATIVE_TEST_LV2_PATH));
            if (!plugins.cdUp()) {
                std::_Exit(1);
            }
            const auto path = QFile::encodeName(plugins.absolutePath());
            qputenv("LV2_PATH", path);
            LV2EffectsModule provider;
            if (!provider.Initialize()) {
                std::_Exit(2);
            }
            provider.AutoRegisterPlugins(PluginManager::Get());
            if (qgetenv("LV2_PATH") != path) {
                std::_Exit(3);
            }
            const LilvNodePtr uri { lilv_new_uri(LV2Symbols::gWorld, "urn:audacity:test:native-instrument") };
            const auto* found = lilv_plugins_get_by_uri(lilv_world_get_all_plugins(LV2Symbols::gWorld), uri.get());
            std::_Exit(found ? 0 : 4);
        }, testing::ExitedWithCode(0), "");
}

TEST(NativeLv2Tests, MidiOffsetsAndHostTempoReachTheNativePlugin)
{
    auto instrument = createLv2Instrument(fixturePath(), {});
    instrument->prepare(48000.0, 512);
    std::vector<float> left(512), right(512);
    float* output[] { left.data(), right.data() };
    const std::array events { MidiEvent { 128, 0x90, 60, 127 }, MidiEvent { 300, 0x80, 60, 0 } };
    InstrumentProcessContext context;
    context.tempo = 180.0;
    instrument->process(output, 2, 512, events, context);
    for (size_t i = 0; i < left.size(); ++i) {
        EXPECT_FLOAT_EQ(left[i], i >= 128 && i < 300 ? 0.75f : 0.0f) << i;
    }
    EXPECT_EQ(left, right);
    EXPECT_EQ(instrument->latency(), 0u);
}

TEST(NativeLv2Tests, AutomationUpdatesDspWithoutMarkingTheInstrumentAsManuallyEdited)
{
    const auto instrument = createLv2Instrument(fixturePath(), {});
    instrument->prepare(48000.0, 512);
    instrument->takeChanges();
    ASSERT_TRUE(instrument->setAutomatedParameter("gain", 0.25));
    EXPECT_FALSE(instrument->takeChanges());
    std::array<float, 512> left {}, right {};
    float* output[] { left.data(), right.data() };
    const std::array events { MidiEvent { 0, 0x90, 60, 127 } };
    instrument->process(output, 2, 512, events, {});
    EXPECT_FLOAT_EQ(left[100], 0.25f);
    ASSERT_TRUE(instrument->setParameter("gain", 0.5));
    EXPECT_TRUE(instrument->takeChanges());
}

TEST(NativeLv2Tests, AutomatedNativeStateKeepsManualPortsAndRestoresThemWithoutReleasingNotes)
{
    auto instrument = createLv2Instrument(fixturePath(), {});
    instrument->prepare(48000.0, 512);
    ASSERT_TRUE(instrument->setParameter("gain", 0.6));
    instrument->takeChanges();
    ASSERT_TRUE(instrument->setAutomatedParameter("gain", 0.2));
    std::array<float, 512> left {}, right {};
    float* output[] { left.data(), right.data() };
    const std::array events { MidiEvent { 0, 0x90, 60, 127 } };
    instrument->process(output, 2, 512, events, {});
    EXPECT_FLOAT_EQ(left[100], 0.2f);
    EXPECT_DOUBLE_EQ(instrument->parameters()[0].value, 0.6);
    EXPECT_FALSE(instrument->takeChanges());
    auto restored = createLv2Instrument(fixturePath(), instrument->state());
    restored->prepare(48000.0, 512);
    restored->process(output, 2, 512, events, {});
    EXPECT_FLOAT_EQ(left[100], 0.6f);
    instrument->restoreManualParameters();
    instrument->process(output, 2, 512, {}, { 512 });
    EXPECT_FLOAT_EQ(left[100], 0.6f);
}

TEST(NativeLv2Tests, SavingWhileRunChangesProcessingPortsKeepsTheManualPresetAndPrivateFiles)
{
    auto instrument = createLv2Instrument(fixturePath(), {});
    instrument->prepare(48000.0, 512);
    ASSERT_TRUE(instrument->setParameter("gain", 0.6));
    std::array<float, 512> left {}, right {};
    float* output[] { left.data(), right.data() };
    const std::array on { MidiEvent { 0, 0x90, 60, 127 } };
    instrument->process(output, 2, 512, on, {});
    std::atomic<size_t> blocks { 0 };
    std::atomic<bool> invalidAudio { false };
    std::atomic<bool> stop { false };
    std::thread worker([&] {
        int64_t sample = 512;
        while (!stop.load(std::memory_order_relaxed)) {
            const double gain = (sample / 512) % 2 ? 0.2 : 0.4;
            instrument->setAutomatedParameter("gain", gain);
            instrument->process(output, 2, 512, {}, { sample });
            if (!std::all_of(left.begin(), left.end(), [gain](float value) { return value == static_cast<float>(gain); })) {
                invalidAudio.store(true);
            }
            sample += 512;
            blocks.fetch_add(1, std::memory_order_release);
        }
    });
    auto join = finally([&] {
        stop.store(true, std::memory_order_relaxed);
        if (worker.joinable()) {
            worker.join();
        }
    });
    while (blocks.load(std::memory_order_acquire) == 0) {
        std::this_thread::yield();
    }
    std::string saved;
    for (int i = 0; i < 12; ++i) {
        saved = instrument->state();
        const auto object = QJsonDocument::fromJson(QByteArray::fromStdString(saved)).object();
        EXPECT_DOUBLE_EQ(object["parameters"].toObject()["gain"].toDouble(), 0.6);
        EXPECT_NE(saved.find("asset.bin"), std::string::npos);
    }
    stop.store(true, std::memory_order_relaxed);
    worker.join();
    EXPECT_FALSE(invalidAudio.load());
    EXPECT_GT(blocks.load(), 0u);
    auto restored = createLv2Instrument(fixturePath(), saved);
    restored->prepare(48000.0, 512);
    restored->process(output, 2, 512, on, {});
    EXPECT_FLOAT_EQ(left[100], 0.6f);
    EXPECT_DOUBLE_EQ(restored->parameters()[0].value, 0.6);
}

TEST(NativeLv2Tests, SustainAndResetReachTheOriginalLv2Host)
{
    auto instrument = createLv2Instrument(fixturePath(), {});
    instrument->prepare(48000.0, 512);
    std::vector<float> result(512);
    float* output[] { result.data() };
    const std::array events { MidiEvent { 0, 0xb0, 64, 127 }, MidiEvent { 0, 0x90, 60, 127 },
                              MidiEvent { 128, 0x80, 60, 0 }, MidiEvent { 400, 0xb0, 64, 0 } };
    instrument->process(output, 1, 512, events, {});
    EXPECT_FLOAT_EQ(result[399], 0.5f);
    EXPECT_FLOAT_EQ(result[400], 0.0f);
    const std::array on { MidiEvent { 0, 0x90, 64, 127 } };
    instrument->process(output, 1, 512, on, {});
    EXPECT_FLOAT_EQ(result.back(), 0.5f);
    instrument->reset();
    instrument->process(output, 1, 512, {}, {});
    EXPECT_TRUE(std::all_of(result.begin(), result.end(), [](float value) { return value == 0.0f; }));
}

TEST(NativeLv2Tests, StateArchivesPrivatePropertiesAndSampleFilesBeyondInstanceLifetime)
{
    std::string saved;
    {
        auto original = createLv2Instrument(fixturePath(), {});
        original->prepare(48000.0, 512);
        std::vector<float> result(512);
        float* output[] { result.data() };
        const std::array program { MidiEvent { 0, 0xc0, 2, 0 } };
        original->process(output, 1, 512, program, {});
        ASSERT_TRUE(original->setParameter("gain", 0.25));
        saved = original->state();
        EXPECT_NE(saved.find("asset.bin"), std::string::npos);
    }
    // Original instance and all its temporary files have gone away.
    auto restored = createLv2Instrument(fixturePath(), saved);
    restored->prepare(96000.0, 512);
    ASSERT_EQ(restored->parameters().size(), 1u);
    EXPECT_DOUBLE_EQ(restored->parameters()[0].value, 0.25);
    std::vector<float> result(512);
    float* output[] { result.data() };
    const std::array note { MidiEvent { 0, 0x90, 60, 127 } };
    restored->process(output, 1, 512, note, {});
    EXPECT_FLOAT_EQ(result[128], 0.75f);
}

TEST(NativeLv2Tests, FixedBlockInstrumentAcceptsPartialMixerRequestsWithDeclaredLatency)
{
    auto instrument = createLv2Instrument(fixturePath(true), {});
    instrument->prepare(48000.0, 512);
    ASSERT_EQ(instrument->latency(), 511u);
    std::vector<float> result;
    size_t position = 0;
    for (size_t frames : { 113u, 211u, 188u, 64u, 448u, 137u, 375u }) {
        std::vector<float> block(frames);
        float* output[] { block.data() };
        std::vector<MidiEvent> events;
        if (position <= 70 && position + frames > 70) {
            events.push_back({ static_cast<int64_t>(70 - position), 0x90, 60, 127 });
        }
        if (position <= 600 && position + frames > 600) {
            events.push_back({ static_cast<int64_t>(600 - position), 0x80, 60, 0 });
        }
        InstrumentProcessContext context;
        context.sample = position;
        instrument->process(output, 1, frames, events, context);
        result.insert(result.end(), block.begin(), block.end());
        position += frames;
    }
    for (size_t i = 0; i < result.size(); ++i) {
        EXPECT_FLOAT_EQ(result[i], i >= 70 + 511 && i < 600 + 511 ? 0.5f : 0.0f) << i;
    }
}

TEST(NativeLv2Tests, AudacityMixerCompensatesFixedInstrumentLatencyAtStartAndAfterSeek)
{
    auto track = MidiTrack::Create();
    // At 120 BPM and 48 kHz this note starts at frame 240 and ends at 720.
    ASSERT_TRUE(track->SetClips({ { 1, "Phrase", 0.0, 1.0, 1, { { 1, 0.01, 0.02, 60, 127 } }, {} } }));
    auto sequence = std::make_shared<MidiPlayableSequence>(track, createLv2Instrument(fixturePath(true), std::string {}), 48000.0);
    Mixer::Inputs inputs { Mixer::Input { sequence } };
    Mixer mixer(std::move(inputs), std::nullopt, true, Mixer::WarpOptions { static_cast<const AudacityProject*>(nullptr) },
                0.0, 1.0, 2, 512, false, 48000.0, floatSample);
    for (int pass = 0; pass < 2; ++pass) {
        if (pass) {
            mixer.Reposition(0.0, true);
        }
        std::vector<float> result;
        for (int block = 0; block < 2; ++block) {
            ASSERT_EQ(mixer.Process(512), 512u);
            const auto* samples = reinterpret_cast<const float*>(mixer.GetBuffer(0));
            result.insert(result.end(), samples, samples + 512);
        }
        for (size_t i = 0; i < result.size(); ++i) {
            EXPECT_FLOAT_EQ(result[i], i >= 240 && i < 720 ? 0.5f : 0.0f) << i;
        }
    }
}

TEST(NativeLv2Tests, MixerMeasuresInstrumentPublishedDelayAtStartAndAfterManualPresetChanges)
{
    auto track = MidiTrack::Create();
    ASSERT_TRUE(track->SetClips({ { 1, "Phrase", 0.0, 1.0, 1,
                                    { { 1, 100.0 / 24000, 200.0 / 24000, 60, 127 } }, {} } }));
    const auto path = std::string("urn:audacity:test:delayed-instrument@")
                      + QUrl::fromLocalFile(QString::fromUtf8(AU_NATIVE_TEST_LV2_PATH)).toEncoded().toStdString();
    auto instrument = createLv2Instrument(path, {});
    auto sequence = std::make_shared<MidiPlayableSequence>(track, instrument, 48000.0);
    Mixer mixer({ Mixer::Input { sequence } }, std::nullopt, true,
                Mixer::WarpOptions { static_cast<const AudacityProject*>(nullptr) }, 0.0, 1.0, 2, 512, false, 48000.0, floatSample);
    // The latency output is not valid before the plugin's first run(). Later
    // edits deliberately change it in both directions without replacing Mixer.
    for (const auto delay : { 7, 31, 0, 15 }) {
        SCOPED_TRACE(delay);
        ASSERT_TRUE(instrument->setParameter("lookahead", delay / 31.0));
        mixer.Reposition(0.0, true);
        ASSERT_EQ(mixer.Process(512), 512u);
        std::vector<float> expected(512);
        std::fill(expected.begin() + 100, expected.begin() + 300, 0.5f);
        const auto* actual = reinterpret_cast<const float*>(mixer.GetBuffer(0));
        EXPECT_EQ(std::vector<float>(actual, actual + 512), expected);
        EXPECT_EQ(instrument->latency(), static_cast<size_t>(delay));
    }
}

TEST(NativeLv2Tests, MixerProbesSelectionAutomationBeforeMeasuringInstrumentDelay)
{
    auto track = MidiTrack::Create();
    ASSERT_TRUE(track->SetClips({ { 1, "Phrase", 0.0, 1.0, 1,
                                    { { 1, 100.0 / 24000, 200.0 / 24000, 60, 127 },
                                        { 2, 1124.0 / 24000, 200.0 / 24000, 60, 127 } }, {} } }));
    ASSERT_TRUE(TrackAutomation::Get(*track).SetLane({ "instrument/lookahead", true,
                                                       { { 0, 23.0 / 31 }, { 512.0 / 48000, 23.0 / 31 },
                                                           { 1024.0 / 48000, 3.0 / 31 }, { 2048.0 / 48000, 3.0 / 31 } } }));
    const auto path = std::string("urn:audacity:test:delayed-instrument@")
                      + QUrl::fromLocalFile(QString::fromUtf8(AU_NATIVE_TEST_LV2_PATH)).toEncoded().toStdString();
    auto instrument = createLv2Instrument(path, {});
    auto sequence = std::make_shared<MidiPlayableSequence>(track, instrument, 48000.0);
    Mixer mixer({ Mixer::Input { sequence } }, std::nullopt, true,
                Mixer::WarpOptions { static_cast<const AudacityProject*>(nullptr) }, 0.0, 1.0, 2, 512, false, 48000.0, floatSample);
    for (const auto start : { 0, 1024, 0 }) {
        SCOPED_TRACE(start);
        mixer.Reposition(start / 48000.0, true);
        ASSERT_EQ(mixer.Process(512), 512u);
        std::vector<float> expected(512);
        std::fill(expected.begin() + 100, expected.begin() + 300, 0.5f);
        const auto* actual = reinterpret_cast<const float*>(mixer.GetBuffer(0));
        EXPECT_EQ(std::vector<float>(actual, actual + 512), expected);
        EXPECT_EQ(instrument->latency(), start == 0 ? 23u : 3u);
        // Processing probes/automation must leave the user's preset untouched.
        const auto parameters = instrument->parameters();
        const auto manual = std::find_if(parameters.begin(), parameters.end(), [](const auto& p) { return p.id == "lookahead"; });
        ASSERT_NE(manual, parameters.end());
        EXPECT_DOUBLE_EQ(manual->value, 7.0 / 31);
    }
}

namespace {
std::vector<float> renderChangingInstrumentDelay(double outputRate, size_t block, bool changing)
{
    constexpr int frames = 5120;
    auto track = MidiTrack::Create();
    std::vector<MidiNoteData> notes;
    for (int sample = 0; sample < frames; sample += 64) {
        notes.push_back({ sample / 64 + 1, sample / 24000.0, 64.0 / 24000, 60, 32 + (sample / 64 % 8) * 8 });
    }
    EXPECT_TRUE(track->SetClips({ { 1, "Delay transitions", 0.0, frames / 24000.0, 1, notes, {} } }));
    if (changing) {
        EXPECT_TRUE(TrackAutomation::Get(*track).SetLane({ "instrument/lookahead", true,
                                                           { { 0, 7.0 / 31 }, { 1023.0 / 48000, 7.0 / 31 }, { 1024.0 / 48000, 23.0 / 31 },
                                                               { 2047.0 / 48000, 23.0 / 31 }, { 2048.0 / 48000, 3.0 / 31 },
                                                               { 3071.0 / 48000, 3.0 / 31 }, { 3072.0 / 48000, 15.0 / 31 },
                                                               { 4095.0 / 48000, 15.0 / 31 }, { 4096.0 / 48000, 0 } } }));
    }
    const auto path = std::string("urn:audacity:test:delayed-instrument@")
                      + QUrl::fromLocalFile(QString::fromUtf8(AU_NATIVE_TEST_LV2_PATH)).toEncoded().toStdString();
    auto instrument = createLv2Instrument(path, {});
    if (!changing) {
        EXPECT_TRUE(instrument->setParameter("lookahead", 0));
    }
    auto sequence = std::make_shared<MidiPlayableSequence>(track, instrument, 48000);
    Mixer mixer({ Mixer::Input { sequence } }, std::nullopt, true,
                Mixer::WarpOptions { static_cast<const AudacityProject*>(nullptr) }, 0.0, frames / 48000.0,
                2, block, false, outputRate, floatSample);
    std::vector<float> result;
    while (const auto count = mixer.Process(block)) {
        const auto* samples = reinterpret_cast<const float*>(mixer.GetBuffer(0));
        result.insert(result.end(), samples, samples + count);
    }
    return result;
}

void checkChangingInstrumentDelay(double rate, size_t block)
{
    const auto actual = renderChangingInstrumentDelay(rate, block, true);
    const auto reference = renderChangingInstrumentDelay(rate, block, false);
    ASSERT_EQ(actual.size(), reference.size());
    size_t compared = 0, mismatches = 0;
    double maximum = 0;
    for (size_t frame = 0; frame < actual.size(); ++frame) {
        const auto input = frame * 48000.0 / rate;
        // A native delay reduction can omit its own queued samples. Exclude
        // only the transition and the resampler's filter support, then require
        // exact timeline alignment throughout every stable segment.
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
            ++mismatches;
        }
    }
    EXPECT_GT(compared, 2500u);
    EXPECT_EQ(mismatches, 0u) << "maximum stable-region error=" << maximum << ", output rate=" << rate;
}
}

TEST(NativeLv2Tests, ChangingInstrumentDelayStaysAlignedAcrossAutomatedBlocksInsideOneMixerRead)
{
    checkChangingInstrumentDelay(48000, 4096);
}

TEST(NativeLv2Tests, ChangingInstrumentDelayStaysAlignedThroughOriginalResamplerReadAheadAndSmallOutputBuffers)
{
    checkChangingInstrumentDelay(44100, 73);
}

TEST(NativeLv2Tests, CompensatedShortLoopsKeepBufferedNativeAudioAndExcludeNotesBeyondTheLoop)
{
    for (const bool fixed : { false, true }) {
        for (const bool live : { false, true }) {
            SCOPED_TRACE(testing::Message() << "fixed=" << fixed << ", live=" << live);
            auto track = MidiTrack::Create();
            ASSERT_TRUE(track->SetClips({ { 1, "Loop", 0, 128.0 / 24000, 1,
                                            { { 1, 16.0 / 24000, 4.0 / 24000, 60, 64 }, { 2, 20.0 / 24000, 4.0 / 24000, 64, 96 },
                                                { 3, 24.0 / 24000, 64.0 / 24000, 67, 127 } }, {} } }));
            auto processor = std::make_shared<LoopInstrumentProbe>(createLv2Instrument(fixed ? fixturePath(true)
                                                                                       : "urn:audacity:test:delayed-instrument@"
                                                                                       + QUrl::fromLocalFile(QString::fromUtf8(
                                                                                                                 AU_NATIVE_TEST_LV2_PATH)).
                                                                                       toEncoded().toStdString(), {}));
            if (!fixed) {
                ASSERT_TRUE(processor->setParameter("lookahead", 23.0 / 31));
            }
            processor->takeChanges();
            std::shared_ptr<LiveInstrumentSession> session;
            std::shared_ptr<InstrumentProcessor> source = processor;
            if (live) {
                session = std::make_shared<LiveInstrumentSession>(track, processor);
                session->beginTimeline();
                source = session;
            }
            auto sequence = std::make_shared<MidiPlayableSequence>(track, source, 48000);
            sequence->SetPlaybackLoop(true, 16.0 / 48000, 24.0 / 48000);
            if (session) {
                session->sendMidi(0x90, 80, 32);
            }
            Mixer mixer({ Mixer::Input { sequence } }, std::nullopt, true,
                        Mixer::WarpOptions { static_cast<const AudacityProject*>(nullptr) }, 16.0 / 48000, 24.0 / 48000,
                        2, 8, false, 48000, floatSample);
            size_t resets = 0;
            for (int loop = 0; loop < 32; ++loop) {
                ASSERT_EQ(mixer.Process(8), 8u) << loop;
                if (loop == 0) {
                    resets = processor->resets;
                }
                EXPECT_EQ(processor->resets, resets) << loop;
                for (int channel = 0; channel < 2; ++channel) {
                    const auto* samples = reinterpret_cast<const float*>(mixer.GetBuffer(channel));
                    for (int frame = 0; frame < 8; ++frame) {
                        const auto velocity = (frame < 4 ? 64 : 96) + (live ? 32 : 0);
                        ASSERT_NEAR(samples[frame], 0.5f * velocity / 127.0f, 1e-6) << "loop=" << loop << ", frame=" << frame;
                    }
                }
                mixer.SetTimesAndSpeed(16.0 / 48000, 24.0 / 48000, 1, true);
            }
            EXPECT_EQ(processor->ons[67], 0u); // Never render the out-of-loop clip note, even ahead of playback.
            EXPECT_EQ(processor->ons[80], live ? 1u : 0u);
            EXPECT_LE(processor->rendered, 256u + (fixed ? 511u : 23u) + (live ? 0u : 512u) + 8u);
            EXPECT_FALSE(source->takeChanges());
            if (session) {
                session->endTimeline();
                session->close();
            }
        }
    }
}

TEST(NativeLv2Tests, EditingLoopBoundsSeekingAndDisablingLoopDiscardStaleLookaheadButKeepHeldKeyboardInput)
{
    for (const bool fixed : { false, true }) {
        for (const bool live : { false, true }) {
            SCOPED_TRACE(testing::Message() << "fixed=" << fixed << ", live=" << live);
            auto track = MidiTrack::Create();
            ASSERT_TRUE(track->SetClips({ { 1, "Loop edits", 0, 128.0 / 24000, 1,
                                            { { 1, 16.0 / 24000, 4.0 / 24000, 60, 64 }, { 2, 20.0 / 24000, 4.0 / 24000, 64, 80 },
                                                { 3, 24.0 / 24000, 8.0 / 24000, 67, 127 }, { 4, 48.0 / 24000, 4.0 / 24000, 60, 96 },
                                                { 5, 52.0 / 24000, 4.0 / 24000, 64, 112 }, { 6, 56.0 / 24000, 8.0 / 24000, 67, 127 } },
                                            {} } }));
            auto processor = std::make_shared<LoopInstrumentProbe>(createLv2Instrument(fixed ? fixturePath(true)
                                                                                       : "urn:audacity:test:delayed-instrument@"
                                                                                       + QUrl::fromLocalFile(QString::fromUtf8(
                                                                                                                 AU_NATIVE_TEST_LV2_PATH)).
                                                                                       toEncoded().toStdString(), {}));
            if (!fixed) {
                ASSERT_TRUE(processor->setParameter("lookahead", 23.0 / 31));
            }
            processor->takeChanges();
            std::shared_ptr<LiveInstrumentSession> session;
            std::shared_ptr<InstrumentProcessor> source = processor;
            if (live) {
                session = std::make_shared<LiveInstrumentSession>(track, processor);
                session->beginTimeline();
                source = session;
            }
            MidiPlayableSequence sequence(track, source, 48000);
            if (session) {
                session->sendMidi(0x90, 80, 32);
            }
            std::array<float, 16> left {}, right {};
            float* output[] { left.data(), right.data() };
            const auto check = [&](int first, std::initializer_list<int> velocities) {
                EXPECT_TRUE(sequence.GetFloats(0, 2, output, first, velocities.size()));
                size_t frame = 0;
                for (const auto velocity : velocities) {
                    const auto value = 0.5f * (velocity + (live ? 32 : 0)) / 127.0f;
                    EXPECT_NEAR(left[frame], value, 1e-6) << first << ":" << frame;
                    EXPECT_NEAR(right[frame], value, 1e-6) << first << ":" << frame;
                    ++frame;
                }
            };
            sequence.SetPlaybackLoop(true, 16.0 / 48000, 24.0 / 48000);
            for (int loop = 0; loop < 4; ++loop) {
                check(16, { 64, 64, 64, 64, 80, 80, 80, 80 });
            }
            sequence.SetPlaybackLoop(true, 48.0 / 48000, 56.0 / 48000);
            for (int loop = 0; loop < 4; ++loop) {
                check(48, { 96, 96, 96, 96, 112, 112, 112, 112 });
            }
            check(50, { 96, 96, 112, 112 });
            sequence.SetPlaybackLoop(false, 48.0 / 48000, 56.0 / 48000);
            check(48, { 96, 96, 96, 96, 112, 112, 112, 112, 127, 127, 127, 127, 127, 127, 127, 127 });
            sequence.SetPlaybackLoop(true, 16.0 / 48000, 24.0 / 48000);
            check(16, { 64, 64, 64, 64, 80, 80, 80, 80 });
            EXPECT_EQ(processor->ons[80], live ? 1u : 0u);
            EXPECT_FALSE(source->takeChanges());
            if (session) {
                session->endTimeline();
                session->close();
            }
        }
    }
}

TEST(NativeLv2Tests, DelayedLoopsMatchDryNativeAudioThroughResamplingAndFastAndSlowPlayback)
{
    for (const auto [rate, speed] : { std::pair { 44100.0, 1.0 }, std::pair { 48000.0, 1.25 }, std::pair { 48000.0, 0.8 } }) {
        SCOPED_TRACE(testing::Message() << "rate=" << rate << ", speed=" << speed);
        const auto expected = static_cast<size_t>(std::llround(480 * rate / 48000 / speed));
        const auto capture = [&](bool delayed) {
            auto track = MidiTrack::Create();
            std::vector<MidiNoteData> notes;
            for (int i = 0; i < 9; ++i) {
                notes.push_back({ i + 1, (2400.0 - 32 + i * 64) / 24000, 64.0 / 24000, 60, 32 + i * 8 });
            }
            EXPECT_TRUE(track->SetClips({ { 1, "Resampled loop", 0, 6000.0 / 24000, 1, notes, {} } }));
            const auto path = std::string("urn:audacity:test:delayed-instrument@")
                              + QUrl::fromLocalFile(QString::fromUtf8(AU_NATIVE_TEST_LV2_PATH)).toEncoded().toStdString();
            auto processor = createLv2Instrument(path, {});
            EXPECT_TRUE(processor->setParameter("lookahead", delayed ? 23.0 / 31 : 0));
            auto sequence = std::make_shared<MidiPlayableSequence>(track, processor, 48000);
            sequence->SetPlaybackLoop(true, 2400.0 / 48000, 2880.0 / 48000);
            const auto warp = speed == 1 ? Mixer::WarpOptions { static_cast<const AudacityProject*>(nullptr) }
            : Mixer::WarpOptions { 0.01, 32.0, speed };
            Mixer mixer({ Mixer::Input { sequence } }, std::nullopt, true, warp,
                        2400.0 / 48000, 2880.0 / 48000, 2, 73, false, rate, floatSample);
            std::vector<std::vector<float> > result;
            for (int loop = 0; loop < 8; ++loop) {
                std::vector<float> samples;
                while (samples.size() < expected) {
                    const auto requested = std::min<size_t>(73, expected - samples.size());
                    const auto count = mixer.Process(requested);
                    EXPECT_EQ(count, requested) << "loop=" << loop << ", first=" << samples.size();
                    if (!count) {
                        break;
                    }
                    const auto* data = reinterpret_cast<const float*>(mixer.GetBuffer(0));
                    samples.insert(samples.end(), data, data + count);
                }
                result.push_back(std::move(samples));
                mixer.SetTimesAndSpeed(2400.0 / 48000, 2880.0 / 48000, speed, true);
            }
            return result;
        };
        const auto dry = capture(false), actual = capture(true);
        ASSERT_EQ(dry.size(), actual.size());
        for (size_t loop = 0; loop < dry.size(); ++loop) {
            ASSERT_EQ(dry[loop].size(), expected) << loop;
            ASSERT_EQ(actual[loop].size(), expected) << loop;
            size_t mismatches = 0;
            double maximum = 0;
            for (size_t frame = 0; frame < expected; ++frame) {
                const auto error = std::abs(dry[loop][frame] - actual[loop][frame]);
                maximum = std::max(maximum, static_cast<double>(error));
                if (error > 1e-6) {
                    ++mismatches;
                }
            }
            EXPECT_EQ(mismatches, 0u) << "loop=" << loop << ", maximum residual=" << maximum;
            EXPECT_GT(std::count_if(dry[loop].begin(), dry[loop].end(), [](float sample) { return sample > 0.01f; }), expected / 2);
        }
    }
}

TEST(NativeLv2Tests, PublishedInstrumentDelayCanBeReadWhileTheRealPluginChangesItsOutputPort)
{
    const auto path = std::string("urn:audacity:test:delayed-instrument@")
                      + QUrl::fromLocalFile(QString::fromUtf8(AU_NATIVE_TEST_LV2_PATH)).toEncoded().toStdString();
    auto instrument = createLv2Instrument(path, {});
    instrument->prepare(48000.0, 64);
    std::atomic<bool> ready { false }, start { false }, finished { false };
    std::atomic<unsigned> reads { 0 };
    std::thread reader([&] {
        ready.store(true);
        while (!start.load()) { std::this_thread::yield();
        }
        do {
            const auto delay = instrument->latency();
            EXPECT_TRUE(delay == 0 || delay == 3 || delay == 7 || delay == 31) << delay;
            ++reads;
        } while (!finished.load());
    });
    while (!ready.load()) {
        std::this_thread::yield();
    }
    start.store(true);
    std::array<float, 64> left {}, right {};
    float* output[] { left.data(), right.data() };
    const std::array delays { 7u, 3u, 0u, 31u };
    for (unsigned block = 0; block < 2000; ++block) {
        const auto delay = delays[block % delays.size()];
        EXPECT_TRUE(instrument->setAutomatedParameter("lookahead", delay / 31.0));
        instrument->process(output, 2, 64, {}, {});
        EXPECT_EQ(instrument->latency(), delay);
    }
    finished.store(true);
    reader.join();
    EXPECT_GT(reads.load(), 0u);
    EXPECT_EQ(instrument->latency(), 31u);
    EXPECT_FALSE(instrument->takeChanges());
}

TEST(NativeLv2Tests, DiscoveryKeepsAndClassifiesMidiInstruments)
{
    auto instrument = createLv2Instrument(fixturePath(), {});
    LV2EffectsModule provider;
    const auto* plugin = provider.GetPlugin(wxString::FromUTF8(fixturePath()));
    ASSERT_NE(plugin, nullptr);
    LV2EffectBase effect(*plugin);
    EXPECT_EQ(effect.GetType(), EffectTypeInstrument);
    const auto paths = provider.FindModulePaths(PluginManager::Get());
    EXPECT_NE(std::find(paths.begin(), paths.end(), wxString::FromUTF8(fixturePath())), paths.end());
}

TEST(NativeLv2Tests, FormatProbingDeclinesVstPathsWithoutUserVisibleErrors)
{
    class CaptureLog final : public wxLog
    {
    public:
        std::vector<wxString> messages;
    protected:
        void DoLogText(const wxString& text) override { messages.push_back(text); }
    } log;
    LV2EffectsModule provider;
    // These are ordinary candidates in a mixed-format scan, including Windows
    // VST3 paths and names containing '@'. They must not open wx error dialogs.
    auto* previous = wxLog::SetActiveTarget(&log);
    for (const auto* path : { "C:/Program Files/Common Files/VST3/Fasttracker II.vst3",
                              "C:/Program Files/Common Files/VST3/MeldaProduction/Modulation/MRingModulator.vst3",
                              "C:/Program Files/Common Files/VST3/Synth@Home.vst3",
                              "/Library/Audio/Plug-Ins/VST3/Analog Lab V.vst3", "appl/aumu/dls /DLSMusicDevice" }) {
        EXPECT_FALSE(provider.LoadBundle(wxString::FromUTF8(path)));
        EXPECT_FALSE(provider.CheckPluginExist(wxString::FromUTF8(path)));
    }
    log.Flush();
    wxLog::SetActiveTarget(previous);
    EXPECT_TRUE(log.messages.empty());
    auto instrument = createLv2Instrument(fixturePath(), {});
    EXPECT_TRUE(provider.LoadBundle(wxString::FromUTF8(fixturePath())));
    EXPECT_TRUE(provider.CheckPluginExist(wxString::FromUTF8(fixturePath())));
}

TEST(NativeLv2Tests, RegistrationValidatesInstrumentOwnedStatePathsBeforeOfferingTheInstrumentInTheApp)
{
    auto instrument = createLv2Instrument(fixturePath(), {});
    LV2EffectsModule provider;
    TranslatableString error;
    PluginID id = "native-instrument-state-validation";
    int callbacks = 0;
    EXPECT_EQ(provider.DiscoverPluginsAtPath(wxString::FromUTF8(fixturePath()), error,
                                             [&](PluginProvider*, ComponentInterface* component) -> const PluginID& {
        ++callbacks;
        const auto* effect = dynamic_cast<const EffectDefinitionInterface*>(component);
        EXPECT_NE(effect, nullptr);
        if (effect) {
            EXPECT_EQ(effect->GetType(), EffectTypeInstrument);
        }
        const auto validator = provider.MakeValidator();
        EXPECT_NO_THROW(validator->Validate(*component));
        return id;
    }), 1u);
    EXPECT_EQ(callbacks, 1);
}

TEST(NativeLv2Tests, InvalidArchivedResourcePathsAreRejected)
{
    EXPECT_THROW(createLv2Instrument(fixturePath(), R"({"lv2Files":[{"path":"../outside","data":""}]})"), std::runtime_error);
}

TEST(NativeLv2Tests, AudioEffectParametersUseTheOriginalEffectSettingsWithoutOpeningAnInstrument)
{
    auto instrument = createLv2Instrument(fixturePath(), {});
    const auto path = std::string("urn:audacity:test:audio-effect@")
                      + QUrl::fromLocalFile(QString::fromUtf8(AU_NATIVE_TEST_LV2_PATH)).toEncoded().toStdString();
    LV2EffectsModule provider;
    const auto* plugin = provider.GetPlugin(wxString::FromUTF8(path));
    ASSERT_NE(plugin, nullptr);
    LV2EffectBase effect(*plugin);
    auto settings = effect.MakeSettings();
    GetSettings(settings).values[0] = 0.31f;
    const auto parameters = nativeEffectParameters(effect, settings);
    ASSERT_EQ(parameters.size(), 1u);
    EXPECT_EQ(parameters[0].id, "gain");
    EXPECT_EQ(parameters[0].name, "Gain");
    EXPECT_NEAR(parameters[0].value, 0.31, 1e-6);
    EXPECT_EQ(effect.GetType(), EffectTypeProcess);
    EXPECT_THROW(createLv2Instrument(path, {}), std::runtime_error);
}

TEST(NativeLv2Tests, OriginalAudioEffectProcessesStereoWithAutomationInTheNativeChain)
{
    auto instrument = createLv2Instrument(fixturePath(), {});
    const auto path = std::string("urn:audacity:test:audio-effect@")
                      + QUrl::fromLocalFile(QString::fromUtf8(AU_NATIVE_TEST_LV2_PATH)).toEncoded().toStdString();
    LV2EffectsModule provider;
    const auto* plugin = provider.GetPlugin(wxString::FromUTF8(path));
    ASSERT_NE(plugin, nullptr);
    LV2EffectBase effect(*plugin);
    AutomationData automation;
    ASSERT_TRUE(automation.SetLane({ "gain", true, { { 0.0, 0.25, 0.0 }, { 16.0 / 48000.0, 0.75, 0.0 } } }));
    NativeEffectChain chain({ { &effect, effect.MakeSettings(), automation } }, 48000.0, 16);
    ASSERT_EQ(chain.latency(), 15u);
    std::vector<float> left(64, 1.0f), right(64, 0.5f);
    chain.process({ left.data(), right.data() }, 64, 0);
    for (size_t i = 0; i < left.size(); ++i) {
        const float expected = i < 15 ? 0.0f : i < 31 ? 0.25f : 0.75f;
        EXPECT_FLOAT_EQ(left[i], expected) << i;
        EXPECT_FLOAT_EQ(right[i], expected * 0.5f) << i;
    }
}

TEST(NativeLv2Tests, NativeEditorReopensAndSharesParametersWithTheAudioInstance)
{
    if (QGuiApplication::platformName() == "offscreen") {
        GTEST_SKIP() << "Foreign native windows require a window-system Qt platform.";
    }
    auto instrument = createLv2Instrument(fixturePath(), {});
    QWindow parent;
    parent.resize(640, 480);
    parent.show();
    auto* handle = reinterpret_cast<void*>(parent.winId());
    for (int pass = 0; pass < 3; ++pass) {
        ASSERT_TRUE(instrument->attachView(handle, 10, 10, 500, 300));
        EXPECT_EQ(instrument->editorSize().width, 360);
        EXPECT_EQ(instrument->editorSize().height, 180);
        QPointer<QSlider> slider;
        for (auto* widget : QApplication::allWidgets()) {
            if (widget->objectName() == "nativeLv2Gain") {
                slider = qobject_cast<QSlider*>(widget);
            }
        }
        ASSERT_TRUE(slider);
        slider->setValue(75 + pass);
        EXPECT_NEAR(instrument->parameters()[0].value, (75 + pass) / 100.0, 1e-6);
        EXPECT_TRUE(instrument->takeChanges());
        std::vector<float> block(512);
        float* output[] { block.data() };
        const std::array events { MidiEvent { 0, 0xc0, 0, 0 }, MidiEvent { 0, 0x90, 60, 127 } };
        instrument->reset();
        instrument->process(output, 1, 512, events, {});
        EXPECT_NEAR(block[128], (75 + pass) / 100.0f, 1e-6);
        QPointer<QPushButton> button;
        for (auto* widget : QApplication::allWidgets()) {
            if (widget->objectName() == "nativeLv2Preset") {
                button = qobject_cast<QPushButton*>(widget);
            }
        }
        ASSERT_TRUE(button);
        button->click();
        instrument->process(output, 1, 512, {}, {});
        EXPECT_NEAR(block[128], 3.0f * (75 + pass) / 100.0f, 1e-6);
        instrument->detachView();
        EXPECT_TRUE(slider.isNull());
        EXPECT_TRUE(button.isNull());
    }
}

TEST(NativeLv2Tests, NativeEditorSampleEditsSurviveDestructionOfTheOriginalInstanceAndFiles)
{
    if (QGuiApplication::platformName() == "offscreen") {
        GTEST_SKIP() << "Foreign native windows require a window-system Qt platform.";
    }
    std::string saved;
    {
        auto instrument = createLv2Instrument(fixturePath(), {});
        instrument->prepare(48000.0, 512);
        QWindow parent;
        parent.resize(640, 480);
        parent.show();
        ASSERT_TRUE(instrument->attachView(reinterpret_cast<void*>(parent.winId()), 0, 0, 500, 300));
        for (const auto* name : { "nativeLv2Preset", "nativeLv2Asset" }) {
            QPushButton* button = nullptr;
            for (auto* widget : QApplication::allWidgets()) {
                if (widget->objectName() == name) {
                    button = qobject_cast<QPushButton*>(widget);
                }
            }
            ASSERT_NE(button, nullptr);
            button->click();
        }
        std::array<float, 512> result {};
        float* output[] { result.data() };
        const std::array note { MidiEvent { 0, 0x90, 60, 127 } };
        instrument->process(output, 1, 512, note, {});
        ASSERT_FLOAT_EQ(result[128], 2.25f); // gain .5 * private 3 * asset 1.5
        saved = instrument->state();
        EXPECT_NE(saved.find("asset.bin"), std::string::npos);
        instrument->detachView();
    }
    auto restored = createLv2Instrument(fixturePath(), saved);
    restored->prepare(96000.0, 256);
    std::array<float, 256> result {};
    float* output[] { result.data() };
    const std::array note { MidiEvent { 0, 0x90, 60, 127 } };
    restored->process(output, 1, 256, note, {});
    EXPECT_FLOAT_EQ(result[128], 2.25f); // a new default sample would yield 1.5
}
}

namespace au::au3audio {
TEST(NativeLv2Tests, FixedAudioEffectBuffersPartialSlicesLatchesControlsAndResetsDelay)
{
    // Load the bundle through the same native discovery path as an instrument.
    auto bundle = createLv2Instrument(fixturePath(), {});
    const auto path = std::string("urn:audacity:test:fixed-audio-effect@")
                      + QUrl::fromLocalFile(QString::fromUtf8(AU_NATIVE_TEST_LV2_PATH)).toEncoded().toStdString();
    LV2EffectsModule provider;
    const auto* plugin = provider.GetPlugin(wxString::FromUTF8(path));
    ASSERT_NE(plugin, nullptr);
    LV2EffectBase effect(*plugin);
    auto settings = effect.MakeSettings();
    const auto instance = effect.MakeInstance();
    ASSERT_EQ(instance->SetBlockSize(100), 64u);
    ASSERT_TRUE(instance->RealtimeInitialize(settings, 48000.0, 100));
    ASSERT_TRUE(instance->RealtimeAddProcessor(settings, nullptr, 1, 48000.0));
    ASSERT_TRUE(instance->RealtimeAddProcessor(settings, nullptr, 1, 48000.0));
    ASSERT_TRUE(instance->RealtimeResume());
    EXPECT_EQ(instance->GetLatency(settings, 48000.0), 63u);
    std::vector<float> left, right;
    size_t position = 0;
    for (const auto count : { 13u, 24u, 63u, 1u, 27u, 64u, 11u, 53u, 16u }) {
        std::vector<float> input(count), opposite(count), result(count), other(count);
        for (size_t i = 0; i < count; ++i) {
            input[i] = (position + i + 1) / 1000.0f;
            opposite[i] = -input[i];
        }
        GetSettings(settings).values[0] = position < 37 ? 0.25f : 0.75f;
        if (position == 37) {
            // Two four-byte atom bodies also exercise sequence alignment.
            const LV2_Atom_Float change { { sizeof(float), LV2Symbols::urid_Float }, 0.5f };
            const auto& ports = dynamic_cast<LV2Instance&>(*instance).GetPortStates();
            ports.mAtomPortStates.front()->ReceiveFromDialog(&change, sizeof(change));
            ports.mAtomPortStates.front()->ReceiveFromDialog(&change, sizeof(change));
        }
        EffectInstance::MessagePackage package { settings, nullptr };
        ASSERT_TRUE(instance->RealtimeProcessStart(package));
        const float* in[] { input.data() };
        float* out[] { result.data() };
        const float* inOther[] { opposite.data() };
        float* outOther[] { other.data() };
        ASSERT_EQ(instance->RealtimeProcess(0, settings, in, out, count), count);
        ASSERT_EQ(instance->RealtimeProcess(1, settings, inOther, outOther, count), count);
        ASSERT_TRUE(instance->RealtimeProcessEnd(settings));
        left.insert(left.end(), result.begin(), result.end());
        right.insert(right.end(), other.begin(), other.end());
        position += count;
    }
    for (size_t i = 0; i < left.size(); ++i) {
        const auto source = static_cast<int64_t>(i) - 63;
        const float expected = source < 0 ? 0.0f : (source + 1) / 1000.0f * (source < 64 ? 0.25f : 0.75f) * (source < 37 ? 1.0f : 0.5f);
        EXPECT_FLOAT_EQ(left[i], expected) << i;
        EXPECT_FLOAT_EQ(right[i], -expected) << i;
    }
    ASSERT_TRUE(instance->RealtimeSuspend());
    ASSERT_TRUE(instance->RealtimeResume());
    std::array<float, 64> silence {}, output {};
    const float* in[] { silence.data() };
    float* out[] { output.data() };
    EffectInstance::MessagePackage package { settings, nullptr };
    ASSERT_TRUE(instance->RealtimeProcessStart(package));
    ASSERT_EQ(instance->RealtimeProcess(0, settings, in, out, 64), 64u);
    EXPECT_TRUE(std::all_of(output.begin(), output.end(), [](float value) { return value == 0.0f; }));
    ASSERT_TRUE(instance->RealtimeProcessEnd(settings));
    ASSERT_TRUE(instance->RealtimeFinalize(settings));
}
TEST(NativeLv2Tests, ChangedPluginLatencyIsPublishedAfterProcessingWithoutReadingPluginOutputAcrossThreads)
{
    auto bundle = createLv2Instrument(fixturePath(), {});
    const auto path = std::string("urn:audacity:test:fixed-audio-effect@")
                      + QUrl::fromLocalFile(QString::fromUtf8(AU_NATIVE_TEST_LV2_PATH)).toEncoded().toStdString();
    LV2EffectsModule provider;
    const auto* plugin = provider.GetPlugin(wxString::FromUTF8(path));
    ASSERT_NE(plugin, nullptr);
    LV2EffectBase effect(*plugin);
    auto settings = effect.MakeSettings();
    const auto instance = effect.MakeInstance();
    ASSERT_EQ(instance->SetBlockSize(64), 64u);
    ASSERT_TRUE(instance->RealtimeInitialize(settings, 48000.0, 64));
    ASSERT_TRUE(instance->RealtimeAddProcessor(settings, nullptr, 1, 48000.0));
    ASSERT_TRUE(instance->RealtimeResume());
    std::array<float, 64> input, output;
    input.fill(1.0f);
    const float* in[] { input.data() };
    float* out[] { output.data() };
    uint64_t revision = instance->RealtimeLatencyRevision();
    for (const size_t delay : { 0u, 7u, 7u, 2u }) {
        GetSettings(settings).values[1] = delay;
        const auto before = instance->GetLatency(settings, 48000.0);
        EffectInstance::MessagePackage package { settings, nullptr };
        ASSERT_TRUE(instance->RealtimeProcessStart(package));
        ASSERT_EQ(instance->RealtimeProcess(0, settings, in, out, input.size()), input.size());
        ASSERT_TRUE(instance->RealtimeProcessEnd(settings));
        EXPECT_EQ(instance->GetLatency(settings, 48000.0), 63u + delay);
        const auto next = instance->RealtimeLatencyRevision();
        if (before == 63u + delay) {
            EXPECT_EQ(next, revision);
        } else {
            EXPECT_GT(next, revision);
        }
        revision = next;
    }
    ASSERT_TRUE(instance->RealtimeFinalize(settings));
}

TEST(NativeLv2Tests, EffectTransportUpdatesKeepUiAtomsAndUseEachProcessorsMusicalPosition)
{
    auto bundle = createLv2Instrument(fixturePath(), {});
    const auto path = std::string("urn:audacity:test:fixed-audio-effect@")
                      + QUrl::fromLocalFile(QString::fromUtf8(AU_NATIVE_TEST_LV2_PATH)).toEncoded().toStdString();
    LV2EffectsModule provider;
    const auto* plugin = provider.GetPlugin(wxString::FromUTF8(path));
    ASSERT_NE(plugin, nullptr);
    LV2EffectBase effect(*plugin);
    auto settings = effect.MakeSettings();
    const auto instance = effect.MakeInstance();
    ASSERT_EQ(instance->SetBlockSize(64), 64u);
    ASSERT_TRUE(instance->RealtimeInitialize(settings, 48000.0, 64));
    ASSERT_TRUE(instance->RealtimeAddProcessor(settings, nullptr, 1, 48000.0));
    ASSERT_TRUE(instance->RealtimeAddProcessor(settings, nullptr, 1, 48000.0));
    ASSERT_TRUE(instance->RealtimeResume());
    const auto& port = dynamic_cast<LV2Instance&>(*instance).GetPortStates().mAtomPortStates.front();
    const LV2_Atom_Float patch { { sizeof(float), LV2Symbols::urid_Float }, 0.25f };
    port->ReceiveFromDialog(&patch, sizeof(patch));
    EffectInstance::MessagePackage package { settings, nullptr };
    ASSERT_TRUE(instance->RealtimeProcessStart(package));
    std::array<float, 64> input, output;
    input.fill(1.0f);
    const float* in[] { input.data() };
    float* out[] { output.data() };
    for (size_t group = 0; group < 2; ++group) {
        const EffectProcessingContext context { group == 0 ? -0.25 : 2.0, 48000.0, 93.0, 7, 8, 1.0, group != 0 };
        instance->SetRealtimeProcessingContext(group, context);
        ASSERT_EQ(instance->RealtimeProcess(group, settings, in, out, input.size()), input.size());
        EXPECT_FLOAT_EQ(output.back(), 0.125f); // Both fixed processors received the UI atom too.
        const auto* sequence = reinterpret_cast<const LV2_Atom_Sequence*>(port->mBuffer.get());
        const auto* event = lv2_atom_sequence_begin(&sequence->body);
        ASSERT_EQ(event->body.type, LV2Symbols::urid_Object);
        const auto* position = reinterpret_cast<const LV2_Atom_Object*>(&event->body);
        const LV2_Atom* frame = nullptr, * speed = nullptr, * tempo = nullptr, * numerator = nullptr,
                      * denominator = nullptr, * bar = nullptr, * beat = nullptr, * rate = nullptr;
        lv2_atom_object_get(position, LV2Symbols::urid_Frame, &frame, LV2Symbols::urid_Speed, &speed,
                            LV2Symbols::urid_BeatsPerMinute, &tempo, LV2Symbols::urid_BeatsPerBar, &numerator,
                            LV2Symbols::urid_BeatUnit, &denominator, LV2Symbols::urid_Bar, &bar,
                            LV2Symbols::urid_BarBeat, &beat, LV2Symbols::urid_FramesPerSecond, &rate, 0);
        ASSERT_TRUE(frame && speed && tempo && numerator && denominator && bar && beat && rate);
        EXPECT_EQ(reinterpret_cast<const LV2_Atom_Long*>(frame)->body, group == 0 ? -12000 : 96000);
        EXPECT_FLOAT_EQ(reinterpret_cast<const LV2_Atom_Float*>(speed)->body, group == 0 ? 0.0f : 1.0f);
        EXPECT_FLOAT_EQ(reinterpret_cast<const LV2_Atom_Float*>(tempo)->body, 93.0f);
        EXPECT_FLOAT_EQ(reinterpret_cast<const LV2_Atom_Float*>(numerator)->body, 7.0f);
        EXPECT_EQ(reinterpret_cast<const LV2_Atom_Int*>(denominator)->body, 8);
        EXPECT_FLOAT_EQ(reinterpret_cast<const LV2_Atom_Float*>(rate)->body, 48000.0f);
        EXPECT_EQ(reinterpret_cast<const LV2_Atom_Long*>(bar)->body, group == 0 ? -1 : 0);
        EXPECT_NEAR(reinterpret_cast<const LV2_Atom_Float*>(beat)->body, group == 0 ? 6.225 : 6.2, 1e-6);
        event = lv2_atom_sequence_next(event);
        ASSERT_FALSE(lv2_atom_sequence_is_end(&sequence->body, sequence->atom.size, event));
        EXPECT_EQ(event->body.type, LV2Symbols::urid_Float);
    }
    ASSERT_TRUE(instance->RealtimeProcessEnd(settings));
    ASSERT_TRUE(instance->RealtimeFinalize(settings));
}

TEST(NativeLv2Tests, FixedAudioEffectsKeepTheirBlockContractForFreezeAndDestructiveFinalBlocks)
{
    auto bundle = createLv2Instrument(fixturePath(), {});
    const auto path = std::string("urn:audacity:test:fixed-audio-effect@")
                      + QUrl::fromLocalFile(QString::fromUtf8(AU_NATIVE_TEST_LV2_PATH)).toEncoded().toStdString();
    LV2EffectsModule provider;
    const auto* plugin = provider.GetPlugin(wxString::FromUTF8(path));
    ASSERT_NE(plugin, nullptr);
    LV2EffectBase effect(*plugin);
    AutomationData automation;
    ASSERT_TRUE(automation.SetLane({ "gain", true, { { 0.0, 0.25, 0.0 }, { 16.0 / 48000.0, 0.75, 0.0 } } }));
    NativeEffectChain chain({ { &effect, effect.MakeSettings(), automation } }, 48000.0, 16);
    ASSERT_EQ(chain.latency(), 30u); // Chain input adapter + fixed LV2 adapter.
    std::vector<float> left(91, 1.0f), right(91, 0.5f);
    chain.process({ left.data(), right.data() }, left.size(), 0);
    for (size_t i = 0; i < left.size(); ++i) {
        const auto expected = i < 30 ? 0.0f : i < 46 ? 0.25f : 0.75f;
        EXPECT_FLOAT_EQ(left[i], expected) << i;
        EXPECT_FLOAT_EQ(right[i], 0.5f * expected) << i;
    }
    auto settings = effect.MakeSettings();
    GetSettings(settings).values.front() = 0.3f;
    const auto instance = effect.MakeInstance();
    ASSERT_TRUE(instance->ProcessInitialize(settings, 48000.0, nullptr));
    const auto size = instance->GetBlockSize();
    ASSERT_GT(size, 37u);
    std::vector<float> input(size, 0.6f), output(size, -100.0f);
    const float* in[] { input.data() };
    float* out[] { output.data() };
    ASSERT_EQ(instance->ProcessBlock(settings, in, out, size), size);
    EXPECT_FLOAT_EQ(output.back(), 0.18f);
    std::fill(output.begin(), output.end(), -100.0f);
    ASSERT_EQ(instance->ProcessBlock(settings, in, out, 37), 37u);
    for (size_t i = 0; i < 37; ++i) {
        EXPECT_FLOAT_EQ(output[i], 0.18f) << i;
    }
    EXPECT_FLOAT_EQ(output[37], -100.0f);
    ASSERT_TRUE(instance->ProcessFinalize());
}
}
