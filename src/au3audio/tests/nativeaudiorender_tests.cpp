/*
* Audacity: A Digital Audio Editor
*/
#include <gtest/gtest.h>
#include <QTemporaryDir>
#include <QUrl>
#include <QCoreApplication>
#include <QLibrary>
#include <bit>
#include <cmath>
#include <future>
#include <thread>
#include <portaudio.h>
#include "async/processevents.h"

#include "../internal/nativeaudiorendersource.h"
#include "../internal/midiplayablesequence.h"
#include "../internal/nativeeffectparameters.h"
#include "../internal/nativeplaybackprocessor.h"
#include "../internal/nativetimesignature.h"
#include "../internal/nativeinstrumentsessions.h"
#include "../internal/liveinstrumentsession.h"
#include "../internal/nativemonitorsource.h"
#include "../internal/au3audioengine.h"
#include "record/tests/mocks/recordconfigurationmock.h"
#include "../internal/defaultplaybackpolicy.h"
#include "au3wrap/internal/au3project.h"
#include "project/tests/testtools.h"
#include "au3-builtin-effects/BassTrebleBase.h"
#include "au3-audio-devices/AudioIOBase.h"
#include "au3-midi-track/MidiTrack.h"
#include "au3-mixer/Mix.h"
#include "au3-mixer/Envelope.h"
#include "au3-playable-track/TrackAutomation.h"
#include "au3-playable-track/TrackRouting.h"
#include "au3-project/Project.h"
#include "au3-numeric-formats/ProjectTimeSignature.h"
#include "au3-time-frequency-selection/ViewInfo.h"
#include "au3-realtime-effects/RealtimeEffectList.h"
#include "au3-realtime-effects/RealtimeEffectState.h"
#include "au3-realtime-effects/RealtimeEffectManager.h"
#include "au3-stretching-sequence/StretchingSequence.h"
#include "au3-wave-track/WaveTrack.h"
#include "au3-wave-track/WaveClip.h"
#include "mod-pcm/ExportPCM.h"
#ifdef AU_NATIVE_TEST_INSTRUMENT_PATH
#include "au3-vst3/VST3EffectBase.h"
#include "au3-vst3/VST3EffectsModule.h"
#endif
#ifdef AU_NATIVE_TEST_CODEC_FLAC
#include "mod-flac/ExportFLAC.h"
#endif
#ifdef AU_NATIVE_TEST_CODEC_OGG
#include "mod-ogg/ExportOGG.h"
#endif
#ifdef AU_NATIVE_TEST_CODEC_OPUS
#include "mod-opus/ExportOpus.h"
#endif
#ifdef AU_NATIVE_TEST_CODEC_MP3
#include "mod-mp3/ExportMP3.h"
#endif
#ifdef AU_NATIVE_TEST_CODEC_MP2
#include "mod-mp2/ExportMP2.h"
#endif
#if defined(AU_NATIVE_TEST_CODEC_MP3) || defined(AU_NATIVE_TEST_CODEC_MP2)
#include <mpg123.h>
#endif
#ifdef AU_NATIVE_TEST_CODEC_WAVPACK
#include "mod-wavpack/ExportWavPack.h"
#include <wavpack/wavpack.h>
#endif
#include <sndfile.h>
#ifdef AU_NATIVE_TEST_LV2_PATH
#include "au3-lv2/LoadLV2.h"
#endif

namespace au::au3audio {
namespace {
class ToneControls final : public BassTrebleBase
{
public:
    std::shared_ptr<EffectInstance> MakeInstance() const override { return std::make_shared<Instance>(*this); }
};
class DelayedToneControls final : public BassTrebleBase
{
public:
    class DelayedInstance final : public BassTrebleBase::Instance
    {
    public:
        explicit DelayedInstance(const PerTrackEffect& effect)
            : BassTrebleBase::Instance(effect) {}
        bool RealtimeInitialize(EffectSettings& settings, double rate, size_t capacity) override
        {
            delayed.clear();
            positions.clear();
            return BassTrebleBase::Instance::RealtimeInitialize(settings, rate, capacity);
        }

        bool RealtimeAddProcessor(EffectSettings& settings, EffectOutputs* outputs, unsigned channels, float rate) override
        {
            delayed.push_back({});
            positions.push_back(0);
            return BassTrebleBase::Instance::RealtimeAddProcessor(settings, outputs, channels, rate);
        }

        SampleCount GetLatency(const EffectSettings&, double) const override { return 3; }
        size_t RealtimeProcess(size_t group, EffectSettings& settings, const float* const* input, float* const* output,
                               size_t count) override
        {
            const auto result = BassTrebleBase::Instance::RealtimeProcess(group, settings, input, output, count);
            for (size_t i = 0; i < result; ++i) {
                const auto value = output[0][i];
                output[0][i] = delayed[group][positions[group]];
                delayed[group][positions[group]] = value;
                positions[group] = (positions[group] + 1) % 3;
            }
            return result;
        }

        std::vector<std::array<float, 3> > delayed;
        std::vector<size_t> positions;
    };
    std::shared_ptr<EffectInstance> MakeInstance() const override { return std::make_shared<DelayedInstance>(*this); }
};

class VariableDelayToneControls final : public BassTrebleBase
{
public:
    mutable std::atomic<size_t> delay { 3 };
    mutable std::atomic<uint64_t> revision { 0 };
    mutable std::atomic<size_t> requestedDelay { 3 };
    mutable std::atomic<bool> pending { false };
    mutable int calls = 0;
    void setDelay(size_t value) const { delay = value; ++revision; }
    void requestDelay(size_t value) const { requestedDelay = value; pending = true; }
    class VariableInstance final : public BassTrebleBase::Instance
    {
    public:
        explicit VariableInstance(const VariableDelayToneControls& effect)
            : BassTrebleBase::Instance(effect), effect(effect) {}
        bool RealtimeAddProcessor(EffectSettings& settings, EffectOutputs* output, unsigned channels, float rate) override
        {
            delayed.push_back({});
            positions.push_back(0);
            return BassTrebleBase::Instance::RealtimeAddProcessor(settings, output, channels, rate);
        }

        bool RealtimeSuspend() override
        {
            for (auto& channel : delayed) {
                channel.fill(0.0f);
            }
            std::fill(positions.begin(), positions.end(), 0);
            return BassTrebleBase::Instance::RealtimeSuspend();
        }

        SampleCount GetLatency(const EffectSettings&, double) const override { return effect.delay.load(); }
        uint64_t RealtimeLatencyRevision() const noexcept override { return effect.revision.load(); }
        bool RealtimeLatencyUpdatePending() const noexcept override { return effect.pending.load(); }
        bool RefreshRealtimeLatency() override
        {
            if (!effect.pending.exchange(false) || effect.requestedDelay.load() == effect.delay.load()) {
                return false;
            }
            effect.setDelay(effect.requestedDelay.load());
            return true;
        }

        size_t RealtimeProcess(size_t group, EffectSettings& settings, const float* const* input, float* const* output,
                               size_t count) override
        {
            ++effect.calls;
            const auto result = BassTrebleBase::Instance::RealtimeProcess(group, settings, input, output, count);
            auto& data = delayed[group];
            auto& head = positions[group];
            const auto delay = effect.delay.load();
            for (size_t i = 0; i < result; ++i) {
                data[head] = output[0][i];
                output[0][i] = data[(head + data.size() - delay) % data.size()];
                head = (head + 1) % data.size();
            }
            return result;
        }

        const VariableDelayToneControls& effect;
        std::vector<std::array<float, 64> > delayed;
        std::vector<size_t> positions;
    };
    std::shared_ptr<EffectInstance> MakeInstance() const override { return std::make_shared<VariableInstance>(*this); }
};

class TransportToneControls final : public BassTrebleBase
{
public:
    struct Record {
        size_t processor;
        EffectProcessingContext context;
    };
    mutable std::vector<Record> records;
    class TransportInstance final : public BassTrebleBase::Instance
    {
    public:
        explicit TransportInstance(const TransportToneControls& effect)
            : BassTrebleBase::Instance(effect), effect(effect) {}
        bool RealtimeAddProcessor(EffectSettings& settings, EffectOutputs* output, unsigned channels, float rate) override
        {
            contexts.push_back(std::nullopt);
            return BassTrebleBase::Instance::RealtimeAddProcessor(settings, output, channels, rate);
        }

        void SetRealtimeProcessingContext(size_t group, const EffectProcessingContext& context) override { contexts.at(group) = context; }
        size_t RealtimeProcess(size_t group, EffectSettings& settings, const float* const* input, float* const* output,
                               size_t count) override
        {
            if (contexts.at(group)) {
                effect.records.push_back({ group, *contexts[group] });
            }
            return BassTrebleBase::Instance::RealtimeProcess(group, settings, input, output, count);
        }

        const TransportToneControls& effect;
        std::vector<std::optional<EffectProcessingContext> > contexts;
    };
    std::shared_ptr<EffectInstance> MakeInstance() const override { return std::make_shared<TransportInstance>(*this); }
};

class ShortToneControls final : public BassTrebleBase
{
public:
    class ShortInstance final : public BassTrebleBase::Instance
    {
    public:
        explicit ShortInstance(const PerTrackEffect& effect)
            : BassTrebleBase::Instance(effect) {}
        size_t RealtimeProcess(size_t processor, EffectSettings&, const float* const* input, float* const* output, size_t count) override
        {
            const auto produced = processor == 0 ? count : count / 2;
            std::fill_n(output[0], count, 100.0f);
            std::copy_n(input[0], produced, output[0]);
            return produced;
        }
    };
    std::shared_ptr<EffectInstance> MakeInstance() const override { return std::make_shared<ShortInstance>(*this); }
};

class RefusingToneControls final : public BassTrebleBase
{
public:
    class RefusingInstance final : public BassTrebleBase::Instance
    {
    public:
        explicit RefusingInstance(const PerTrackEffect& effect)
            : BassTrebleBase::Instance(effect) {}
        bool RealtimeInitialize(EffectSettings&, double, size_t) override { return false; }
        bool RealtimeProcessStart(MessagePackage&) override { ++starts; return false; }
        size_t RealtimeProcess(size_t, EffectSettings&, const float* const*, float* const*, size_t) override { ++processes; return 0; }
        bool RealtimeProcessEnd(EffectSettings&) noexcept override { ++ends; return false; }
        int starts = 0, processes = 0, ends = 0;
    };
    std::shared_ptr<EffectInstance> MakeInstance() const override { return std::make_shared<RefusingInstance>(*this); }
};

class MonitorCallback final : public AudioIoCallback
{
public:
    MonitorCallback() { mNumPlaybackChannels = 2; SetMixerOutputVol(1.0f); }
    void StopStream() override { StopLiveOutput(); }
    void StopMonitoring() override { StopLiveOutput(); }
    void render(float* output, size_t frames) { MixLiveOutput(output, output, frames); }
};

class MonitorCapture final : public IMeterSender
{
public:
    struct Entry {
        uint8_t channel;
        int64_t track;
        std::vector<float> samples;
        TimePoint time;
    };
    void push(uint8_t channel, const InterleavedSampleData& data, const std::optional<TrackId>& track) override
    {
        entries.push_back({ channel, track ? track->value : -1, {}, data.dacTime });
        auto& samples = entries.back().samples;
        for (size_t frame = 0; frame < data.frames; ++frame) {
            samples.push_back(data.buffer[frame * data.nChannels]);
        }
    }

    void start(double) override {}
    void stop() override {}
    std::vector<Entry> entries;
};

class MonitorProbeInstrument final : public InstrumentProcessor
{
public:
    bool latencyUpdatePending() const noexcept override { return pending; }
    bool refreshLatency() override
    {
        ++refreshes;
        if (failRefresh) {
            throw std::runtime_error("Monitoring fixture refresh failed.");
        }
        pending = unstable;
        return true;
    }

    std::vector<audio::InstrumentParameter> parameters() const override { return { { "gain", "Gain", {}, manual } }; }
    bool setParameter(const std::string&, double value) override { manual = gain = value; return true; }
    bool setAutomatedParameter(const std::string&, double value) override { gain = value; return true; }
    void restoreManualParameters() noexcept override { gain = manual; }
    bool restoreManualParameter(const std::string& id) noexcept override
    {
        if (id != "gain") {
            return false;
        }
        gain = manual;
        return true;
    }

    std::string state() const override { return std::to_string(manual); }
    void prepare(double, size_t) override { ++prepares; reset(); }
    void reset() override { ++resets; held = false; }
    void process(float* const* output, size_t channels, size_t frames, std::span<const MidiEvent> events,
                 const InstrumentProcessContext& context) override
    {
        if (failProcess) {
            throw std::runtime_error("Monitoring fixture processing failed.");
        }
        contexts.push_back(context);
        for (const auto& event : events) {
            if ((event.status & 0xf0) == 0x90 && event.data2) {
                held = true;
            }
            if ((event.status & 0xf0) == 0x80 || ((event.status & 0xf0) == 0x90 && !event.data2)) {
                held = false;
            }
        }
        for (size_t ch = 0; ch < channels; ++ch) {
            std::fill_n(output[ch], frames, held ? gain : 0.0f);
        }
    }

    double manual = 0.8, gain = 0.8;
    int prepares = 0, resets = 0;
    bool held = false;
    bool pending = false, unstable = false, failRefresh = false, failProcess = false;
    unsigned refreshes = 0;
    std::vector<InstrumentProcessContext> contexts;
};

#ifdef AU_NATIVE_TEST_INSTRUMENT_PATH
std::shared_ptr<LiveInstrumentSession> nativeMonitorInstrument(const std::shared_ptr<MidiTrack>& track)
{
    return std::make_shared<LiveInstrumentSession>(track,
                                                   createVstInstrument(std::string(AU_NATIVE_TEST_INSTRUMENT_PATH)
                                                                       + ";F5A81297A6724B218E434FA1A4050C01", {}));
}

#endif

class LinearProcessingTime final : public RealtimeEffects::ProcessingTime
{
public:
    explicit LinearProcessingTime(int64_t sample = 0, double rate = 48000.0)
        : sample(sample), rate(rate) {}
    double TimeAt(int64_t offset) const override { return (sample + offset) / rate; }
    int64_t sample;
    double rate;
};
}

class NativeAudioRenderTests : public testing::Test
{
protected:
    void SetUp() override
    {
        ASSERT_TRUE(directory.isValid());
        path = directory.filePath("render.aup4").toStdString();
        const auto fixture = std::string(au3audio_tests_DATA_ROOT) + "/../../trackedit/tests/data/empty.aup4";
        ASSERT_TRUE(testtools::copyFile(fixture, path.toStdString()));
        owner = std::make_shared<au3::Au3ProjectAccessor>(muse::modularity::globalCtx());
        ASSERT_TRUE(owner->load(path, false));
        registerNativeAudioRenderer();
        options.sampleRate = 48000.0;
        options.bufferSize = 73;
        options.stopTime = 512.0 / 48000.0;
    }

    void TearDown() override
    {
        owner->clearSavedState();
        owner->close();
        owner.reset();
    }

    AudacityProject& project() { return *reinterpret_cast<AudacityProject*>(owner->au3ProjectPtr()); }
    std::shared_ptr<WaveTrack> wave(float left, float right = 0.0f, size_t channels = 1)
    {
        auto track = WaveTrackFactory::Get(project()).Create(channels, floatSample, 48000.0);
        auto clip = track->CreateClip(0.0, "Audio");
        std::vector<float> l(512, left), r(512, right);
        constSamplePtr buffers[] { reinterpret_cast<constSamplePtr>(l.data()), reinterpret_cast<constSamplePtr>(r.data()) };
        clip->Append(buffers, floatSample, l.size(), 1, floatSample);
        clip->Flush();
        track->InsertInterval(clip, true);
        TrackList::Get(project()).Add(track);
        return track;
    }

    std::shared_ptr<WaveTrack> bus(const std::string& id)
    {
        auto track = WaveTrackFactory::Get(project()).Create(2, floatSample, 48000.0);
        TrackRouting::Get(*track).busId = id;
        TrackList::Get(project()).Add(track);
        return track;
    }

    std::shared_ptr<RealtimeEffectState> insert(RealtimeEffectList& list, double gain)
    {
        auto state = std::make_shared<RealtimeEffectState>("native-test-tone-controls");
        state->GetAccess()->ModifySettings([gain](EffectSettings& settings) {
            settings.cast<BassTrebleSettings>()->mGain = gain;
            return nullptr;
        });
        EXPECT_TRUE(list.AddState(state));
        return state;
    }

    std::array<std::vector<float>, 2> render(const AudioRenderSource::Tracks& tracks)
    {
        auto source = AudioRenderSource::Create(project(), tracks, options);
        std::array<std::vector<float>, 2> result;
        while (const auto count = source->Process()) {
            for (size_t channel = 0; channel < 2; ++channel) {
                const auto* data = reinterpret_cast<const float*>(source->GetBuffer(channel));
                result[channel].insert(result[channel].end(), data, data + count);
            }
        }
        EXPECT_NEAR(source->MixGetCurrentTime(), options.stopTime, 1.0 / options.sampleRate);
        return result;
    }

    void decodeCodecFile(const std::string& path, const wxString& format, SF_INFO& info, std::vector<float>& samples)
    {
#if defined(AU_NATIVE_TEST_CODEC_MP3) || defined(AU_NATIVE_TEST_CODEC_MP2)
        if (format == "MP3" || format == "MP2") {
            // The bundled libsndfile has no MPEG decoder. Use the same mpg123
            // library and gapless/float flags as Audacity's native importer.
            static const auto initialized = mpg123_init();
            ASSERT_EQ(initialized, MPG123_OK);
            int error = 0;
            auto* decoder = mpg123_new(nullptr, &error);
            ASSERT_NE(decoder, nullptr) << mpg123_plain_strerror(error);
            const auto close = finally([&] { mpg123_delete(decoder); });
            ASSERT_EQ(mpg123_param(decoder, MPG123_FLAGS, MPG123_GAPLESS | MPG123_FORCE_FLOAT, 0), MPG123_OK);
            ASSERT_EQ(mpg123_open(decoder, path.c_str()), MPG123_OK);
            long rate = 0;
            int channels = 0, encoding = 0;
            ASSERT_EQ(mpg123_getformat(decoder, &rate, &channels, &encoding), MPG123_OK);
            ASSERT_EQ(encoding, MPG123_ENC_FLOAT_32);
            info.samplerate = static_cast<int>(rate);
            info.channels = channels;
            info.format = SF_FORMAT_MPEG | (format == "MP3" ? SF_FORMAT_MPEG_LAYER_III : SF_FORMAT_MPEG_LAYER_II);
            std::vector<float> block(mpg123_outblock(decoder) / sizeof(float));
            for (;;) {
                size_t bytes = 0;
                const auto result = mpg123_read(decoder, reinterpret_cast<unsigned char*>(block.data()),
                                                block.size() * sizeof(float), &bytes);
                ASSERT_EQ(bytes % sizeof(float), 0u);
                samples.insert(samples.end(), block.begin(), block.begin() + bytes / sizeof(float));
                if (result == MPG123_DONE) {
                    break;
                }
                ASSERT_EQ(result, MPG123_OK) << mpg123_strerror(decoder);
            }
            ASSERT_GT(channels, 0);
            info.frames = static_cast<sf_count_t>(samples.size() / channels);
            return;
        }
#endif
#ifdef AU_NATIVE_TEST_CODEC_WAVPACK
        if (format == "WavPack") {
            char error[80] {};
            auto* decoder = WavpackOpenFileInput(path.c_str(), error, OPEN_WVC | OPEN_NORMALIZE, 0);
            ASSERT_NE(decoder, nullptr) << error;
            const auto close = finally([&] { WavpackCloseFile(decoder); });
            ASSERT_TRUE(WavpackGetMode(decoder) & MODE_LOSSLESS);
            info.frames = WavpackGetNumSamples64(decoder);
            info.channels = WavpackGetNumChannels(decoder);
            info.samplerate = WavpackGetSampleRate(decoder);
            const auto bits = WavpackGetBitsPerSample(decoder);
            const bool floating = WavpackGetMode(decoder) & MODE_FLOAT;
            // SF_INFO carries normalized sample metadata for the common
            // comparison below; libsndfile does not decode this container.
            info.format = floating ? SF_FORMAT_FLOAT : bits == 16 ? SF_FORMAT_PCM_16 : SF_FORMAT_PCM_24;
            ASSERT_GT(info.frames, 0);
            ASSERT_EQ(info.channels, 2);
            std::vector<int32_t> block(4096 * info.channels);
            for (;;) {
                const auto count = WavpackUnpackSamples(decoder, block.data(), 4096);
                if (!count) {
                    break;
                }
                for (size_t i = 0; i < static_cast<size_t>(count) * info.channels; ++i) {
                    samples.push_back(floating ? std::bit_cast<float>(block[i]) : static_cast<float>(block[i] / std::exp2(bits - 1)));
                }
            }
            EXPECT_EQ(WavpackGetNumErrors(decoder), 0);
            ASSERT_EQ(samples.size(), static_cast<size_t>(info.frames) * info.channels);
            return;
        }
#endif
        auto* file = sf_open(path.c_str(), SFM_READ, &info);
        ASSERT_NE(file, nullptr) << sf_strerror(nullptr);
        const auto close = finally([&] { sf_close(file); });
        samples.resize(static_cast<size_t>(info.frames) * info.channels);
        ASSERT_EQ(sf_readf_float(file, samples.data(), info.frames), info.frames);
    }

    void checkRoutedCodecExport(ExportPlugin& plugin, const wxString& format, bool lossy,
                                ExportProcessor::Parameters parameters = {})
    try
    {
        constexpr size_t frames = 12000;
        options.stopTime = frames / options.sampleRate;
        options.masterEffects = true;
        auto midi = MidiTrack::Create();
        ASSERT_TRUE(midi->SetClips({ { 1, "Encoded instrument", 0, 0.5, 1, { { 1, 0, 0.5, 69, 100 } }, {} } }));
        TrackList::Get(project()).Add(midi);
        options.midiTracks = { midi };
        auto audio = WaveTrackFactory::Get(project()).Create(2, floatSample, options.sampleRate);
        auto clip = audio->CreateClip(0, "Encoded stereo audio");
        std::array<std::vector<float>, 2> signal { std::vector<float>(frames), std::vector<float>(frames) };
        for (size_t frame = 0; frame < frames; ++frame) {
            signal[0][frame] = 0.12f * std::sin(2 * std::acos(-1.0) * 220 * frame / options.sampleRate);
            signal[1][frame] = 0.08f * std::sin(2 * std::acos(-1.0) * 330 * frame / options.sampleRate);
        }
        constSamplePtr buffers[] { reinterpret_cast<constSamplePtr>(signal[0].data()), reinterpret_cast<constSamplePtr>(signal[1].data()) };
        clip->Append(buffers, floatSample, frames, 1, floatSample);
        clip->Flush();
        audio->InsertInterval(clip, true);
        TrackList::Get(project()).Add(audio);
        const auto main = bus("codec-main"), side = bus("codec-side");
        for (auto* track : { static_cast<ChannelGroup*>(midi.get()), static_cast<ChannelGroup*>(audio.get()) }) {
            TrackRouting::Get(*track).output = "codec-main";
            TrackRouting::Get(*track).sends = { { "codec-side", 0.25, true } };
            ASSERT_TRUE(TrackAutomation::Get(*track).SetLane({ "volume", true,
                                                               { { 0, 0.3 / TrackAutomation::MaximumVolume },
                                                                   { options.stopTime, 0.8 / TrackAutomation::MaximumVolume } } }));
        }
        side->SetVolume(0.4);
        ASSERT_TRUE(TrackAutomation::Get(*main).SetLane({ "pan", true, { { 0, 0.2 }, { options.stopTime, 0.8 } } }));
        ToneControls effect;
        const RealtimeEffectState::EffectFactory::Scope factory([&](const PluginID&) -> const EffectInstanceFactory* { return &effect; });
        insert(RealtimeEffectList::Get(*main), -6.0);
        insert(RealtimeEffectList::Get(project()), -3.0);
        int index = -1;
        // "Other uncompressed files" may advertise the same name as a
        // dedicated PCM format; use the dedicated entry and its options.
        for (int i = 0; i < plugin.GetFormatCount(); ++i) {
            if (plugin.GetFormatInfo(i).format == format) {
                index = i;
                break;
            }
        }
        ASSERT_GE(index, 0);
        if (parameters.empty()) {
            auto editor = plugin.CreateOptionsEditor(index, nullptr);
            ASSERT_NE(editor, nullptr);
            for (int i = 0; i < editor->GetOptionsCount(); ++i) {
                ExportOption option;
                ASSERT_TRUE(editor->GetOption(i, option));
                ExportValue value;
                ASSERT_TRUE(editor->GetValue(option.id, value));
                parameters.emplace_back(option.id, std::move(value));
            }
        }
        const auto extension = plugin.GetFormatInfo(index).extensions.front();
        for (const auto first : { 0.0, 0.05 }) {
            SCOPED_TRACE(testing::Message() << "format=" << format.ToStdString() << ", first=" << first);
            options.startTime = first;
            const auto expected = render({ audio });
            const auto destination = directory.filePath(QString::fromStdString(format.ToStdString())
                                                        + QString::number(first) + "."
                                                        + QString::fromStdString(extension.ToStdString())).toStdString();
            auto processor = plugin.CreateProcessor(index);
            ASSERT_TRUE(processor->Initialize(project(), parameters, wxString::FromUTF8(destination), first,
                                              options.stopTime, false, options.sampleRate, 2));
            struct Delegate final : ExportProcessorDelegate {
                bool IsCancelled() const override { return false; }
                bool IsStopped() const override { return false; }
                void SetStatusString(const TranslatableString&) override {}
                void OnProgress(double value) override { progress = value; }
                double progress = 0;
            } delegate;
            auto task = std::async(std::launch::async, [&] { return processor->Process(delegate); });
            EXPECT_EQ(task.get(), ExportResult::Success);
            processor.reset();
            SF_INFO info {};
            std::vector<float> decoded;
            ASSERT_NO_FATAL_FAILURE(decodeCodecFile(destination, format, info, decoded));
            ASSERT_EQ(info.channels, 2);
            EXPECT_EQ(info.samplerate, options.sampleRate);
            if (format == "MP2") {
                // Raw MPEG Layer II has no gapless length metadata. Its
                // encoder/decoder may retain up to two 1152-sample frames.
                ASSERT_GE(info.frames, expected[0].size());
                ASSERT_LT(info.frames, expected[0].size() + 2 * 1152);
            } else {
                ASSERT_EQ(info.frames, expected[0].size());
            }
            std::vector<float> encodedReference;
            if (lossy) {
                // Encode the already mixed float PCM as an independent Wave
                // project. Codec filtering/priming must be identical whether
                // its input came from native MIDI/routing or ordinary audio.
                const auto referencePath = directory.filePath(QString("codec-reference-%1.aup4").arg(first)).toStdString();
                const auto fixture = std::string(au3audio_tests_DATA_ROOT) + "/../../trackedit/tests/data/empty.aup4";
                ASSERT_TRUE(testtools::copyFile(fixture, referencePath));
                auto referenceOwner = std::make_shared<au3::Au3ProjectAccessor>(muse::modularity::globalCtx());
                const auto closeReference = finally([&] { referenceOwner->clearSavedState(); referenceOwner->close(); });
                ASSERT_TRUE(referenceOwner->load(referencePath, false));
                auto& referenceProject = *reinterpret_cast<AudacityProject*>(referenceOwner->au3ProjectPtr());
                auto referenceTrack = WaveTrackFactory::Get(referenceProject).Create(2, floatSample, options.sampleRate);
                auto referenceClip = referenceTrack->CreateClip(0, "Mixed codec reference");
                constSamplePtr referenceBuffers[] { reinterpret_cast<constSamplePtr>(expected[0].data()),
                                                    reinterpret_cast<constSamplePtr>(expected[1].data()) };
                referenceClip->Append(referenceBuffers, floatSample, expected[0].size(), 1, floatSample);
                referenceClip->Flush();
                referenceTrack->InsertInterval(referenceClip, true);
                TrackList::Get(referenceProject).Add(referenceTrack);
                auto referenceProcessor = plugin.CreateProcessor(index);
                const auto referenceFile = destination + ".reference." + extension.ToStdString();
                ASSERT_TRUE(referenceProcessor->Initialize(referenceProject, parameters, wxString::FromUTF8(referenceFile), 0,
                                                           expected[0].size() / options.sampleRate, false, options.sampleRate, 2));
                auto referenceTask = std::async(std::launch::async, [&] { return referenceProcessor->Process(delegate); });
                ASSERT_EQ(referenceTask.get(), ExportResult::Success);
                referenceProcessor.reset();
                SF_INFO referenceInfo {};
                ASSERT_NO_FATAL_FAILURE(decodeCodecFile(referenceFile, format, referenceInfo, encodedReference));
                ASSERT_EQ(referenceInfo.frames, info.frames);
                ASSERT_EQ(referenceInfo.channels, info.channels);
                ASSERT_EQ(referenceInfo.samplerate, info.samplerate);
            }
            for (size_t channel = 0; channel < 2; ++channel) {
                double error = 0, reference = 0, maximum = 0;
                for (size_t frame = 0; frame < static_cast<size_t>(info.frames); ++frame) {
                    const auto referenceValue = lossy ? encodedReference[frame * 2 + channel] : expected[channel][frame];
                    const auto difference = decoded[frame * 2 + channel] - referenceValue;
                    error += difference * difference;
                    reference += referenceValue * referenceValue;
                    maximum = std::max(maximum, static_cast<double>(std::abs(difference)));
                }
                ASSERT_GT(reference, 0.01);
                if (lossy) {
                    EXPECT_LT(std::sqrt(error / reference), 0.01) << channel;
                } else {
                    // The default shaped dither has a five-tap FIR and up to
                    // +/-1.5 LSB error history. Its bounded peak can approach
                    // 15 LSB; FLAC itself must preserve those quantized samples.
                    const auto bits = (info.format & SF_FORMAT_SUBMASK) == SF_FORMAT_PCM_16 ? 16 : 24;
                    const auto tolerance = (info.format & SF_FORMAT_SUBMASK) == SF_FORMAT_FLOAT ? 1e-6 : 15.0 / std::exp2(bits - 1);
                    EXPECT_LT(maximum, tolerance) << channel;
                }
            }
            EXPECT_GE(delegate.progress, 0.99);
        }
    }
    catch (const ExportException& exception) {
        FAIL() << exception.What().ToStdString();
    } catch (const ExportErrorException& exception) {
        FAIL() << exception.GetMessage().Translation().ToStdString();
    }

    QTemporaryDir directory;
    muse::io::path_t path;
    std::shared_ptr<au3::Au3ProjectAccessor> owner;
    AudioRenderSource::Options options;
};

TEST_F(NativeAudioRenderTests, WaveRenderingMatchesTheOriginalAudacityMixer)
{
    const auto track = wave(0.25f, 0.5f, 2);
    track->SetVolume(0.7);
    track->SetPan(0.25);
    const auto actual = render({ track });
    Mixer reference({ { StretchingSequence::Create(*track, track->GetClipInterfaces()) } }, std::nullopt, true,
                    Mixer::WarpOptions { &project() }, options.startTime, options.stopTime, 2, 512, false, 48000.0, floatSample);
    size_t offset = 0;
    while (const auto count = reference.Process()) {
        for (size_t channel = 0; channel < 2; ++channel) {
            const auto* data = reinterpret_cast<const float*>(reference.GetBuffer(channel));
            for (size_t i = 0; i < count; ++i) {
                EXPECT_NEAR(actual[channel][offset + i], data[i], 1e-7);
            }
        }
        offset += count;
    }
    EXPECT_EQ(offset, actual[0].size());
}

TEST_F(NativeAudioRenderTests, OriginalWaveMixerKeepsProjectClockAndFullRangeAtSlowFastReverseAndEnvelopeSpeeds)
{
    const auto track = wave(0.25f);
    auto sequence = StretchingSequence::Create(*track, track->GetClipInterfaces());
    BoundedEnvelope envelope(false, 0.5, 2.0, 0.8);
    envelope.SetRangeLower(0.5);
    envelope.SetRangeUpper(2.0);
    envelope.SetTrackLen(512.0 / 48000);
    for (const auto [speed, useEnvelope] : { std::pair { 0.8, false }, std::pair { 1.25, false },
                                             std::pair { 0.5, false }, std::pair { 2.0, false }, std::pair { 0.8, true } }) {
        for (const bool backwards : { false, true }) {
            SCOPED_TRACE(testing::Message() << "speed=" << speed << ", envelope=" << useEnvelope << ", backwards=" << backwards);
            const auto first = (backwards ? 496.0 : 16.0) / 48000;
            const auto last = (backwards ? 16.0 : 496.0) / 48000;
            const auto expected = static_cast<size_t>(std::llround(480 / speed));
            const auto warp = useEnvelope ? Mixer::WarpOptions { &envelope } : Mixer::WarpOptions { 0.01, 32.0, speed };
            Mixer mixer({ Mixer::Input { sequence } }, std::nullopt, true, warp, first, last, 2, 73, false, 48000, floatSample);
            std::vector<float> samples;
            while (samples.size() < expected) {
                const auto requested = std::min<size_t>(73, expected - samples.size());
                const auto count = mixer.Process(requested);
                EXPECT_EQ(count, requested);
                if (!count) {
                    break;
                }
                const auto* data = reinterpret_cast<const float*>(mixer.GetBuffer(0));
                samples.insert(samples.end(), data, data + count);
                const auto projectTime = first + (backwards ? -1.0 : 1.0) * samples.size() / 48000.0 * speed;
                EXPECT_NEAR(mixer.MixGetCurrentTime(), projectTime, 0.5 / 48000);
            }
            ASSERT_EQ(samples.size(), expected);
            EXPECT_NEAR(mixer.MixGetCurrentTime(), last, 0.5 / 48000);
            EXPECT_EQ(mixer.Process(73), 0u);
            // A single large request does not need to drain the filter across
            // caller buffers. Every sample, including its legitimate boundary
            // ringing, must be independent of the caller's buffer length.
            Mixer reference({ Mixer::Input { sequence } }, std::nullopt, true, warp, first, last,
                            2, expected, false, 48000, floatSample);
            ASSERT_EQ(reference.Process(expected), expected);
            const auto* data = reinterpret_cast<const float*>(reference.GetBuffer(0));
            for (size_t frame = 0; frame < samples.size(); ++frame) {
                ASSERT_NEAR(samples[frame], data[frame], 1e-6) << frame;
            }
            EXPECT_GT(std::count_if(samples.begin(), samples.end(), [](float sample) { return sample > 0.01f; }), expected / 2);
        }
    }
}

TEST_F(NativeAudioRenderTests, MidiAndWaveAudioAreMixedByTheRegisteredRenderService)
{
    const auto audio = wave(0.125f);
    auto midi = MidiTrack::Create();
    ASSERT_TRUE(midi->SetClips({ { 1, "Phrase", 0.0, 1.0, 1, { { 1, 0.0, 1.0, 69, 100 } }, {} } }));
    midi->SetGain(0.5);
    TrackList::Get(project()).Add(midi);
    options.midiTracks = { midi };
    const auto actual = render({ audio });
    auto sequence = std::make_shared<MidiPlayableSequence>(midi, createBuiltinInstrument({}), 48000.0);
    Mixer reference({ { sequence } }, std::nullopt, true, Mixer::WarpOptions { &project() }, 0.0, options.stopTime,
                    2, 512, false, 48000.0, floatSample);
    ASSERT_EQ(reference.Process(512), 512u);
    const auto* data = reinterpret_cast<const float*>(reference.GetBuffer(0));
    ASSERT_EQ(actual[0].size(), 512u);
    for (size_t i = 0; i < 512; ++i) {
        EXPECT_NEAR(actual[0][i], data[i] + 0.125f, 1e-6);
    }
    EXPECT_EQ(actual[0], actual[1]);
}

TEST_F(NativeAudioRenderTests, VolumeAndPanAutomationAreEvaluatedForEachSample)
{
    const auto track = wave(0.25f);
    auto& automation = TrackAutomation::Get(*track);
    ASSERT_TRUE(automation.SetLane({ "volume", true, { { 0.0, 0.25 / TrackAutomation::MaximumVolume, 0.0 },
                                         { 256.0 / 48000.0, 0.75 / TrackAutomation::MaximumVolume, 0.0 } } }));
    ASSERT_TRUE(automation.SetLane({ "pan", true, { { 0.0, 0.0, 0.0 }, { 256.0 / 48000.0, 1.0, 0.0 } } }));
    const auto actual = render({ track });
    for (size_t i = 0; i < 512; ++i) {
        const auto fraction = std::min(1.0, i / 256.0);
        const auto gain = 0.25 + fraction * 0.5;
        const auto pan = fraction * 2.0 - 1.0;
        EXPECT_NEAR(actual[0][i], 0.25 * gain * std::min(1.0, 1.0 - pan), 1e-7);
        EXPECT_NEAR(actual[1][i], 0.25 * gain * std::min(1.0, 1.0 + pan), 1e-7);
    }
}

TEST_F(NativeAudioRenderTests, ImplicitBusInsertsAndFadersApplyToAllRoutedSources)
{
    ToneControls effect;
    const RealtimeEffectState::EffectFactory::Scope factory([&](const PluginID&) -> const EffectInstanceFactory* { return &effect; });
    const auto first = wave(0.2f), second = wave(0.3f), output = bus("bus");
    TrackRouting::Get(*first).output = TrackRouting::Get(*second).output = "bus";
    output->SetVolume(0.5);
    insert(RealtimeEffectList::Get(*output), -6.0);
    const auto actual = render({ first, second });
    ASSERT_EQ(actual[0].size(), 512u);
    for (auto sample : actual[0]) {
        EXPECT_NEAR(sample, 0.25 * std::pow(10.0, -6.0 / 20.0), 1e-6);
    }
    EXPECT_EQ(actual[0], actual[1]);
}

TEST_F(NativeAudioRenderTests, PreAndPostFaderSendsProduceTheExpectedBusAudio)
{
    const auto track = wave(0.4f), output = bus("send");
    track->SetVolume(0.5);
    output->SetVolume(0.25);
    for (bool pre : { true, false }) {
        TrackRouting::Get(*track).sends = { { "send", 1.0, pre } };
        const auto actual = render({ track });
        for (auto sample : actual[0]) {
            EXPECT_NEAR(sample, pre ? 0.3 : 0.25, 1e-6);
        }
    }
}

TEST_F(NativeAudioRenderTests, EffectAndMasterAutomationKeepTheirTimeAfterPreroll)
{
    ToneControls effect;
    const RealtimeEffectState::EffectFactory::Scope factory([&](const PluginID&) -> const EffectInstanceFactory* { return &effect; });
    const auto track = wave(0.5f);
    auto state = insert(RealtimeEffectList::Get(*track), 0.0);
    ASSERT_TRUE(state->Automation().SetLane({ "Gain", true, { { 0.0, 0.4, 0.0 }, { 128.0 / 48000.0, 0.5, 0.0 } } }));
    insert(RealtimeEffectList::Get(project()), -6.0);
    options.masterEffects = true;
    const auto actual = render({ track });
    const auto gain = std::pow(10.0, -6.0 / 20.0);
    for (size_t i = 0; i < actual[0].size(); ++i) {
        EXPECT_NEAR(actual[0][i], 0.5 * gain * (i < 128 ? gain : 1.0), 1e-6) << i;
    }
}

TEST_F(NativeAudioRenderTests, UpstreamEffectsDoNotShiftTheMasterAutomationGrid)
{
    ToneControls effect;
    const RealtimeEffectState::EffectFactory::Scope factory([&](const PluginID&) -> const EffectInstanceFactory* { return &effect; });
    const auto track = wave(0.5f);
    insert(RealtimeEffectList::Get(*track), 0.0);
    auto master = insert(RealtimeEffectList::Get(project()), 0.0);
    ASSERT_TRUE(master->Automation().SetLane({ "Gain", true, { { 0.0, 0.4, 0.0 }, { 128.0 / 48000.0, 0.5, 0.0 } } }));
    options.masterEffects = true;
    const auto actual = render({ track });
    for (size_t i = 0; i < actual[0].size(); ++i) {
        EXPECT_NEAR(actual[0][i], 0.5 * (i < 128 ? std::pow(10.0, -6.0 / 20.0) : 1.0), 1e-6) << i;
    }
}

TEST_F(NativeAudioRenderTests, CustomChannelMapsKeepNativeSourceChannelsAndInterleaving)
{
    const auto mono = wave(0.2f), stereo = wave(0.3f, 0.4f, 2);
    MixerOptions::Downmix mapping(3, 4);
    ASSERT_TRUE(mapping.SetNumChannels(4));
    for (size_t row = 0; row < 3; ++row) {
        for (size_t col = 0; col < 4; ++col) {
            mapping.mMap[row][col] = false;
        }
    }
    mapping.mMap[0][0] = mapping.mMap[0][3] = mapping.mMap[1][1] = mapping.mMap[2][2] = true;
    options.channels = 4;
    options.interleaved = true;
    options.channelMap = &mapping;
    options.masterEffects = true;
    auto source = AudioRenderSource::Create(project(), { mono, stereo }, options);
    const auto count = source->Process();
    ASSERT_GT(count, 0u);
    const auto* data = reinterpret_cast<const float*>(source->GetBuffer());
    for (size_t i = 0; i < count; ++i) {
        EXPECT_FLOAT_EQ(data[i * 4], 0.2f);
        EXPECT_FLOAT_EQ(data[i * 4 + 1], 0.3f);
        EXPECT_FLOAT_EQ(data[i * 4 + 2], 0.4f);
        EXPECT_FLOAT_EQ(data[i * 4 + 3], 0.2f);
    }
}

TEST_F(NativeAudioRenderTests, CustomMappingUsesTheFinalBusStemAfterRoutedAudioIsProcessed)
{
    const auto track = wave(0.2f), output = bus("bus");
    track->SetVolume(0.5);
    output->SetVolume(0.25);
    TrackRouting::Get(*track).output = "bus";
    MixerOptions::Downmix mapping(2, 4);
    ASSERT_TRUE(mapping.SetNumChannels(4));
    for (size_t row = 0; row < 2; ++row) {
        for (size_t col = 0; col < 4; ++col) {
            mapping.mMap[row][col] = false;
        }
    }
    mapping.mMap[0][0] = mapping.mMap[1][3] = true;
    options.channels = 4;
    options.interleaved = true;
    options.channelMap = &mapping;
    auto source = AudioRenderSource::Create(project(), { track }, options);
    const auto count = source->Process();
    const auto* data = reinterpret_cast<const float*>(source->GetBuffer());
    for (size_t i = 0; i < count; ++i) {
        EXPECT_NEAR(data[i * 4], 0.025, 1e-7);
        EXPECT_FLOAT_EQ(data[i * 4 + 1], 0.0f);
        EXPECT_FLOAT_EQ(data[i * 4 + 2], 0.0f);
        EXPECT_NEAR(data[i * 4 + 3], 0.025, 1e-7);
    }
}

TEST_F(NativeAudioRenderTests, ChannelModeReleasesMatchShortNotesInOfflineRenderAndStayReleasedAfterASeek)
{
    auto midi = MidiTrack::Create();
    midi->SetTempo(120);
    TrackList::Get(project()).Add(midi);
    options.midiTracks = { midi };
    options.stopTime = 1.0;
    constexpr double releaseBeat = 173.0 / 24000.0;
    for (const int mode : { 120, 123, 124, 125, 126, 127 }) {
        SCOPED_TRACE(mode);
        options.startTime = 0;
        const MidiClipData original { 1, "Mode release", 0, 1, 1, { { 1, 0, 1, 60, 100 } }, { { releaseBeat, mode, 0 } } };
        ASSERT_TRUE(midi->SetClips({ original }));
        const auto actual = render({});
        ASSERT_EQ(actual[0].size(), 48000u);
        EXPECT_TRUE(std::any_of(actual[0].begin(), actual[0].begin() + 173, [](auto x) { return x != 0; }));
        if (mode == 120) {
            EXPECT_TRUE(std::all_of(actual[0].begin() + 173, actual[0].end(), [](auto x) { return x == 0; }));
        } else {
            auto shortNote = original;
            shortNote.controllers.clear();
            shortNote.notes[0].length = releaseBeat;
            ASSERT_TRUE(midi->SetClips({ shortNote }));
            const auto expected = render({});
            EXPECT_EQ(actual, expected); // The full registered render/mixer path, including release tails.
        }
        ASSERT_TRUE(midi->SetClips({ original }));
        options.startTime = 0.01;
        const auto sought = render({});
        for (const auto& channel : sought) {
            EXPECT_TRUE(std::all_of(channel.begin(), channel.end(), [](auto x) { return x == 0; }));
        }
    }
}

TEST_F(NativeAudioRenderTests, MidiInstrumentAutomationCanBeDisabledForTheSameProject)
{
    auto midi = MidiTrack::Create();
    ASSERT_TRUE(midi->SetClips({ { 1, "Phrase", 0.0, 1.0, 1, { { 1, 0.0, 1.0, 69, 100 } }, {} } }));
    TrackList::Get(project()).Add(midi);
    ASSERT_TRUE(TrackAutomation::Get(*midi).SetLane({ "instrument/0", true, { { 0.0, 0.0, 0.0 } } }));
    options.midiTracks = { midi };
    const auto silent = render({});
    EXPECT_TRUE(std::all_of(silent[0].begin(), silent[0].end(), [](float value) { return value == 0.0f; }));
    options.trackAutomation = false;
    const auto manual = render({});
    EXPECT_TRUE(std::any_of(manual[0].begin(), manual[0].end(), [](float value) { return std::abs(value) > 0.01f; }));
}

TEST_F(NativeAudioRenderTests, WaveSourcesEndInSilenceWithoutRepeatingTheLastBlock)
{
    const auto track = wave(0.5f);
    options.stopTime = 1024.0 / 48000.0;
    const auto actual = render({ track });
    ASSERT_EQ(actual[0].size(), 1024u);
    for (size_t i = 0; i < actual[0].size(); ++i) {
        EXPECT_FLOAT_EQ(actual[0][i], i < 512 ? 0.5f : 0.0f) << i;
    }
}

TEST_F(NativeAudioRenderTests, WavExporterWritesMidiOnlyProjectsThroughTheNativeRenderer)
{
    auto midi = MidiTrack::Create();
    ASSERT_TRUE(midi->SetClips({ { 1, "Phrase", 0.0, 1.0, 1, { { 1, 0.0, 1.0, 69, 100 } }, {} } }));
    TrackList::Get(project()).Add(midi);
    ExportPCM plugin;
    int wav = -1;
    for (int i = 0; i < plugin.GetFormatCount(); ++i) {
        if (plugin.GetFormatInfo(i).format == "WAV") {
            wav = i;
            break;
        }
    }
    ASSERT_GE(wav, 0);
    auto processor = plugin.CreateProcessor(wav);
    const auto destination = directory.filePath("midi.wav").toStdString();
    ASSERT_TRUE(processor->Initialize(project(), { { SF_FORMAT_WAV, SF_FORMAT_FLOAT } }, wxString::FromUTF8(destination),
                                      0.0, options.stopTime, false, 48000.0, 2));
    struct Delegate final : ExportProcessorDelegate {
        bool IsCancelled() const override { return false; }
        bool IsStopped() const override { return false; }
        void SetStatusString(const TranslatableString&) override {}
        void OnProgress(double) override {}
    } delegate;
    EXPECT_EQ(processor->Process(delegate), ExportResult::Success);
    processor.reset();
    SF_INFO info {};
    auto* file = sf_open(destination.c_str(), SFM_READ, &info);
    ASSERT_NE(file, nullptr);
    std::vector<float> samples(info.frames * info.channels);
    EXPECT_EQ(sf_readf_float(file, samples.data(), info.frames), info.frames);
    sf_close(file);
    EXPECT_EQ(info.samplerate, 48000);
    EXPECT_EQ(info.channels, 2);
    EXPECT_EQ(info.frames, 512);
    EXPECT_TRUE(std::any_of(samples.begin(), samples.end(), [](float value) { return std::abs(value) > 0.01f; }));
}

TEST_F(NativeAudioRenderTests, CodecSizedRequestsAccumulateBoundedInternalChunksInBothOutputLayouts)
{
    const auto track = wave(0.25f);
    options.bufferSize = 8192;
    options.stopTime = 20000.0 / options.sampleRate;
    for (const bool interleaved : { false, true }) {
        SCOPED_TRACE(interleaved);
        options.interleaved = interleaved;
        auto source = AudioRenderSource::Create(project(), { track }, options);
        size_t position = 0;
        for (const size_t requested : { 8192u, 19u, 8192u, 8192u }) {
            const auto expected = std::min<size_t>(requested, 20000 - position);
            const auto count = source->Process(requested);
            ASSERT_EQ(count, expected);
            for (size_t channel = 0; channel < 2; ++channel) {
                const auto* buffer = reinterpret_cast<const float*>(source->GetBuffer(interleaved ? 0 : channel));
                for (size_t frame = 0; frame < count; ++frame) {
                    const auto sample = buffer[frame * (interleaved ? 2 : 1) + (interleaved ? channel : 0)];
                    ASSERT_FLOAT_EQ(sample, position + frame < 512 ? 0.25f : 0.0f) << position + frame;
                }
            }
            position += count;
            EXPECT_DOUBLE_EQ(source->MixGetCurrentTime(), position / options.sampleRate);
        }
        EXPECT_EQ(position, 20000u);
        EXPECT_EQ(source->Process(), 0u);
    }
}

TEST_F(NativeAudioRenderTests, PcmFilesKeepRoutedMidiStereoAutomationAndSelectionAudio)
{
    ExportPCM plugin;
    checkRoutedCodecExport(plugin, "WAV", false, { { SF_FORMAT_WAV, SF_FORMAT_FLOAT } });
}

#ifdef __WXMAC__
TEST_F(NativeAudioRenderTests, Aiff24FilesKeepRoutedMidiStereoAutomationAndSelectionAudio)
{
    ExportPCM plugin;
    checkRoutedCodecExport(plugin, "AIFF", false, { { SF_FORMAT_AIFF, SF_FORMAT_PCM_24 } });
}
#endif

#ifdef AU_NATIVE_TEST_CODEC_WAVPACK
TEST_F(NativeAudioRenderTests, WavPack16FilesKeepRoutedMidiStereoAutomationAndSelectionAudio)
{
    ExportWavPack plugin;
    checkRoutedCodecExport(plugin, "WavPack", false);
}
TEST_F(NativeAudioRenderTests, WavPack24FilesKeepRoutedMidiStereoAutomationAndSelectionAudio)
{
    ExportWavPack plugin;
    ExportProcessor::Parameters parameters;
    ASSERT_TRUE(plugin.ParseConfig(0, R"({"quality":1,"bit_rate":40,"bit_depth":24,"hybrid_mode":false})", parameters));
    checkRoutedCodecExport(plugin, "WavPack", false, std::move(parameters));
}
TEST_F(NativeAudioRenderTests, WavPackFloatFilesKeepRoutedMidiStereoAutomationAndSelectionAudio)
{
    ExportWavPack plugin;
    ExportProcessor::Parameters parameters;
    ASSERT_TRUE(plugin.ParseConfig(0, R"({"quality":1,"bit_rate":40,"bit_depth":32,"hybrid_mode":false})", parameters));
    checkRoutedCodecExport(plugin, "WavPack", false, std::move(parameters));
}
#endif

#ifdef AU_NATIVE_TEST_CODEC_MP2
TEST_F(NativeAudioRenderTests, Mp2FilesKeepRoutedMidiStereoAutomationAndSelectionAudio)
{
    ExportMP2 plugin;
    checkRoutedCodecExport(plugin, "MP2", true);
}
#endif

#ifdef AU_NATIVE_TEST_CODEC_FLAC
TEST_F(NativeAudioRenderTests, FlacFilesKeepRoutedMidiStereoAutomationAndSelectionAudio)
{
    ExportFLAC plugin;
    checkRoutedCodecExport(plugin, "FLAC", false);
}
TEST_F(NativeAudioRenderTests, Flac24FilesKeepRoutedMidiStereoAutomationAndSelectionAudio)
{
    ExportFLAC plugin;
    ExportProcessor::Parameters parameters;
    ASSERT_TRUE(plugin.ParseConfig(0, R"({"level":5,"bit_depth":24})", parameters));
    checkRoutedCodecExport(plugin, "FLAC", false, std::move(parameters));
}
#endif
#ifdef AU_NATIVE_TEST_CODEC_OGG
TEST_F(NativeAudioRenderTests, VorbisFilesKeepRoutedMidiStereoAutomationAndSelectionAudio)
{
    ExportOGG plugin;
    checkRoutedCodecExport(plugin, "OGG", true);
}
#endif
#ifdef AU_NATIVE_TEST_CODEC_OPUS
TEST_F(NativeAudioRenderTests, OpusFilesKeepRoutedMidiStereoAutomationAndSelectionAudio)
{
    ExportOpus plugin;
    checkRoutedCodecExport(plugin, "Opus", true);
}
#endif
#ifdef AU_NATIVE_TEST_CODEC_MP3
TEST_F(NativeAudioRenderTests, Mp3FilesKeepRoutedMidiStereoAutomationAndSelectionAudio)
{
    ExportMP3 plugin;
    checkRoutedCodecExport(plugin, "MP3", true);
}
#endif

#ifdef AU_NATIVE_TEST_INSTRUMENT_PATH
TEST_F(NativeAudioRenderTests, WorkerExportKeepsSerialAndMasterVstDelayEditsAlignedAndMeasuresNativeDelayOnlyOnMain)
{
    constexpr int frames = 20480;
    options.stopTime = frames / options.sampleRate;
    options.masterEffects = true;
    auto track = WaveTrackFactory::Get(project()).Create(2, floatSample, options.sampleRate);
    auto clip = track->CreateClip(0, "Serial native effect automation");
    std::vector<float> signal(frames);
    for (int frame = 0; frame < frames; ++frame) {
        signal[frame] = 0.1f + 0.01f * (frame / 32 % 8);
    }
    constSamplePtr buffers[] { reinterpret_cast<constSamplePtr>(signal.data()), reinterpret_cast<constSamplePtr>(signal.data()) };
    clip->Append(buffers, floatSample, frames, 1, floatSample);
    clip->Flush();
    track->InsertInterval(clip, true);
    TrackList::Get(project()).Add(track);
    QLibrary fixture(QString::fromUtf8(AU_NATIVE_TEST_INSTRUMENT_BINARY));
    ASSERT_TRUE(fixture.load());
    const auto offMain = reinterpret_cast<uint64_t (*)()>(fixture.resolve("nativeFixtureOffMainLatencyQueries"));
    ASSERT_NE(offMain, nullptr);
    const auto beforeQueries = offMain();
    VST3EffectsModule provider;
    auto component = provider.LoadPlugin(wxString::FromUTF8(std::string(AU_NATIVE_TEST_INSTRUMENT_PATH)
                                                            + ";F5A81297A6724B218E434FA1A4050C03"));
    const auto* effect = dynamic_cast<const VST3EffectBase*>(component.get());
    ASSERT_NE(effect, nullptr);
    const RealtimeEffectState::EffectFactory::Scope factory([&](const PluginID&) -> const EffectInstanceFactory* { return effect; });
    std::array<std::shared_ptr<RealtimeEffectState>, 3> states;
    for (size_t i = 0; i < states.size(); ++i) {
        states[i] = std::make_shared<RealtimeEffectState>("offline-sdk-delay");
        ASSERT_TRUE((i == 2 ? RealtimeEffectList::Get(project()) : RealtimeEffectList::Get(*track)).AddState(states[i]));
    }
    ASSERT_TRUE(states[0]->Automation().SetLane({ "2", true,
                                                  { { 0, 7.0 / 31 }, { 4095.0 / 48000, 7.0 / 31 }, { 4096.0 / 48000, 23.0 / 31 },
                                                      { 8191.0 / 48000, 23.0 / 31 }, { 8192.0 / 48000, 3.0 / 31 },
                                                      { 12287.0 / 48000, 3.0 / 31 }, { 12288.0 / 48000, 15.0 / 31 },
                                                      { 16383.0 / 48000, 15.0 / 31 }, { 16384.0 / 48000, 0 } } }));
    ASSERT_TRUE(states[1]->Automation().SetLane({ "2", true, { { 0, 3.0 / 31 } } }));
    ASSERT_TRUE(states[1]->Automation().SetLane({ "0", true,
                                                  { { 0, 0.5 }, { 6143.0 / 48000, 0.5 }, { 6144.0 / 48000, 0.25 } } }));
    ASSERT_TRUE(states[2]->Automation().SetLane({ "2", true,
                                                  { { 0, 11.0 / 31 }, { 8191.0 / 48000, 11.0 / 31 }, { 8192.0 / 48000, 31.0 / 31 },
                                                      { 12287.0 / 48000, 31.0 / 31 }, { 12288.0 / 48000, 0 } } }));
    for (const size_t buffer : { 73u, 8192u }) {
        for (const int first : { 0, 1536 }) {
            SCOPED_TRACE(testing::Message() << "buffer=" << buffer << ", first=" << first);
            options.bufferSize = buffer;
            options.startTime = first / options.sampleRate;
            auto source = AudioRenderSource::Create(project(), { track }, options);
            auto task = std::async(std::launch::async, [&] {
                std::array<std::vector<float>, 2> result;
                while (const auto count = source->Process()) { for (size_t channel = 0; channel < 2; ++channel) {
                                                                   const auto* data
                                                                       = reinterpret_cast<const float*>(source->GetBuffer(channel));
                                                                   result[channel].insert(result[channel].end(), data, data + count);
                                                               }
                }
                return result;
            });
            while (task.wait_for(std::chrono::milliseconds(1)) != std::future_status::ready) {
                QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
            }
            const auto actual = task.get();
            source.reset(); // Native instances are finalized on main, after the worker joins.
            ASSERT_EQ(actual[0].size(), static_cast<size_t>(frames - first));
            for (const auto& channel : actual) {
                size_t compared = 0, mismatches = 0;
                double maximum = 0;
                for (size_t frame = 0; frame < channel.size(); ++frame) {
                    const auto position = first + static_cast<int>(frame);
                    bool transition = false;
                    for (const auto change : { 4096, 6144, 8192, 12288, 16384 }) {
                        transition |= std::abs(position - change) <= 512;
                    }
                    if (transition) {
                        continue;
                    }
                    const auto expected = signal[position] * 0.5f * (position < 6144 ? 0.5f : 0.25f) * 0.5f;
                    const auto error = std::abs(channel[frame] - expected);
                    maximum = std::max(maximum, static_cast<double>(error));
                    ++compared;
                    if (error > 1e-6) {
                        ++mismatches;
                    }
                }
                EXPECT_GT(compared, 12000u);
                EXPECT_EQ(mismatches, 0u) << "maximum stable-region error=" << maximum;
            }
        }
    }
    EXPECT_EQ(offMain(), beforeQueries);
}

TEST_F(NativeAudioRenderTests, WorkerWavExportMeasuresNativeInstrumentDelayOnMainAndWritesEveryMidiSample)
{
    QLibrary fixture(QString::fromUtf8(AU_NATIVE_TEST_INSTRUMENT_BINARY));
    ASSERT_TRUE(fixture.load()) << fixture.errorString().toStdString();
    const auto offMainQueries = reinterpret_cast<uint64_t (*)()>(fixture.resolve("nativeFixtureOffMainLatencyQueries"));
    ASSERT_NE(offMainQueries, nullptr);
    auto midi = MidiTrack::Create();
    ASSERT_TRUE(midi->SetClips({ { 1, "Phrase", 0.0, 1.0, 1,
                                   { { 1, 100.0 / 24000, 200.0 / 24000, 60, 127 } }, {} } }));
    wxArrayString description;
    for (const auto* part : { "Effect", "VST3", "Audacity", "Native fixture" }) {
        description.Add(part);
    }
    description.Add(wxString::FromUTF8(std::string(AU_NATIVE_TEST_INSTRUMENT_PATH) + ";F5A81297A6724B218E434FA1A4050C01"));
    midi->SetInstrument(wxJoin(description, '_'), {});
    ASSERT_TRUE(TrackAutomation::Get(*midi).SetLane({ "instrument/2", true, { { 0, 7.0 / 31 } } }));
    TrackList::Get(project()).Add(midi);
    ExportPCM plugin;
    int wav = -1;
    for (int i = 0; i < plugin.GetFormatCount(); ++i) {
        if (plugin.GetFormatInfo(i).format == "WAV") {
            wav = i;
        }
    }
    ASSERT_GE(wav, 0);
    auto processor = plugin.CreateProcessor(wav);
    const auto destination = directory.filePath("native-worker-midi.wav").toStdString();
    ASSERT_TRUE(processor->Initialize(project(), { { SF_FORMAT_WAV, SF_FORMAT_FLOAT } }, wxString::FromUTF8(destination),
                                      0.0, options.stopTime, false, 48000.0, 2));
    struct Delegate final : ExportProcessorDelegate {
        bool IsCancelled() const override { return false; }
        bool IsStopped() const override { return false; }
        void SetStatusString(const TranslatableString&) override {}
        void OnProgress(double) override {}
    } delegate;
    const auto before = offMainQueries();
    auto task = std::async(std::launch::async, [&] { return processor->Process(delegate); });
    // Match Au3Exporter's worker/UI ownership while keeping native control
    // calls on the application thread and audio processing on the worker.
    while (task.wait_for(std::chrono::milliseconds(1)) != std::future_status::ready) {
        QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
    }
    EXPECT_EQ(task.get(), ExportResult::Success);
    EXPECT_EQ(offMainQueries(), before);
    processor.reset();
    SF_INFO info {};
    auto* file = sf_open(destination.c_str(), SFM_READ, &info);
    ASSERT_NE(file, nullptr);
    const auto close = finally([&] { sf_close(file); });
    ASSERT_EQ(info.frames, 512);
    ASSERT_EQ(info.channels, 2);
    EXPECT_EQ(info.samplerate, 48000);
    std::vector<float> samples(info.frames * info.channels);
    ASSERT_EQ(sf_readf_float(file, samples.data(), info.frames), info.frames);
    for (size_t frame = 0; frame < 512; ++frame) {
        for (size_t channel = 0; channel < 2; ++channel) {
            EXPECT_FLOAT_EQ(samples[frame * 2 + channel], frame >= 100 && frame < 300 ? 0.5f : 0.0f) << frame << ":" << channel;
        }
    }
}
#endif

TEST_F(NativeAudioRenderTests, RawRealtimeScopesPreserveSerialInsertLatencyAndEditorInstances)
{
    DelayedToneControls effect;
    const RealtimeEffectState::EffectFactory::Scope factory([&](const PluginID&) -> const EffectInstanceFactory* { return &effect; });
    const auto track = wave(0.5f);
    auto first = insert(RealtimeEffectList::Get(*track), -6.0);
    auto second = insert(RealtimeEffectList::Get(*track), -6.0);
    const auto editorInstance = first->GetInstance();
    ASSERT_TRUE(editorInstance);
    RealtimeEffects::InitializationScope initialization(project().shared_from_this(), 48000.0, 2, 512);
    initialization.AddGroup(*track, 2, 48000.0f, 512);
    EXPECT_EQ(first->PeekInstance(), editorInstance);
    std::vector<float> left(32, 1.0f), right(32, 0.5f), a(32), b(32), dummy(32);
    float* buffers[] { left.data(), right.data() };
    float* scratch[] { a.data(), b.data() };
    LinearProcessingTime time;
    {
        RealtimeEffects::ProcessingScope processing(initialization, project().shared_from_this());
        EXPECT_EQ(processing.ProcessRaw(track.get(), buffers, scratch, dummy.data(), 2, 32, &time), 0u);
    }
    const auto gain = std::pow(10.0, -12.0 / 20.0);
    for (size_t i = 0; i < left.size(); ++i) {
        EXPECT_NEAR(left[i], i < 6 ? 0.0 : gain, 1e-6) << i;
        EXPECT_NEAR(right[i], i < 6 ? 0.0 : gain* 0.5, 1e-6) << i;
    }
    EXPECT_EQ(first->ProcessingLatency(track.get()), 3u);
    EXPECT_EQ(second->ProcessingLatency(track.get()), 3u);
}

TEST_F(NativeAudioRenderTests, RealtimeAutomationUsesTheExistingWorkerSettingsForBothChannels)
{
    registerNativeEffectAutomation();
    ToneControls effect;
    const RealtimeEffectState::EffectFactory::Scope factory([&](const PluginID&) -> const EffectInstanceFactory* { return &effect; });
    const auto track = wave(0.5f);
    auto state = insert(RealtimeEffectList::Get(*track), 0.0);
    ASSERT_TRUE(state->Automation().SetLane({ "Gain", true, { { 0.0, 0.4, 0.0 }, { 512.0 / 48000.0, 0.5, 0.0 } } }));
    RealtimeEffects::InitializationScope initialization(project().shared_from_this(), 48000.0, 2, 512);
    initialization.AddGroup(*track, 2, 48000.0f, 512);
    std::vector<float> left(512), right(512), a(512), b(512), dummy(512);
    float* buffers[] { left.data(), right.data() };
    float* scratch[] { a.data(), b.data() };
    for (int block = 0; block < 2; ++block) {
        std::fill(left.begin(), left.end(), 1.0f);
        std::fill(right.begin(), right.end(), 0.5f);
        LinearProcessingTime time(block * 512);
        {
            RealtimeEffects::ProcessingScope processing(initialization, project().shared_from_this());
            processing.ProcessRaw(track.get(), buffers, scratch, dummy.data(), 2, 512, &time);
        }
        const double gain = block == 0 ? std::pow(10.0, -6.0 / 20.0) : 1.0;
        for (size_t i = 0; i < left.size(); ++i) {
            EXPECT_NEAR(left[i], gain, 1e-6);
            EXPECT_NEAR(right[i], gain * 0.5, 1e-6);
        }
    }
}

TEST_F(NativeAudioRenderTests, RoutedBypassPreservesTheConfiguredLatencyAndDryHistory)
{
    DelayedToneControls effect;
    const RealtimeEffectState::EffectFactory::Scope factory([&](const PluginID&) -> const EffectInstanceFactory* { return &effect; });
    const auto track = wave(0.5f);
    auto state = insert(RealtimeEffectList::Get(*track), -6.0);
    state->SetActive(false);
    RealtimeEffects::InitializationScope initialization(project().shared_from_this(), 48000.0, 2, 512);
    initialization.AddGroup(*track, 2, 48000.0f, 512);
    std::vector<float> left(512), right(512), a(512), b(512), dummy(512);
    float* buffers[] { left.data(), right.data() };
    float* scratch[] { a.data(), b.data() };
    {
        RealtimeEffects::ProcessingScope processing(initialization, project().shared_from_this());
        processing.ProbeRaw(track.get(), buffers, scratch, dummy.data(), 2, 512);
    }
    ASSERT_EQ(state->ProcessingLatency(track.get()), 3u);
    auto process = [&](float value) {
        std::fill(left.begin(), left.end(), value);
        std::fill(right.begin(), right.end(), value);
        RealtimeEffects::ProcessingScope processing(initialization, project().shared_from_this());
        EXPECT_EQ(processing.ProcessRaw(track.get(), buffers, scratch, dummy.data(), 2, 32), 0u);
    };
    process(1.0f);
    for (size_t i = 0; i < 32; ++i) {
        EXPECT_FLOAT_EQ(left[i], i < 3 ? 0.0f : 1.0f);
    }
    auto access = state->GetAccess();
    access->ModifySettings([](EffectSettings& settings) { settings.extra.SetActive(true); return nullptr; });
    process(2.0f);
    EXPECT_NEAR(left[31], 2.0 * std::pow(10.0, -6.0 / 20.0), 1e-6);
    access->ModifySettings([](EffectSettings& settings) { settings.extra.SetActive(false); return nullptr; });
    process(3.0f);
    for (size_t i = 0; i < 32; ++i) {
        EXPECT_FLOAT_EQ(left[i], i < 3 ? 2.0f : 3.0f);
    }
    EXPECT_EQ(state->ProcessingLatency(track.get()), 3u);
}
TEST_F(NativeAudioRenderTests, PlaybackRoutesOriginalSourceBuffersThroughBusAndMasterInserts)
{
    DelayedToneControls effect;
    const RealtimeEffectState::EffectFactory::Scope factory([&](const PluginID&) -> const EffectInstanceFactory* { return &effect; });
    const auto first = wave(0.5f), second = wave(0.25f), target = bus("bus");
    TrackRouting::Get(*first).output = "bus";
    TrackRouting::Get(*second).output = "bus";
    target->SetVolume(0.5f);
    const auto a = insert(RealtimeEffectList::Get(*first), 0.0);
    const auto b = insert(RealtimeEffectList::Get(*target), -6.0);
    const auto c = insert(RealtimeEffectList::Get(project()), -6.0);
    const auto editorInstance = b->GetInstance();
    AudioPlaybackProcessor::Sequences sequences {
        StretchingSequence::Create(*first, first->GetClipInterfaces()),
        StretchingSequence::Create(*second, second->GetClipInterfaces()),
        StretchingSequence::Create(*target, target->GetClipInterfaces())
    };
    NativePlaybackProcessor processor(project(), sequences, 48000.0, 2, 64);
    RealtimeEffects::InitializationScope initialization(project().shared_from_this(), 48000.0, 2, 512);
    for (size_t index = 0; index < sequences.size(); ++index) {
        initialization.AddGroup(*sequences[index]->FindChannelGroup(), processor.ProcessingChannels(index), 48000.0f, 512);
    }
    std::vector<float> one(64, 0.5f), two(64, 0.25f), zero(64);
    const float* inputs[] { one.data(), two.data(), zero.data(), zero.data() };
    std::vector<double> times(64);
    const auto gain = std::pow(10.0, -6.0 / 20.0);
    for (int pass = 0; pass < 3; ++pass) {
        if (pass == 2) {
            processor.Reset();
        }
        const auto start = pass == 0 ? 0 : pass == 1 ? 64 : 480000;
        for (size_t i = 0; i < 64; ++i) {
            times[i] = (start + i) / 48000.0;
        }
        RealtimeEffects::ProcessingScope scope(initialization, project().shared_from_this());
        const auto discard = processor.Process(scope, inputs, times);
        EXPECT_EQ(discard, pass == 1 ? 0u : 9u);
        for (size_t i = discard; i < 64; ++i) {
            EXPECT_NEAR(processor.Output(0)[i], 0.75 * 0.5 * gain * gain, 1e-6) << pass << ":" << i;
            EXPECT_NEAR(processor.Output(1)[i], processor.Output(0)[i], 1e-6);
            EXPECT_NEAR(processor.Meter(0, 0)[i], 0.5, 1e-6);
            EXPECT_NEAR(processor.Meter(1, 0)[i], 0.25, 1e-6);
            EXPECT_NEAR(processor.Meter(2, 0)[i], 0.75 * 0.5 * gain, 1e-6);
        }
    }
    EXPECT_EQ(b->PeekInstance(), editorInstance);
}

TEST_F(NativeAudioRenderTests, PlaybackLoopShorterThanInsertDelayKeepsAutomationAndMetersAligned)
{
    DelayedToneControls effect;
    const RealtimeEffectState::EffectFactory::Scope factory([&](const PluginID&) -> const EffectInstanceFactory* { return &effect; });
    const auto track = wave(1.0f);
    insert(RealtimeEffectList::Get(*track), 0.0);
    insert(RealtimeEffectList::Get(project()), 0.0);
    auto& automation = TrackAutomation::Get(*track);
    ASSERT_TRUE(automation.SetLane({ "volume", true, { { 0.0, 0.25 / TrackAutomation::MaximumVolume, 0.0 },
                                         { 1.0 / 48000.0, 0.75 / TrackAutomation::MaximumVolume, 0.0 } } }));
    AudioPlaybackProcessor::Sequences sequences { StretchingSequence::Create(*track, track->GetClipInterfaces()) };
    NativePlaybackProcessor processor(project(), sequences, 48000.0, 2, 2);
    RealtimeEffects::InitializationScope initialization(project().shared_from_this(), 48000.0, 2, 512);
    initialization.AddGroup(*track, processor.ProcessingChannels(0), 48000.0f, 512);
    std::vector<float> signal(2, 1.0f);
    const float* inputs[] { signal.data() };
    const double times[] { 0.0, 1.0 / 48000.0 };
    for (int loop = 0; loop < 12; ++loop) {
        RealtimeEffects::ProcessingScope scope(initialization, project().shared_from_this());
        const auto discard = processor.Process(scope, inputs, times);
        EXPECT_EQ(discard, loop < 3 ? 2u : 0u);
        if (loop >= 3) {
            EXPECT_NEAR(processor.Output(0)[0], 0.25, 1e-6);
            EXPECT_NEAR(processor.Output(0)[1], 0.75, 1e-6);
            EXPECT_NEAR(processor.Meter(0, 0)[0], 0.25, 1e-6);
            EXPECT_NEAR(processor.Meter(0, 0)[1], 0.75, 1e-6);
        }
    }
}

TEST_F(NativeAudioRenderTests, PlaybackFadersSendsMuteAndManualEffectControlsStayLive)
{
    ToneControls effect;
    const RealtimeEffectState::EffectFactory::Scope factory([&](const PluginID&) -> const EffectInstanceFactory* { return &effect; });
    const auto track = wave(1.0f), target = bus("bus");
    track->SetVolume(0.5f);
    TrackRouting::Get(*track).sends = { { "bus", 0.25, true } };
    const auto state = insert(RealtimeEffectList::Get(*target), -6.0);
    const auto editorInstance = state->GetInstance();
    AudioPlaybackProcessor::Sequences sequences { StretchingSequence::Create(*track, track->GetClipInterfaces()),
                                                  StretchingSequence::Create(*target, target->GetClipInterfaces()) };
    NativePlaybackProcessor processor(project(), sequences, 48000.0, 2, 64);
    RealtimeEffects::InitializationScope initialization(project().shared_from_this(), 48000.0, 2, 512);
    initialization.AddGroup(*track, processor.ProcessingChannels(0), 48000.0f, 512);
    initialization.AddGroup(*target, processor.ProcessingChannels(1), 48000.0f, 512);
    std::vector<float> one(64, 1.0f), zero(64);
    const float* inputs[] { one.data(), zero.data(), zero.data() };
    std::vector<double> times(64);
    auto check = [&](int block, double expected) {
        for (size_t i = 0; i < 64; ++i) {
            times[i] = (block * 64 + i) / 48000.0;
        }
        RealtimeEffects::ProcessingScope scope(initialization, project().shared_from_this());
        EXPECT_EQ(processor.Process(scope, inputs, times), 0u);
        for (size_t i = 0; i < 64; ++i) {
            EXPECT_NEAR(processor.Output(0)[i], expected, 1e-6);
        }
    };
    check(0, 0.5 + 0.25 * std::pow(10.0, -6.0 / 20.0));
    state->GetAccess()->ModifySettings([](EffectSettings& settings) {
        settings.cast<BassTrebleSettings>()->mGain = 0.0;
        return nullptr;
    });
    track->SetVolume(0.25f);
    check(1, 0.5);
    track->SetMute(true);
    check(2, 0.0);
    track->SetSolo(true);
    check(3, 0.5);
    EXPECT_EQ(state->PeekInstance(), editorInstance);
}

TEST_F(NativeAudioRenderTests, WorkerPlaybackRecompensatesChangedInsertLatencyAndKeepsBypassAligned)
{
    VariableDelayToneControls effect;
    const RealtimeEffectState::EffectFactory::Scope factory([&](const PluginID&) -> const EffectInstanceFactory* { return &effect; });
    const auto first = wave(0.0f), second = wave(0.0f);
    const auto state = insert(RealtimeEffectList::Get(*first), 0.0);
    AudioPlaybackProcessor::Sequences sequences { StretchingSequence::Create(*first, first->GetClipInterfaces()),
                                                  StretchingSequence::Create(*second, second->GetClipInterfaces()) };
    NativePlaybackProcessor processor(project(), sequences, 48000.0, 2, 64);
    RealtimeEffects::InitializationScope initialization(project().shared_from_this(), 48000.0, 2, 512);
    initialization.AddGroup(*first, 1, 48000.0f, 512);
    initialization.AddGroup(*second, 1, 48000.0f, 512);
    std::array<float, 64> a, b;
    std::array<double, 64> times;
    const float* inputs[] { a.data(), b.data() };
    for (int block = 0; block < 3; ++block) {
        if (block == 1) {
            effect.setDelay(7);
        }
        if (block == 2) {
            state->GetAccess()->ModifySettings([](EffectSettings& settings) {
                settings.extra.SetActive(false);
                return nullptr;
            });
        }
        for (size_t i = 0; i < a.size(); ++i) {
            a[i] = (block * 64 + i + 1) / 1000.0f;
            b[i] = a[i] * 2.0f;
            times[i] = (block * 64 + i) / 48000.0;
        }
        RealtimeEffects::ProcessingScope scope(initialization, project().shared_from_this());
        const auto discard = processor.Process(scope, inputs, times);
        const size_t delay = block ? 7 : 3;
        EXPECT_EQ(discard, block == 2 ? 0u : delay);
        EXPECT_EQ(state->ProcessingLatency(first.get()), delay);
        for (size_t i = discard; i < a.size(); ++i) {
            const auto expected = (block * 64 + static_cast<int>(i) - delay + 1) / 1000.0 * 3.0;
            EXPECT_NEAR(processor.Output(0)[i], expected, 1e-6) << block << ":" << i;
            EXPECT_NEAR(processor.Output(1)[i], expected, 1e-6) << block << ":" << i;
        }
    }
}

TEST_F(NativeAudioRenderTests, MainThreadManagerRefreshPublishesPendingPluginDelayForTheNextWorkerBlock)
{
    VariableDelayToneControls effect;
    const RealtimeEffectState::EffectFactory::Scope factory([&](const PluginID&) -> const EffectInstanceFactory* { return &effect; });
    const auto first = wave(0.0f), second = wave(0.0f);
    const auto state = insert(RealtimeEffectList::Get(*first), 0.0);
    auto& manager = RealtimeEffectManager::Get(project());
    EXPECT_FALSE(manager.LatencyUpdatePending());
    EXPECT_FALSE(manager.RefreshRealtimeLatencies());
    AudioPlaybackProcessor::Sequences sequences { StretchingSequence::Create(*first, first->GetClipInterfaces()),
                                                  StretchingSequence::Create(*second, second->GetClipInterfaces()) };
    NativePlaybackProcessor processor(project(), sequences, 48000.0, 2, 64);
    {
        RealtimeEffects::InitializationScope initialization(project().shared_from_this(), 48000.0, 2, 512);
        initialization.AddGroup(*first, 1, 48000.0f, 512);
        initialization.AddGroup(*second, 1, 48000.0f, 512);
        std::array<float, 64> a, b;
        std::array<double, 64> times;
        const float* inputs[] { a.data(), b.data() };
        for (int block = 0; block < 2; ++block) {
            if (block) {
                effect.requestDelay(13);
                EXPECT_TRUE(manager.LatencyUpdatePending());
                EXPECT_EQ(effect.revision.load(), 0u);
                EXPECT_EQ(state->ProcessingLatency(first.get()), 3u);
                EXPECT_TRUE(manager.RefreshRealtimeLatencies());
                EXPECT_FALSE(manager.LatencyUpdatePending());
                EXPECT_FALSE(manager.RefreshRealtimeLatencies());
                EXPECT_EQ(effect.revision.load(), 1u);
            }
            for (size_t i = 0; i < a.size(); ++i) {
                a[i] = (block * 64 + i + 1) / 1000.0f;
                b[i] = a[i] * 2.0f;
                times[i] = (block * 64 + i) / 48000.0;
            }
            RealtimeEffects::ProcessingScope scope(initialization, project().shared_from_this());
            const auto discard = processor.Process(scope, inputs, times);
            const size_t delay = block ? 13 : 3;
            EXPECT_EQ(discard, delay);
            EXPECT_EQ(state->ProcessingLatency(first.get()), delay);
            for (size_t i = discard; i < a.size(); ++i) {
                EXPECT_NEAR(processor.Output(0)[i], (block * 64 + i - delay + 1) / 1000.0 * 3.0, 1e-6);
            }
        }
    }
    EXPECT_FALSE(manager.LatencyUpdatePending());
    EXPECT_FALSE(manager.RefreshRealtimeLatencies());
}

TEST_F(NativeAudioRenderTests, RoutedPlaybackClockTracksTheRenderedRampAcrossRepeatedDelayChanges)
{
    VariableDelayToneControls effect;
    const RealtimeEffectState::EffectFactory::Scope factory([&](const PluginID&) -> const EffectInstanceFactory* { return &effect; });
    const auto track = wave(0.0f);
    insert(RealtimeEffectList::Get(*track), 0.0);
    AudioPlaybackProcessor::Sequences sequences { StretchingSequence::Create(*track, track->GetClipInterfaces()) };
    NativePlaybackProcessor processor(project(), sequences, 48000.0, 2, 64);
    RealtimeEffects::InitializationScope initialization(project().shared_from_this(), 48000.0, 2, 512);
    initialization.AddGroup(*track, 1, 48000.0f, 512);
    AudioIOStartStreamOptions options(project().shared_from_this(), 48000.0);
    PlaybackSchedule schedule;
    schedule.Init(0.0, 2.0, options, nullptr);
    auto& policy = schedule.GetPolicy();
    policy.Initialize(schedule, 48000.0);
    const auto finalize = finally([&] { policy.Finalize(schedule); });
    schedule.mTimeQueue.Init(64);
    schedule.mTimeQueue.Prime(0.0);
    std::array<float, 64> input {};
    std::array<double, 65> times {};
    const float* inputs[] { input.data() };
    size_t consumed = 0, checked = 0;
    for (int block = 0; block < 48; ++block) {
        if (block == 8) {
            effect.setDelay(13);
        }
        if (block == 16) {
            effect.setDelay(1);
        }
        if (block == 24) {
            effect.setDelay(31);
        }
        if (block == 32) {
            effect.setDelay(0);
        }
        schedule.mTimeQueue.GenerateTimes(schedule, PlaybackSlice { 64, 64, 64 }, times);
        for (size_t frame = 0; frame < input.size(); ++frame) {
            input[frame] = (times[frame] * 48000.0 + 1.0) / 100000.0;
        }
        RealtimeEffects::ProcessingScope scope(initialization, project().shared_from_this());
        const auto discard = processor.Process(scope, inputs, { times.data(), input.size() }, times.back());
        schedule.mTimeQueue.PublishTimes(processor.OutputTimes().subspan(discard));
        for (size_t frame = discard; frame < input.size(); ++frame) {
            const auto time = schedule.mTimeQueue.Consumer(1, 48000.0);
            if (++consumed % TimeQueueGrainSize == 0) {
                // The ramp identifies the actual audible source sample. At a
                // queue boundary the next playhead position must match it,
                // including after increasing and decreasing the insert delay.
                EXPECT_NEAR(time, processor.Output(0)[frame] * 100000.0 / 48000.0, 1e-8) << block << ":" << frame;
                ++checked;
            }
        }
    }
    EXPECT_GE(checked, 6u);
}

TEST_F(NativeAudioRenderTests, DeviceCallbackRequestsLatencyRebuildBeforeProcessingAndMainRefreshesTheDelay)
{
    VariableDelayToneControls effect;
    const RealtimeEffectState::EffectFactory::Scope factory([&](const PluginID&) -> const EffectInstanceFactory* { return &effect; });
    const auto track = wave(0.0f);
    const auto state = insert(RealtimeEffectList::Get(*track), 0.0);
    AudioPlaybackProcessor::Sequences sequences { StretchingSequence::Create(*track, track->GetClipInterfaces()) };
    RealtimeEffects::InitializationScope initialization(project().shared_from_this(), 48000.0, 2, 512);
    initialization.AddGroup(*track, 1, 48000.0f, 512);
    std::array<float, 64> input;
    input.fill(1.0f);
    const float* inputs[] { input.data() };
    std::array<double, 64> times {};
    NativePlaybackProcessor callback(project(), sequences, 48000.0, 2, 64, false);
    {
        RealtimeEffects::ProcessingScope scope(initialization, project().shared_from_this());
        callback.Process(scope, inputs, times); // Main-thread probe, before publication.
    }
    callback.SetRealtimeMode();
    effect.setDelay(11);
    const auto calls = effect.calls;
    {
        RealtimeEffects::ProcessingScope scope(initialization, project().shared_from_this());
        EXPECT_EQ(callback.Process(scope, inputs, times), times.size());
    }
    EXPECT_EQ(effect.calls, calls);
    EXPECT_TRUE(callback.NeedsRebuild());
    EXPECT_TRUE(std::all_of(callback.Output(0), callback.Output(0) + times.size(), [](auto value) { return value == 0.0f; }));
    EXPECT_EQ(state->ProcessingLatency(track.get()), 3u); // Cached history has not been resized on callback.
    NativePlaybackProcessor refreshed(project(), sequences, 48000.0, 2, 64, false);
    {
        RealtimeEffects::ProcessingScope scope(initialization, project().shared_from_this());
        EXPECT_EQ(refreshed.Process(scope, inputs, times), 11u);
    }
    EXPECT_EQ(state->ProcessingLatency(track.get()), 11u);
    refreshed.SetRealtimeMode();
    {
        RealtimeEffects::ProcessingScope scope(initialization, project().shared_from_this());
        EXPECT_EQ(refreshed.Process(scope, inputs, times), 0u);
    }
    EXPECT_FALSE(refreshed.NeedsRebuild());
    EXPECT_FLOAT_EQ(refreshed.Output(0)[63], 1.0f);
}

#ifdef AU_NATIVE_TEST_LV2_PATH
TEST_F(NativeAudioRenderTests, ChangingBusEffectDelayKeepsNonconstantWaveAndDryPathsAlignedThroughoutExport)
{
    constexpr int frames = 20480;
    options.stopTime = frames / options.sampleRate;
    options.masterEffects = true;
    std::vector<float> signal(frames), opposite(frames);
    for (int frame = 0; frame < frames; ++frame) {
        signal[frame] = 0.1f + 0.01f * (frame / 32 % 8);
        opposite[frame] = -0.5f * signal[frame];
    }
    const auto addWave = [&](const std::vector<float>& samples) {
        auto track = WaveTrackFactory::Get(project()).Create(1, floatSample, options.sampleRate);
        auto clip = track->CreateClip(0, "Variable effect delay");
        constSamplePtr buffers[] { reinterpret_cast<constSamplePtr>(samples.data()) };
        clip->Append(buffers, floatSample, samples.size(), 1, floatSample);
        clip->Flush();
        track->InsertInterval(clip, true);
        TrackList::Get(project()).Add(track);
        return track;
    };
    const auto source = addWave(signal), dry = addWave(opposite), target = bus("variable-effect-bus");
    TrackRouting::Get(*source).output = "variable-effect-bus";
    const auto location = QUrl::fromLocalFile(QString::fromUtf8(AU_NATIVE_TEST_LV2_PATH)).toEncoded().toStdString();
    auto bundle = createLv2Instrument("urn:audacity:test:native-instrument@" + location, {});
    LV2EffectsModule provider;
    const auto* plugin = provider.GetPlugin(wxString::FromUTF8("urn:audacity:test:fixed-audio-effect@" + location));
    ASSERT_NE(plugin, nullptr);
    LV2EffectBase effect(*plugin);
    const RealtimeEffectState::EffectFactory::Scope factory([&](const PluginID&) -> const EffectInstanceFactory* { return &effect; });
    auto state = std::make_shared<RealtimeEffectState>("variable-offline-delay");
    ASSERT_TRUE(RealtimeEffectList::Get(*target).AddState(state));
    ASSERT_TRUE(state->Automation().SetLane({ "lookahead", true,
                                              { { 0, 7.0 / 31 }, { 4095.0 / 48000, 7.0 / 31 }, { 4096.0 / 48000, 23.0 / 31 },
                                                  { 8191.0 / 48000, 23.0 / 31 }, { 8192.0 / 48000, 3.0 / 31 },
                                                  { 12287.0 / 48000, 3.0 / 31 }, { 12288.0 / 48000, 15.0 / 31 },
                                                  { 16383.0 / 48000, 15.0 / 31 }, { 16384.0 / 48000, 0 } } }));
    auto master = std::make_shared<RealtimeEffectState>("fixed-master-delay");
    ASSERT_TRUE(RealtimeEffectList::Get(project()).AddState(master));
    ASSERT_TRUE(master->Automation().SetLane({ "lookahead", true, { { 0, 19.0 / 31 } } }));
    for (const size_t buffer : { 73u, 8192u }) {
        for (const int first : { 0, 1536 }) {
            SCOPED_TRACE(testing::Message() << "buffer=" << buffer << ", first=" << first);
            options.bufferSize = buffer;
            options.startTime = first / options.sampleRate;
            const auto actual = render({ source, dry });
            ASSERT_EQ(actual[0].size(), static_cast<size_t>(frames - first));
            for (const auto& channel : actual) {
                size_t compared = 0, mismatches = 0;
                double maximum = 0;
                for (size_t frame = 0; frame < channel.size(); ++frame) {
                    const auto position = first + static_cast<int>(frame);
                    // Native delay edits can alter their own buffered transition.
                    // Beyond that bounded settling interval, every path must
                    // regain its original content position rather than drift.
                    bool transition = false;
                    for (const auto change : { 4096, 8192, 12288, 16384 }) {
                        transition |= std::abs(position - change) <= 1024;
                    }
                    if (transition) {
                        continue;
                    }
                    ++compared;
                    maximum = std::max(maximum, static_cast<double>(std::abs(channel[frame])));
                    if (std::abs(channel[frame]) > 1e-6) {
                        ++mismatches;
                    }
                }
                EXPECT_GT(compared, 8000u);
                EXPECT_EQ(mismatches, 0u) << "maximum stable-region residual=" << maximum;
            }
        }
    }
    EXPECT_FLOAT_EQ(GetSettings(state->GetAccess()->Get()).values[1], 0.0f);
    EXPECT_FLOAT_EQ(GetSettings(master->GetAccess()->Get()).values[1], 0.0f);
}

TEST_F(NativeAudioRenderTests, ChangingInstrumentDelayKeepsNonconstantWaveAudioAlignedThroughAutomatedBusSendsAndMaster)
{
    constexpr int frames = 5120;
    options.stopTime = frames / options.sampleRate;
    options.masterEffects = true;
    const auto location = QUrl::fromLocalFile(QString::fromUtf8(AU_NATIVE_TEST_LV2_PATH)).toEncoded().toStdString();
    const auto instrumentPath = "urn:audacity:test:delayed-instrument@" + location;
    auto instrument = createLv2Instrument(instrumentPath, {});
    auto midi = MidiTrack::Create();
    std::vector<MidiNoteData> notes;
    std::vector<float> opposite(frames);
    for (int first = 0; first < frames; first += 64) {
        const auto velocity = 32 + (first / 64 % 8) * 8;
        notes.push_back({ first / 64 + 1, first / 24000.0, 64.0 / 24000, 60, velocity });
        std::fill_n(opposite.begin() + first, 64, -0.5f * velocity / 127.0f);
    }
    ASSERT_TRUE(midi->SetClips({ { 1, "Delay transitions", 0, frames / 24000.0, 1, notes, {} } }));
    wxArrayString description;
    for (const auto* part : { "Effect", "LV2", "Audacity", "Delayed fixture" }) {
        description.Add(part);
    }
    description.Add(wxString::FromUTF8(instrumentPath));
    midi->SetInstrument(wxJoin(description, '_'), wxString::FromUTF8(instrument->state()));
    ASSERT_TRUE(TrackAutomation::Get(*midi).SetLane({ "instrument/lookahead", true,
                                                      { { 0, 7.0 / 31 }, { 1023.0 / 48000, 7.0 / 31 }, { 1024.0 / 48000, 23.0 / 31 },
                                                          { 2047.0 / 48000, 23.0 / 31 }, { 2048.0 / 48000, 3.0 / 31 },
                                                          { 3071.0 / 48000, 3.0 / 31 }, { 3072.0 / 48000, 15.0 / 31 },
                                                          { 4095.0 / 48000, 15.0 / 31 }, { 4096.0 / 48000, 0 } } }));
    TrackList::Get(project()).Add(midi);
    options.midiTracks = { midi };

    auto dry = WaveTrackFactory::Get(project()).Create(2, floatSample, options.sampleRate);
    auto clip = dry->CreateClip(0.0, "Opposite waveform");
    const auto* data = reinterpret_cast<constSamplePtr>(opposite.data());
    constSamplePtr channels[] { data, data };
    clip->Append(channels, floatSample, opposite.size(), 1, floatSample);
    clip->Flush();
    dry->InsertInterval(clip, true);
    TrackList::Get(project()).Add(dry);
    const auto output = bus("delay-main"), send = bus("delay-send");
    TrackRouting::Get(*midi).output = TrackRouting::Get(*dry).output = "delay-main";
    for (auto* track : { static_cast<ChannelGroup*>(midi.get()), static_cast<ChannelGroup*>(dry.get()) }) {
        auto& automation = TrackAutomation::Get(*track);
        ASSERT_TRUE(automation.SetLane({ "volume", true,
                                         { { 0, 0.2 / TrackAutomation::MaximumVolume },
                                             { options.stopTime, 0.9 / TrackAutomation::MaximumVolume } } }));
        ASSERT_TRUE(automation.SetLane({ "pan", true, { { 0, 0.15 }, { options.stopTime, 0.85 } } }));
    }
    ASSERT_TRUE(TrackAutomation::Get(*output).SetLane({ "volume", true,
                                                        { { 0, 0.8 / TrackAutomation::MaximumVolume },
                                                            { options.stopTime, 0.3 / TrackAutomation::MaximumVolume } } }));
    send->SetVolume(0.4);

    LV2EffectsModule provider;
    const auto* plugin = provider.GetPlugin(wxString::FromUTF8("urn:audacity:test:fixed-audio-effect@" + location));
    ASSERT_NE(plugin, nullptr);
    LV2EffectBase effect(*plugin);
    const RealtimeEffectState::EffectFactory::Scope factory([&](const PluginID&) -> const EffectInstanceFactory* { return &effect; });
    std::array<std::shared_ptr<RealtimeEffectState>, 3> states;
    const std::array lists { &RealtimeEffectList::Get(*output), &RealtimeEffectList::Get(*send), &RealtimeEffectList::Get(project()) };
    const std::array delays { 11.0, 19.0, 3.0 };
    for (size_t i = 0; i < states.size(); ++i) {
        states[i] = std::make_shared<RealtimeEffectState>("native-delay-effect");
        ASSERT_TRUE(lists[i]->AddState(states[i]));
        ASSERT_TRUE(states[i]->Automation().SetLane({ "lookahead", true, { { 0, delays[i] / 31 } } }));
    }
    for (const bool pre : { false, true }) {
        TrackRouting::Get(*midi).sends = TrackRouting::Get(*dry).sends = { { "delay-send", 0.7, pre } };
        for (const size_t block : { 73u, 4096u }) {
            options.bufferSize = block;
            for (const int first : { 0, 1536 }) {
                SCOPED_TRACE(testing::Message() << "pre=" << pre << ", block=" << block << ", first=" << first);
                options.startTime = first / options.sampleRate;
                const auto audio = render({ dry });
                ASSERT_EQ(audio[0].size(), static_cast<size_t>(frames - first));
                for (const auto& channel : audio) {
                    size_t compared = 0, mismatches = 0;
                    double maximum = 0;
                    for (size_t frame = 0; frame < channel.size(); ++frame) {
                        const auto position = first + static_cast<int>(frame);
                        // Only the plugin's delay transitions may omit queued
                        // content. Stable regions must cancel against actual
                        // Wave PCM, including nonzero selection starts.
                        bool transition = false;
                        for (const auto change : { 1024, 2048, 3072, 4096 }) {
                            transition |= std::abs(position - change) <= 256;
                        }
                        if (transition) {
                            continue;
                        }
                        ++compared;
                        maximum = std::max(maximum, static_cast<double>(std::abs(channel[frame])));
                        if (std::abs(channel[frame]) > 1e-6) {
                            ++mismatches;
                        }
                    }
                    EXPECT_GT(compared, 1900u);
                    EXPECT_EQ(mismatches, 0u) << "maximum stable-region residual=" << maximum;
                }
            }
        }
    }
}

TEST_F(NativeAudioRenderTests, AutomatedInstrumentDelayStaysAlignedWithWaveBusAndMasterDuringExport)
{
    const auto location = QUrl::fromLocalFile(QString::fromUtf8(AU_NATIVE_TEST_LV2_PATH)).toEncoded().toStdString();
    auto instrument = createLv2Instrument("urn:audacity:test:delayed-instrument@" + location, {});
    auto midi = MidiTrack::Create();
    ASSERT_TRUE(midi->SetClips({ { 1, "Held note", 0, 4, 1, { { 1, 0, 4, 60, 127 } }, {} } }));
    wxArrayString description;
    for (const auto* part : { "Effect", "LV2", "Audacity", "Delayed fixture" }) {
        description.Add(part);
    }
    description.Add(wxString::FromUTF8("urn:audacity:test:delayed-instrument@" + location));
    midi->SetInstrument(wxJoin(description, '_'), wxString::FromUTF8(instrument->state()));
    ASSERT_TRUE(TrackAutomation::Get(*midi).SetLane({ "instrument/lookahead", true, { { 0, 23.0 / 31 } } }));
    TrackList::Get(project()).Add(midi);
    options.midiTracks = { midi };
    options.masterEffects = true;
    const auto target = bus("instrument-delay-bus");
    TrackRouting::Get(*midi).output = "instrument-delay-bus";
    const auto dry = wave(-0.25f);
    LV2EffectsModule provider;
    const auto* plugin = provider.GetPlugin(wxString::FromUTF8("urn:audacity:test:fixed-audio-effect@" + location));
    ASSERT_NE(plugin, nullptr);
    LV2EffectBase effect(*plugin);
    const RealtimeEffectState::EffectFactory::Scope factory([&](const PluginID&) -> const EffectInstanceFactory* { return &effect; });
    auto busEffect = std::make_shared<RealtimeEffectState>("native-variable-delay");
    auto masterEffect = std::make_shared<RealtimeEffectState>("native-variable-delay");
    ASSERT_TRUE(RealtimeEffectList::Get(*target).AddState(busEffect));
    ASSERT_TRUE(RealtimeEffectList::Get(project()).AddState(masterEffect));
    ASSERT_TRUE(busEffect->Automation().SetLane({ "lookahead", true, { { 0, 11.0 / 31 } } }));
    ASSERT_TRUE(masterEffect->Automation().SetLane({ "lookahead", true, { { 0, 19.0 / 31 } } }));
    for (const int first : { 0, 128 }) {
        SCOPED_TRACE(first);
        options.startTime = first / options.sampleRate;
        const auto audio = render({ dry });
        ASSERT_EQ(audio[0].size(), 512u - first);
        for (const auto& channel : audio) {
            for (size_t frame = 0; frame < channel.size(); ++frame) {
                // The real synth and two effect delays must cancel the parallel
                // Wave source from frame zero through the exact final sample.
                ASSERT_FLOAT_EQ(channel[frame], 0.0f) << frame;
            }
        }
    }
}

TEST_F(NativeAudioRenderTests, SelectionStartAutomationPrimesRealTrackBusAndMasterPluginDelaysBeforeAlignment)
{
    options.masterEffects = true;
    const auto location = QUrl::fromLocalFile(QString::fromUtf8(AU_NATIVE_TEST_LV2_PATH)).toEncoded().toStdString();
    auto bundle = createLv2Instrument("urn:audacity:test:native-instrument@" + location, {});
    LV2EffectsModule provider;
    const auto* plugin = provider.GetPlugin(wxString::FromUTF8("urn:audacity:test:fixed-audio-effect@" + location));
    ASSERT_NE(plugin, nullptr);
    LV2EffectBase effect(*plugin);
    const RealtimeEffectState::EffectFactory::Scope factory([&](const PluginID&) -> const EffectInstanceFactory* { return &effect; });
    const auto source = wave(0.25f);
    const auto dry = wave(0.125f);
    const auto target = bus("automated-delay-bus");
    TrackRouting::Get(*source).output = "automated-delay-bus";
    std::array<std::shared_ptr<RealtimeEffectState>, 3> states;
    states[0] = std::make_shared<RealtimeEffectState>("native-variable-delay");
    states[1] = std::make_shared<RealtimeEffectState>("native-variable-delay");
    states[2] = std::make_shared<RealtimeEffectState>("native-variable-delay");
    ASSERT_TRUE(RealtimeEffectList::Get(*source).AddState(states[0]));
    ASSERT_TRUE(RealtimeEffectList::Get(*target).AddState(states[1]));
    ASSERT_TRUE(RealtimeEffectList::Get(project()).AddState(states[2]));
    const std::array delays { 7.0, 11.0, 19.0 };
    for (const int first : { 0, 128 }) {
        SCOPED_TRACE(first);
        options.startTime = first / options.sampleRate;
        for (size_t i = 0; i < states.size(); ++i) {
            const auto value = delays[i] / 31.0;
            const std::vector<AutomationPointData> points = first == 0
                                                            ? std::vector<AutomationPointData> { { 0.0, value, 0.0 } }
            : std::vector<AutomationPointData> { { 0.0, 0.0, 0.0 }, { options.startTime, value, 0.0 } };
            ASSERT_TRUE(states[i]->Automation().SetLane({ "lookahead", true, points }));
        }
        const auto audio = render({ source, dry });
        ASSERT_EQ(audio[0].size(), 512u - first);
        for (const auto& channel : audio) {
            for (size_t frame = 0; frame < channel.size(); ++frame) {
                // Three real plugin delays, plus a dry parallel path. Missing
                // initial compensation shifts or truncates this constant signal.
                ASSERT_FLOAT_EQ(channel[frame], (0.25f * 0.5f * 0.5f + 0.125f) * 0.5f) << frame;
            }
        }
        for (const auto& state : states) {
            EXPECT_FLOAT_EQ(GetSettings(state->GetAccess()->Get()).values[1], 0.0f);
        }
    }
}

TEST_F(NativeAudioRenderTests, SmallDeviceSlicesProbeTheNativeFixedBlockPluginBeforeCachingItsLatency)
{
    const auto location = QUrl::fromLocalFile(QString::fromUtf8(AU_NATIVE_TEST_LV2_PATH)).toEncoded().toStdString();
    auto bundle = createLv2Instrument("urn:audacity:test:native-instrument@" + location, {});
    LV2EffectsModule provider;
    const auto* plugin = provider.GetPlugin(wxString::FromUTF8("urn:audacity:test:fixed-audio-effect@" + location));
    ASSERT_NE(plugin, nullptr);
    LV2EffectBase effect(*plugin);
    const RealtimeEffectState::EffectFactory::Scope factory([&](const PluginID&) -> const EffectInstanceFactory* { return &effect; });
    const auto track = wave(0.0f);
    auto state = std::make_shared<RealtimeEffectState>("latency-fixture");
    state->GetAccess()->ModifySettings([](EffectSettings& settings) { GetSettings(settings).values[1] = 7.0f; return nullptr; });
    ASSERT_TRUE(RealtimeEffectList::Get(*track).AddState(state));
    AudioPlaybackProcessor::Sequences sequences { StretchingSequence::Create(*track, track->GetClipInterfaces()) };
    NativePlaybackProcessor processor(project(), sequences, 48000.0, 2, 64);
    RealtimeEffects::InitializationScope initialization(project().shared_from_this(), 48000.0, 2, 512);
    initialization.AddGroup(*track, 1, 48000.0f, 512);
    std::array<float, 64> input {};
    const float* inputs[] { input.data() };
    std::array<double, 64> times {};
    {
        RealtimeEffects::ProcessingScope scope(initialization, project().shared_from_this());
        EXPECT_EQ(processor.Process(scope, inputs, times), 64u);
    }
    EXPECT_EQ(state->ProcessingLatency(track.get()), 511u + 7u);
    EXPECT_GT(state->ProcessingLatencyRevision(), 0u);
    processor.SetRealtimeMode();
    {
        RealtimeEffects::ProcessingScope scope(initialization, project().shared_from_this());
        processor.Process(scope, inputs, times);
    }
    EXPECT_FALSE(processor.NeedsRebuild());
}
#endif

TEST_F(NativeAudioRenderTests, EffectsReceiveProjectTempoMeterAndDelayedContentPositionDuringPlaybackAndSeek)
{
    auto& signature = ProjectTimeSignature::Get(project());
    signature.SetTempo(93.0);
    signature.SetUpperTimeSignature(7);
    signature.SetLowerTimeSignature(8);
    DelayedToneControls delay;
    TransportToneControls busEffect, masterEffect;
    const RealtimeEffectState::EffectFactory::Scope factory([&](const PluginID& id) -> const EffectInstanceFactory* {
        if (id == "transport-bus") {
            return &busEffect;
        }
        if (id == "transport-master") {
            return &masterEffect;
        }
        return &delay;
    });
    const auto track = wave(0.5f), target = bus("musical-bus");
    TrackRouting::Get(*track).output = "musical-bus";
    insert(RealtimeEffectList::Get(*track), 0.0);
    ASSERT_TRUE(RealtimeEffectList::Get(*target).AddState(std::make_shared<RealtimeEffectState>("transport-bus")));
    ASSERT_TRUE(RealtimeEffectList::Get(project()).AddState(std::make_shared<RealtimeEffectState>("transport-master")));
    AudioPlaybackProcessor::Sequences sequences { StretchingSequence::Create(*track, track->GetClipInterfaces()),
                                                  StretchingSequence::Create(*target, target->GetClipInterfaces()) };
    NativePlaybackProcessor processor(project(), sequences, 48000.0, 2, 64);
    RealtimeEffects::InitializationScope initialization(project().shared_from_this(), 48000.0, 2, 512);
    for (size_t i = 0; i < sequences.size(); ++i) {
        initialization.AddGroup(*sequences[i]->FindChannelGroup(), processor.ProcessingChannels(i), 48000.0f, 512);
    }
    std::array<float, 64> one, zero {};
    one.fill(0.5f);
    const float* inputs[] { one.data(), zero.data(), zero.data() };
    std::array<double, 64> times;
    int pass = 0;
    for (const double start : { 1.0, 10.0, 0.0 }) {
        const double speed = pass++ == 1 ? 2.0 : 1.0;
        processor.Reset();
        const bool cycling = pass != 2;
        processor.SetPlaybackLoop(cycling, 0.5, 1.5);
        busEffect.records.clear();
        masterEffect.records.clear();
        for (size_t i = 0; i < times.size(); ++i) {
            times[i] = start + i / 48000.0 * speed;
        }
        RealtimeEffects::ProcessingScope scope(initialization, project().shared_from_this());
        processor.Process(scope, inputs, times);
        for (const auto* effect : { &busEffect, &masterEffect }) {
            ASSERT_FALSE(effect->records.empty());
            const auto& context = effect->records.back().context;
            EXPECT_NEAR(context.time, start - 3.0 / 48000.0 * speed, 1e-12);
            EXPECT_NEAR(context.speed, speed, 1e-7);
            EXPECT_DOUBLE_EQ(context.sampleRate, 48000.0);
            EXPECT_DOUBLE_EQ(context.tempo, 93.0);
            EXPECT_EQ(context.numerator, 7);
            EXPECT_EQ(context.denominator, 8);
            EXPECT_TRUE(context.playing);
            EXPECT_EQ(context.cycling, cycling);
            EXPECT_DOUBLE_EQ(context.cycleStart, 0.5);
            EXPECT_DOUBLE_EQ(context.cycleEnd, 1.5);
        }
    }
}

TEST_F(NativeAudioRenderTests, RunningEffectsFollowMeterEditsWithoutRebuildingOrChangingTheCompiledTempo)
{
    auto& signature = ProjectTimeSignature::Get(project());
    signature.SetTempo(120);
    signature.SetUpperTimeSignature(4);
    signature.SetLowerTimeSignature(4);
    TransportToneControls effect;
    const RealtimeEffectState::EffectFactory::Scope factory([&](const PluginID&) -> const EffectInstanceFactory* { return &effect; });
    const auto track = wave(0.5f);
    const auto state = insert(RealtimeEffectList::Get(*track), 0.0);
    AudioPlaybackProcessor::Sequences sequences { StretchingSequence::Create(*track, track->GetClipInterfaces()) };
    NativePlaybackProcessor processor(project(), sequences, 48000, 2, 64);
    RealtimeEffects::InitializationScope initialization(project().shared_from_this(), 48000, 2, 64);
    initialization.AddGroup(*track, 1, 48000, 64);
    const auto instance = state->PeekInstance();
    std::array<float, 64> input;
    input.fill(0.5f);
    const float* inputs[] { input.data() };
    std::array<double, 64> times {};
    const auto process = [&] {
        std::thread audio([&] {
            RealtimeEffects::ProcessingScope scope(initialization, project().shared_from_this());
            processor.Process(scope, inputs, times);
        });
        audio.join();
    };
    process();
    for (const auto meter : { std::pair { 7, 8 }, std::pair { 3, 4 }, std::pair { 6, 16 } }) {
        signature.SetUpperTimeSignature(meter.first);
        signature.SetLowerTimeSignature(meter.second);
        signature.SetTempo(150); // Live BPM edits are disabled in UI; keep the compiled tempo if changed programmatically.
        effect.records.clear();
        processor.SetPlaybackLoop(true, 0, 1);
        process();
        ASSERT_FALSE(effect.records.empty());
        for (const auto& record : effect.records) {
            EXPECT_EQ(record.context.numerator, meter.first);
            EXPECT_EQ(record.context.denominator, meter.second);
            EXPECT_DOUBLE_EQ(record.context.tempo, 120);
            EXPECT_TRUE(record.context.playing);
            EXPECT_TRUE(record.context.cycling);
        }
        EXPECT_EQ(state->PeekInstance(), instance);
        EXPECT_FALSE(processor.NeedsRebuild());
        EXPECT_TRUE(std::all_of(processor.Output(0), processor.Output(0) + input.size(), [](auto x) { return x == 0.5f; }));
    }
}

TEST_F(NativeAudioRenderTests, ExportEffectsReceiveProjectMusicalContextAtTheBlockInputPosition)
{
    auto& signature = ProjectTimeSignature::Get(project());
    signature.SetTempo(87.0);
    signature.SetUpperTimeSignature(5);
    signature.SetLowerTimeSignature(8);
    TransportToneControls effect;
    const RealtimeEffectState::EffectFactory::Scope factory([&](const PluginID&) -> const EffectInstanceFactory* { return &effect; });
    const auto track = wave(0.5f);
    insert(RealtimeEffectList::Get(*track), 0.0);
    render({ track });
    ASSERT_GT(effect.records.size(), 2u);
    for (const auto& record : effect.records) {
        EXPECT_DOUBLE_EQ(record.context.tempo, 87.0);
        EXPECT_EQ(record.context.numerator, 5);
        EXPECT_EQ(record.context.denominator, 8);
        EXPECT_DOUBLE_EQ(record.context.sampleRate, 48000.0);
        EXPECT_TRUE(record.context.playing);
        EXPECT_FALSE(record.context.cycling);
    }
    EXPECT_TRUE(std::any_of(effect.records.begin(), effect.records.end(), [](const auto& record) { return record.context.time > 0.0; }));
}

TEST_F(NativeAudioRenderTests, ExportKeepsItsMusicalContextWhenTheDocumentMeterChanges)
{
    auto& signature = ProjectTimeSignature::Get(project());
    signature.SetTempo(120);
    signature.SetUpperTimeSignature(4);
    signature.SetLowerTimeSignature(4);
    TransportToneControls effect;
    const RealtimeEffectState::EffectFactory::Scope factory([&](const PluginID&) -> const EffectInstanceFactory* { return &effect; });
    const auto track = wave(0.5f);
    insert(RealtimeEffectList::Get(*track), 0.0);
    auto source = AudioRenderSource::Create(project(), { track }, options);
    signature.SetTempo(87);
    signature.SetUpperTimeSignature(5);
    signature.SetLowerTimeSignature(8);
    while (const auto count = source->Process()) {
        const auto* samples = reinterpret_cast<const float*>(source->GetBuffer(0));
        for (size_t i = 0; i < count; ++i) {
            EXPECT_FLOAT_EQ(samples[i], 0.5f);
        }
    }
    ASSERT_FALSE(effect.records.empty());
    for (const auto& record : effect.records) {
        EXPECT_DOUBLE_EQ(record.context.tempo, 120);
        EXPECT_EQ(record.context.numerator, 4);
        EXPECT_EQ(record.context.denominator, 4);
    }
}

TEST_F(NativeAudioRenderTests, StoppedInstrumentMonitoringReportsTheCursorAndStoppedTransportToEffects)
{
    auto& signature = ProjectTimeSignature::Get(project());
    signature.SetTempo(111.0);
    signature.SetUpperTimeSignature(3);
    signature.SetLowerTimeSignature(4);
    TransportToneControls effect;
    const RealtimeEffectState::EffectFactory::Scope factory([&](const PluginID&) -> const EffectInstanceFactory* { return &effect; });
    const auto track = MidiTrack::Create();
    TrackList::Get(project()).Add(track);
    insert(RealtimeEffectList::Get(*track), 0.0);
    auto session = std::make_shared<LiveInstrumentSession>(track, createBuiltinInstrument({}));
    NativeMonitorSource source(project().shared_from_this(), track, session, 1.25);
    source.Prepare(48000.0);
    effect.records.clear();
    session->sendMidi(0x90, 60, 127);
    std::array<float, 1024> output;
    source.Render(output.data(), 2, 512);
    ASSERT_FALSE(effect.records.empty());
    for (const auto& record : effect.records) {
        EXPECT_FALSE(record.context.playing);
        EXPECT_DOUBLE_EQ(record.context.speed, 0.0);
        EXPECT_DOUBLE_EQ(record.context.time, 1.25);
        EXPECT_DOUBLE_EQ(record.context.tempo, 111.0);
        EXPECT_EQ(record.context.numerator, 3);
        EXPECT_EQ(record.context.denominator, 4);
        EXPECT_FALSE(record.context.cycling);
    }
}

TEST_F(NativeAudioRenderTests, StoppedInstrumentAndEffectsShareTheCursorWhileRenderTimeAdvancesAndRefreshKeepsVoices)
{
    auto& signature = ProjectTimeSignature::Get(project());
    signature.SetTempo(111.0);
    signature.SetUpperTimeSignature(3);
    TransportToneControls effect;
    const RealtimeEffectState::EffectFactory::Scope factory([&](const PluginID&) -> const EffectInstanceFactory* { return &effect; });
    const auto track = MidiTrack::Create();
    TrackList::Get(project()).Add(track);
    const auto state = insert(RealtimeEffectList::Get(*track), 0.0);
    auto processor = std::make_shared<MonitorProbeInstrument>();
    auto session = std::make_shared<LiveInstrumentSession>(track, processor);
    NativeMonitorSource source(project().shared_from_this(), track, session, 1.25);
    source.Prepare(48000.0);
    const auto editor = state->PeekInstance();
    const auto resets = processor->resets;
    session->sendMidi(0x90, 60, 127);
    std::vector<float> output(777 * 2);
    source.Render(output.data(), 2, 777);
    ASSERT_EQ(processor->contexts.size(), 2u);
    EXPECT_EQ(processor->contexts[0].renderSample, 0);
    EXPECT_EQ(processor->contexts[1].renderSample, 512);
    for (const auto& context : processor->contexts) {
        EXPECT_EQ(context.sample, 60000);
        EXPECT_DOUBLE_EQ(context.tempo, 111.0);
        EXPECT_EQ(context.numerator, 3);
        EXPECT_FALSE(context.playing);
        EXPECT_FALSE(context.continuousSample.has_value());
    }
    track->SetTempo(77.0); // The callback must use the main-thread project snapshot.
    signature.SetTempo(87.0);
    signature.SetUpperTimeSignature(5);
    signature.SetLowerTimeSignature(8);
    ASSERT_TRUE(source.NeedsRefresh(2.0));
    ASSERT_TRUE(source.Refresh(2.0));
    EXPECT_FALSE(source.NeedsRefresh(2.0));
    EXPECT_EQ(state->PeekInstance(), editor);
    effect.records.clear();
    source.Render(output.data(), 2, 777);
    EXPECT_EQ(processor->prepares, 1);
    EXPECT_EQ(processor->resets, resets);
    EXPECT_TRUE(processor->held);
    EXPECT_EQ(processor->contexts[2].renderSample, 777);
    for (size_t i = 2; i < processor->contexts.size(); ++i) {
        const auto& context = processor->contexts[i];
        EXPECT_EQ(context.sample, 96000);
        EXPECT_DOUBLE_EQ(context.tempo, 87.0);
        EXPECT_EQ(context.numerator, 5);
        EXPECT_EQ(context.denominator, 8);
    }
    ASSERT_FALSE(effect.records.empty());
    for (const auto& record : effect.records) {
        EXPECT_DOUBLE_EQ(record.context.time, 2.0);
        EXPECT_DOUBLE_EQ(record.context.tempo, 87.0);
        EXPECT_EQ(record.context.numerator, 5);
        EXPECT_EQ(record.context.denominator, 8);
        EXPECT_FALSE(record.context.playing);
    }
}

TEST_F(NativeAudioRenderTests, StoppedInstrumentAutomationFollowsCursorAndReadBypassRestoresManualSoundWithoutReleasingNotes)
{
    const auto track = MidiTrack::Create();
    TrackList::Get(project()).Add(track);
    auto& automation = TrackAutomation::Get(*track);
    ASSERT_TRUE(automation.SetLane({ "instrument/gain", true, { { 0.0, 0.2, 0.0 }, { 2.0, 0.6, 0.0 } } }));
    auto processor = std::make_shared<MonitorProbeInstrument>();
    auto session = std::make_shared<LiveInstrumentSession>(track, processor);
    NativeMonitorSource source(project().shared_from_this(), track, session, 1.0);
    source.Prepare(48000.0);
    session->sendMidi(0x90, 60, 127);
    std::array<float, 128> output;
    source.Render(output.data(), 2, 64);
    for (auto value : output) {
        EXPECT_NEAR(value, 0.4, 1e-6);
    }
    ASSERT_TRUE(source.NeedsRefresh(2.0));
    ASSERT_TRUE(source.Refresh(2.0));
    source.Render(output.data(), 2, 64);
    for (auto value : output) {
        EXPECT_NEAR(value, 0.6, 1e-6);
    }
    EXPECT_DOUBLE_EQ(session->parameters()[0].value, 0.8);
    ASSERT_TRUE(automation.SetLane({ "instrument/gain", false, { { 0.0, 0.2, 0.0 } } }));
    ASSERT_TRUE(source.NeedsRefresh(2.0));
    ASSERT_TRUE(source.Refresh(2.0));
    source.Render(output.data(), 2, 64);
    for (auto value : output) {
        EXPECT_NEAR(value, 0.8, 1e-6);
    }
    ASSERT_TRUE(session->setParameter("gain", 0.7));
    ASSERT_TRUE(automation.SetLane({ "instrument/gain", true, { { 0.0, 0.2, 0.0 } } }));
    ASSERT_TRUE(source.Refresh(2.0));
    source.Render(output.data(), 2, 64);
    for (auto value : output) {
        EXPECT_NEAR(value, 0.2, 1e-6);
    }
    source.Stop();
    EXPECT_DOUBLE_EQ(processor->gain, 0.7);
    EXPECT_EQ(processor->prepares, 1);
    EXPECT_TRUE(processor->held);
}

TEST_F(NativeAudioRenderTests, StoppedInstrumentLatencyRefreshIsBoundedAndDoesNotPrepareOrResetTheInputSession)
{
    const auto track = MidiTrack::Create();
    TrackList::Get(project()).Add(track);
    const auto processor = std::make_shared<MonitorProbeInstrument>();
    const auto session = std::make_shared<LiveInstrumentSession>(track, processor);
    NativeMonitorSource source(project().shared_from_this(), track, session, 0.0);
    source.Prepare(48000);
    session->sendMidi(0x90, 60, 127);
    std::array<float, 128> output;
    source.Render(output.data(), 2, 64);
    const auto resets = processor->resets;
    processor->pending = true;
    EXPECT_TRUE(source.NeedsRefresh(0.0));
    ASSERT_TRUE(source.Refresh(0.0));
    EXPECT_EQ(processor->refreshes, 1u);
    EXPECT_FALSE(source.NeedsRefresh(0.0));
    source.Render(output.data(), 2, 64);
    EXPECT_TRUE(processor->held);
    EXPECT_EQ(processor->prepares, 1);
    EXPECT_EQ(processor->resets, resets);
    for (const auto sample : output) {
        EXPECT_FLOAT_EQ(sample, 0.8f);
    }
    processor->pending = processor->unstable = true;
    EXPECT_FALSE(source.Refresh(0.0));
    EXPECT_EQ(processor->refreshes, 9u); // One successful request, then a bounded eight attempts.
    EXPECT_FALSE(source.ready());
    EXPECT_FALSE(source.Refresh(0.0)); // A failed source cannot silently retry.
    EXPECT_EQ(processor->refreshes, 9u);
    source.Render(output.data(), 2, 64);
    for (const auto sample : output) {
        EXPECT_FLOAT_EQ(sample, 0.0f);
    }
}

TEST_F(NativeAudioRenderTests, FailedMonitoringRefreshRequiresFreshPreparationAndRetainsQueuedKeys)
{
    const auto track = MidiTrack::Create();
    TrackList::Get(project()).Add(track);
    const auto processor = std::make_shared<MonitorProbeInstrument>();
    const auto session = std::make_shared<LiveInstrumentSession>(track, processor);
    NativeMonitorSource source(project().shared_from_this(), track, session, 0.0);
    source.Prepare(48000);
    std::array<float, 128> output;
    processor->pending = processor->failRefresh = true;
    EXPECT_TRUE(source.NeedsRefresh(0.0));
    EXPECT_FALSE(source.Refresh(0.0));
    EXPECT_FALSE(source.ready());
    session->sendMidi(0x90, 67, 127);
    source.Stop();
    session->reportProcessingFailure("Cannot refresh native instrument monitoring.");
    EXPECT_EQ(session->takeMidiInputError(), "Cannot refresh native instrument monitoring.");
    EXPECT_TRUE(session->takeMidiInputError().empty());
    processor->failRefresh = processor->pending = false;
    source.Prepare(48000); // Same rate/block size still needs real native preparation.
    ASSERT_TRUE(source.ready());
    EXPECT_EQ(processor->prepares, 2);
    source.Render(output.data(), 2, 64);
    for (const auto sample : output) {
        EXPECT_FLOAT_EQ(sample, 0.8f);
    }
}

TEST_F(NativeAudioRenderTests, FailedMonitoringProcessingRemainsSilentUntilAnExplicitFreshPreparation)
{
    const auto track = MidiTrack::Create();
    TrackList::Get(project()).Add(track);
    const auto processor = std::make_shared<MonitorProbeInstrument>();
    const auto session = std::make_shared<LiveInstrumentSession>(track, processor);
    NativeMonitorSource source(project().shared_from_this(), track, session, 0.0);
    source.Prepare(48000);
    session->sendMidi(0x90, 60, 127);
    std::array<float, 128> output;
    source.Render(output.data(), 2, 64);
    processor->failProcess = true;
    source.Render(output.data(), 2, 64);
    EXPECT_FALSE(source.ready());
    for (const auto sample : output) {
        EXPECT_FLOAT_EQ(sample, 0.0f);
    }
    processor->failProcess = false;
    EXPECT_FALSE(source.Refresh(0.0));
    EXPECT_FALSE(source.RefreshRealtimeEffects()); // An effect edit must not republish failed native DSP.
    source.Render(output.data(), 2, 64);
    for (const auto sample : output) {
        EXPECT_FLOAT_EQ(sample, 0.0f);
    }
    source.Stop();
    session->reportProcessingFailure("Cannot refresh native instrument monitoring.");
    source.Prepare(48000);
    source.Render(output.data(), 2, 64);
    EXPECT_EQ(processor->prepares, 2);
    for (const auto sample : output) {
        EXPECT_FLOAT_EQ(sample, 0.8f);                              // Previously held input is chased once.
    }
}

TEST_F(NativeAudioRenderTests, MidiSequenceKeepsItsUnloopedProcessingClockWhenProjectPositionWraps)
{
    auto track = MidiTrack::Create();
    auto processor = std::make_shared<MonitorProbeInstrument>();
    MidiPlayableSequence sequence(track, processor, 48000.0);
    std::array<float, 64> left {}, right {};
    float* output[] { left.data(), right.data() };
    ASSERT_TRUE(sequence.GetFloats(0, 2, output, 60000, 64));
    ASSERT_TRUE(sequence.GetFloats(0, 2, output, 60064, 64));
    ASSERT_TRUE(sequence.GetFloats(0, 2, output, 24000, 64));
    // The two discarded latency probes must not advance the audible clock.
    ASSERT_EQ(processor->contexts.size(), 5u);
    EXPECT_EQ(processor->contexts[0].sample, 60000);
    EXPECT_EQ(processor->contexts[0].continuousSample, 60000);
    EXPECT_EQ(processor->contexts[1].sample, 60000);
    EXPECT_EQ(processor->contexts[1].continuousSample, 60000);
    EXPECT_EQ(processor->contexts[2].sample, 60064);
    EXPECT_EQ(processor->contexts[2].continuousSample, 60064);
    EXPECT_EQ(processor->contexts[3].sample, 24000);
    EXPECT_EQ(processor->contexts[3].continuousSample, 60128);
    EXPECT_EQ(processor->contexts[4].sample, 24000);
    EXPECT_EQ(processor->contexts[4].continuousSample, 60128);
}

TEST_F(NativeAudioRenderTests, PlaybackPolicySampleClockMatchesRepeatedTwoSampleLoops)
{
    AudioIOStartStreamOptions streamOptions(project().shared_from_this(), 48000.0);
    streamOptions.policyFactory = [&](const AudioIOStartStreamOptions&) {
        return std::make_unique<DefaultPlaybackPolicy>(project(), 1.0, 2.0 / 48000.0, 0.0, true, false);
    };
    PlaybackSchedule schedule;
    schedule.Init(0.0, 2.0 / 48000.0, streamOptions, nullptr);
    auto& policy = schedule.GetPolicy();
    policy.Initialize(schedule, 48000.0);
    schedule.mTimeQueue.Init(64);
    schedule.mTimeQueue.Prime(0.0);
    std::vector<double> times(8, -1.0);
    const PlaybackPolicy::Mixers mixers;
    for (int loop = 0; loop < 12; ++loop) {
        EXPECT_TRUE(policy.Looping(schedule));
        EXPECT_EQ(policy.LoopBounds(schedule), (std::pair { 0.0, 2.0 / 48000.0 }));
        const auto slice = policy.GetPlaybackSlice(schedule, 8);
        ASSERT_EQ(slice.frames, 2u) << loop;
        schedule.mTimeQueue.Producer(schedule, slice, times.data());
        EXPECT_NEAR(times[0], 0.0, 1e-12);
        EXPECT_NEAR(times[1], 1.0 / 48000.0, 1e-12);
        EXPECT_NEAR(schedule.mTimeQueue.GetLastTime(), 0.0, 1e-12);
        policy.RepositionPlayback(schedule, mixers, slice.frames, 8 - slice.frames);
    }
    policy.Finalize(schedule);
}

TEST_F(NativeAudioRenderTests, DeferredPlaybackTimesMatchTheOriginalPolicyAcrossPartialGrainsAndTrailingSilence)
{
    AudioIOStartStreamOptions options(project().shared_from_this(), 48000.0);
    PlaybackSchedule original, routed;
    for (auto* schedule : { &original, &routed }) {
        schedule->Init(0.25, 2.0, options, nullptr);
        schedule->GetPolicy().Initialize(*schedule, 48000.0);
        schedule->mTimeQueue.Init(64);
        schedule->mTimeQueue.Prime(0.25);
    }
    const auto finalize = finally([&] {
        original.GetPolicy().Finalize(original);
        routed.GetPolicy().Finalize(routed);
    });
    for (const auto count : { 7u, 511u, 13u, 1600u, 1u, 967u }) {
        const PlaybackSlice slice { count, count, count - 1 };
        std::vector<double> expected(count), actual(count + 1);
        original.mTimeQueue.Producer(original, slice, expected.data());
        routed.mTimeQueue.GenerateTimes(routed, slice, actual);
        for (size_t frame = 0; frame < count; ++frame) {
            EXPECT_NEAR(actual[frame], expected[frame], 1e-12);
        }
        EXPECT_DOUBLE_EQ(actual.back(), original.mTimeQueue.GetLastTime());
        routed.mTimeQueue.PublishTimes(actual);
        for (size_t frame = 0; frame < count; ++frame) {
            EXPECT_NEAR(routed.mTimeQueue.Consumer(1, 48000.0), original.mTimeQueue.Consumer(1, 48000.0), 1e-12);
        }
    }
}

TEST_F(NativeAudioRenderTests, DeferredPlaybackClockPublishesCompensatedTwoSampleLoopsWithoutAdvancingThePolicyTwice)
{
    DelayedToneControls effect;
    const RealtimeEffectState::EffectFactory::Scope factory([&](const PluginID&) -> const EffectInstanceFactory* { return &effect; });
    const auto track = wave(0.0f);
    insert(RealtimeEffectList::Get(*track), 0.0);
    AudioPlaybackProcessor::Sequences sequences { StretchingSequence::Create(*track, track->GetClipInterfaces()) };
    NativePlaybackProcessor processor(project(), sequences, 48000.0, 2, 2);
    RealtimeEffects::InitializationScope initialization(project().shared_from_this(), 48000.0, 2, 512);
    initialization.AddGroup(*track, 1, 48000.0f, 512);
    AudioIOStartStreamOptions options(project().shared_from_this(), 48000.0);
    options.policyFactory = [&](const AudioIOStartStreamOptions&) {
        return std::make_unique<DefaultPlaybackPolicy>(project(), 1.0, 2.0 / 48000.0, 0.0, true, false);
    };
    PlaybackSchedule schedule;
    schedule.Init(0.0, 2.0 / 48000.0, options, nullptr);
    auto& policy = schedule.GetPolicy();
    policy.Initialize(schedule, 48000.0);
    const auto finalize = finally([&] { policy.Finalize(schedule); });
    schedule.mTimeQueue.Init(2);
    schedule.mTimeQueue.Prime(0.0);
    const PlaybackPolicy::Mixers mixers;
    size_t consumed = 0;
    for (int loop = 0; loop < 800; ++loop) {
        const auto slice = policy.GetPlaybackSlice(schedule, 2);
        ASSERT_EQ(slice.frames, 2u);
        std::array<double, 3> times {};
        schedule.mTimeQueue.GenerateTimes(schedule, slice, times);
        EXPECT_NEAR(times[0], 0.0, 1e-12);
        EXPECT_NEAR(times[1], 1.0 / 48000.0, 1e-12);
        EXPECT_NEAR(times[2], 0.0, 1e-12);
        std::array<float, 2> signal { 0.25f, 0.75f };
        const float* inputs[] { signal.data() };
        RealtimeEffects::ProcessingScope scope(initialization, project().shared_from_this());
        const auto discard = processor.Process(scope, inputs, { times.data(), 2 }, times.back());
        if (discard < 2) {
            schedule.mTimeQueue.PublishTimes(processor.OutputTimes().subspan(discard));
        }
        for (size_t frame = discard; frame < 2; ++frame) {
            const auto time = processor.OutputTimes()[frame];
            EXPECT_NEAR(time, consumed % 2 ? 1.0 / 48000.0 : 0.0, 1e-12);
            EXPECT_FLOAT_EQ(processor.Output(0)[frame], consumed % 2 ? 0.75f : 0.25f);
            EXPECT_NEAR(schedule.mTimeQueue.Consumer(1, 48000.0), 0.0, 1e-12);
            ++consumed;
        }
        EXPECT_NEAR(schedule.mTimeQueue.GetLastTime(), 0.0, 1e-12);
        policy.RepositionPlayback(schedule, mixers, slice.frames, 0);
    }
    EXPECT_EQ(consumed, 1597u);
}

TEST_F(NativeAudioRenderTests, PublishedPlaybackTimesSurviveQueueGrowthReuseAndPrimeWithoutPublishingDiscardedInput)
{
    PlaybackSchedule::TimeQueue queue;
    queue.Init(2);
    queue.Prime(0.25);
    for (int pass = 0; pass < 3; ++pass) {
        std::vector<double> boundaries(64 * TimeQueueGrainSize + 1);
        for (size_t frame = 0; frame < boundaries.size(); ++frame) {
            // A different slope in each grain exercises more than a fixed
            // global time offset, including backward source discontinuities.
            boundaries[frame] = pass + (frame / TimeQueueGrainSize % 3) * 0.1 + (frame % TimeQueueGrainSize) / 48000.0;
        }
        queue.PublishTimes(boundaries);
        for (size_t grain = 1; grain <= 64; ++grain) {
            EXPECT_DOUBLE_EQ(queue.Consumer(TimeQueueGrainSize, 48000.0), boundaries[grain * TimeQueueGrainSize]);
        }
    }
    queue.Prime(7.0);
    EXPECT_DOUBLE_EQ(queue.Consumer(0, 48000.0), 7.0);
    queue.PublishTimes({});
    EXPECT_DOUBLE_EQ(queue.Consumer(0, 48000.0), 7.0);
}

TEST_F(NativeAudioRenderTests, OriginalPlaybackClockRetainsEveryGrainWhenOneProducerCallGrowsSeveralNodes)
{
    AudioIOStartStreamOptions options(project().shared_from_this(), 48000.0);
    PlaybackSchedule schedule;
    schedule.Init(0.0, 5.0, options, nullptr);
    auto& policy = schedule.GetPolicy();
    policy.Initialize(schedule, 48000.0);
    const auto finalize = finally([&] { policy.Finalize(schedule); });
    schedule.mTimeQueue.Init(2);
    schedule.mTimeQueue.Prime(0.0);
    for (int pass = 0; pass < 3; ++pass) {
        constexpr size_t frames = 64 * TimeQueueGrainSize;
        schedule.mTimeQueue.Producer(schedule, PlaybackSlice { frames, frames, frames });
        for (size_t grain = 1; grain <= 64; ++grain) {
            EXPECT_NEAR(schedule.mTimeQueue.Consumer(TimeQueueGrainSize, 48000.0),
                        (pass * 64 + grain) * TimeQueueGrainSize / 48000.0, 1e-12);
        }
    }
}

TEST_F(NativeAudioRenderTests, PlaybackPolicyPublishesLoopRegionEditsOnlyWhenTheWorkerConsumesThem)
{
    auto& region = ViewInfo::Get(project()).playRegion;
    region.SetTimes(0.25, 0.75);
    region.SetActive(true);
    AudioIOStartStreamOptions options(project().shared_from_this(), 48000.0);
    options.policyFactory = [&](const AudioIOStartStreamOptions&) {
        return std::make_unique<DefaultPlaybackPolicy>(project(), 4.0, 0.75, 0.25, true, false);
    };
    PlaybackSchedule schedule;
    schedule.Init(0.25, 0.75, options, nullptr);
    auto& policy = schedule.GetPolicy();
    policy.Initialize(schedule, 48000.0);
    schedule.mTimeQueue.Init(64);
    schedule.mTimeQueue.Prime(0.25);
    const PlaybackPolicy::Mixers mixers;
    EXPECT_TRUE(policy.Looping(schedule));
    EXPECT_EQ(policy.LoopBounds(schedule), (std::pair { 0.25, 0.75 }));
    region.SetTimes(0.125, 1.25);
    EXPECT_EQ(policy.LoopBounds(schedule), (std::pair { 0.25, 0.75 }));
    policy.RepositionPlayback(schedule, mixers, 0, 64);
    EXPECT_TRUE(policy.Looping(schedule));
    EXPECT_EQ(policy.LoopBounds(schedule), (std::pair { 0.125, 1.25 }));
    region.SetActive(false);
    EXPECT_TRUE(policy.Looping(schedule));
    policy.RepositionPlayback(schedule, mixers, 0, 64);
    EXPECT_FALSE(policy.Looping(schedule));
    EXPECT_EQ(policy.LoopBounds(schedule), (std::pair { 0.125, 1.25 }));
    EXPECT_DOUBLE_EQ(schedule.mT1, 4.0); // Playback end and saved loop end differ.
    policy.Finalize(schedule);
}

TEST_F(NativeAudioRenderTests, ShortRawEffectOutputSilencesMissingSamplesOnEveryProcessor)
{
    ShortToneControls effect;
    const RealtimeEffectState::EffectFactory::Scope factory([&](const PluginID&) -> const EffectInstanceFactory* { return &effect; });
    const auto track = wave(0.5f);
    insert(RealtimeEffectList::Get(*track), 0.0);
    RealtimeEffects::InitializationScope initialization(project().shared_from_this(), 48000.0, 2, 512);
    initialization.AddGroup(*track, 2, 48000.0f, 512);
    std::vector<float> left(32, 1.0f), right(32, 0.5f), a(32), b(32), dummy(32);
    float* buffers[] { left.data(), right.data() };
    float* scratch[] { a.data(), b.data() };
    {
        RealtimeEffects::ProcessingScope processing(initialization, project().shared_from_this());
        EXPECT_EQ(processing.ProcessRaw(track.get(), buffers, scratch, dummy.data(), 2, 32), 16u);
    }
    for (size_t i = 0; i < 32; ++i) {
        EXPECT_FLOAT_EQ(left[i], 1.0f);
        EXPECT_FLOAT_EQ(right[i], i < 16 ? 0.5f : 0.0f);
    }
}

TEST_F(NativeAudioRenderTests, PlaybackRebuildsCompensationWhenAnInsertIsAddedAndRemoved)
{
    DelayedToneControls effect;
    const RealtimeEffectState::EffectFactory::Scope factory([&](const PluginID&) -> const EffectInstanceFactory* { return &effect; });
    const auto track = wave(1.0f);
    AudioPlaybackProcessor::Sequences sequences { StretchingSequence::Create(*track, track->GetClipInterfaces()) };
    NativePlaybackProcessor processor(project(), sequences, 48000.0, 2, 64);
    RealtimeEffects::InitializationScope initialization(project().shared_from_this(), 48000.0, 2, 512);
    initialization.AddGroup(*track, processor.ProcessingChannels(0), 48000.0f, 512);
    std::vector<float> signal(64, 0.5f);
    const float* inputs[] { signal.data() };
    std::vector<double> times(64);
    auto check = [&](int block, size_t expectedDiscard) {
        for (size_t i = 0; i < 64; ++i) {
            times[i] = (block * 64 + i) / 48000.0;
        }
        RealtimeEffects::ProcessingScope scope(initialization, project().shared_from_this());
        const auto discard = processor.Process(scope, inputs, times);
        EXPECT_EQ(discard, expectedDiscard);
        for (size_t i = discard; i < 64; ++i) {
            EXPECT_NEAR(processor.Output(0)[i], 0.5, 1e-6);
            EXPECT_NEAR(processor.Meter(0, 0)[i], 0.5, 1e-6);
        }
    };
    check(0, 0);
    auto& manager = RealtimeEffectManager::Get(project());
    const auto state = manager.AddState(&initialization, track.get(), "native-test-tone-controls");
    ASSERT_TRUE(state);
    const auto instance = std::dynamic_pointer_cast<DelayedToneControls::DelayedInstance>(state->PeekInstance());
    ASSERT_TRUE(instance);
    EXPECT_EQ(instance->delayed.size(), 1u);
    check(1, 3);
    check(2, 0);
    manager.RemoveState(&initialization, track.get(), state);
    check(3, 0);
}

TEST_F(NativeAudioRenderTests, PlaybackOfWaveAndMidiThroughOriginalMixersMatchesRoutedExport)
{
    const auto audio = wave(0.125f), target = bus("bus");
    const auto midi = MidiTrack::Create();
    ASSERT_TRUE(midi->SetClips({ { 1, "Phrase", 0.0, 1.0, 1, { { 1, 0.0, 1.0, 69, 100 } }, {} } }));
    midi->SetGain(0.6);
    midi->SetPan(-0.25);
    audio->SetVolume(0.7f);
    audio->SetPan(0.5f);
    target->SetVolume(0.5f);
    TrackList::Get(project()).Add(midi);
    TrackRouting::Get(*audio).output = TrackRouting::Get(*midi).output = "bus";
    options.midiTracks = { midi };
    const auto expected = render({ audio });
    AudioPlaybackProcessor::Sequences sequences {
        StretchingSequence::Create(*audio, audio->GetClipInterfaces()),
        StretchingSequence::Create(*target, target->GetClipInterfaces()),
        std::make_shared<MidiPlayableSequence>(midi, createBuiltinInstrument({}), 48000.0)
    };
    const size_t capacity = 73;
    NativePlaybackProcessor processor(project(), sequences, 48000.0, 2, capacity);
    std::vector<std::unique_ptr<Mixer> > mixers;
    for (const auto& sequence : sequences) {
        mixers.push_back(std::make_unique<Mixer>(Mixer::Inputs { { sequence } }, std::nullopt, true,
                                                 Mixer::WarpOptions { &project() }, 0.0, options.stopTime + 0.5 / 48000.0,
                                                 sequence->NChannels(), capacity, false, 48000.0, floatSample, false,
                                                 nullptr, Mixer::ApplyVolume::Discard));
    }
    RealtimeEffects::InitializationScope initialization(project().shared_from_this(), 48000.0, 2, 512);
    for (size_t i = 0; i < sequences.size(); ++i) {
        initialization.AddGroup(*sequences[i]->FindChannelGroup(), processor.ProcessingChannels(i), 48000.0f, 512);
    }
    for (size_t offset = 0; offset < 512; offset += capacity) {
        const auto frames = std::min<size_t>(capacity, 512 - offset);
        std::vector<const float*> inputs;
        for (size_t index = 0; index < mixers.size(); ++index) {
            ASSERT_EQ(mixers[index]->Process(frames), frames);
            for (size_t channel = 0; channel < sequences[index]->NChannels(); ++channel) {
                inputs.push_back(reinterpret_cast<const float*>(mixers[index]->GetBuffer(channel)));
            }
        }
        std::vector<double> times(frames);
        for (size_t frame = 0; frame < frames; ++frame) {
            times[frame] = (offset + frame) / 48000.0;
        }
        RealtimeEffects::ProcessingScope scope(initialization, project().shared_from_this());
        ASSERT_EQ(processor.Process(scope, inputs, times), 0u);
        for (size_t channel = 0; channel < 2; ++channel) {
            for (size_t frame = 0; frame < frames; ++frame) {
                EXPECT_NEAR(processor.Output(channel)[frame], expected[channel][offset + frame], 1e-6);
            }
        }
    }
}

TEST_F(NativeAudioRenderTests, DocumentInstrumentCacheRetainsEditorStateAndInvalidatesUndoSnapshots)
{
    const auto track = MidiTrack::Create();
    TrackList::Get(project()).Add(track);
    auto& cache = NativeInstrumentSessions::Get(project());
    EXPECT_FALSE(cache.find(*track));
    const auto first = cache.get(project(), track);
    EXPECT_EQ(cache.find(*track), first);
    EXPECT_FALSE(first->active());
    first->open();
    first->Prepare(48000.0);
    ASSERT_TRUE(first->setParameter("0", 0.25));
    EXPECT_EQ(cache.get(project(), track), first);
    const auto saved = first->state();
    track->SetInstrument(track->GetInstrumentDescription(), wxString::FromUTF8(saved));
    EXPECT_EQ(cache.get(project(), track), first);
    first->close();
    const auto reopened = cache.get(project(), track);
    EXPECT_EQ(reopened, first);
    EXPECT_NEAR(reopened->parameters()[0].value, 0.25, 1e-12);
    // Undo restores the earlier serialized document, which must not reuse a
    // cached processor containing the newer sound.
    track->SetInstrument(track->GetInstrumentDescription(), {});
    EXPECT_FALSE(cache.find(*track));
    const auto restored = cache.get(project(), track);
    EXPECT_NE(restored, first);
    EXPECT_NEAR(restored->parameters()[0].value, 0.5, 1e-12);
}

TEST_F(NativeAudioRenderTests, LiveKeyboardOnAnEmptyMidiTrackPassesThroughTheOriginalMixerAndBus)
{
    const auto midi = MidiTrack::Create();
    TrackList::Get(project()).Add(midi);
    const auto target = bus("bus");
    target->SetVolume(0.0f);
    TrackRouting::Get(*midi).output = "bus";
    const auto session = NativeInstrumentSessions::Get(project()).get(project(), midi);
    session->open();
    session->beginTimeline();
    AudioPlaybackProcessor::Sequences sequences {
        std::make_shared<MidiPlayableSequence>(midi, session, 48000.0, true, false),
        StretchingSequence::Create(*target, target->GetClipInterfaces())
    };
    NativePlaybackProcessor processor(project(), sequences, 48000.0, 2, 512);
    Mixer mixer({ { sequences[0] } }, std::nullopt, true, Mixer::WarpOptions { &project() },
                0.0, 1.0, 2, 512, false, 48000.0, floatSample, false, nullptr, Mixer::ApplyVolume::Discard);
    RealtimeEffects::InitializationScope initialization(project().shared_from_this(), 48000.0, 2, 512);
    initialization.AddGroup(*midi, 2, 48000.0f, 512);
    initialization.AddGroup(*target, 2, 48000.0f, 512);
    session->sendMidi(0x90, 69, 100);
    std::vector<float> zero(512);
    std::vector<double> times(512);
    for (int block = 0; block < 2; ++block) {
        if (block) {
            target->SetVolume(0.5f);
        }
        ASSERT_EQ(mixer.Process(512), 512u);
        const float* inputs[] { reinterpret_cast<const float*>(mixer.GetBuffer(0)),
                                reinterpret_cast<const float*>(mixer.GetBuffer(1)), zero.data(), zero.data() };
        ASSERT_TRUE(std::any_of(inputs[0], inputs[0] + 512, [](float value) { return std::abs(value) > 0.001f; }));
        for (size_t i = 0; i < 512; ++i) {
            times[i] = (block * 512 + i) / 48000.0;
        }
        RealtimeEffects::ProcessingScope scope(initialization, project().shared_from_this());
        ASSERT_EQ(processor.Process(scope, inputs, times), 0u);
        for (size_t channel = 0; channel < 2; ++channel) {
            for (size_t i = 0; i < 512; ++i) {
                EXPECT_NEAR(processor.Output(channel)[i], inputs[channel][i] * (block ? 0.5 : 0.0), 1e-7);
                EXPECT_NEAR(processor.Meter(1, channel)[i], processor.Output(channel)[i], 1e-7);
            }
        }
    }
    session->endTimeline();
}

#ifdef AU_NATIVE_TEST_INSTRUMENT_PATH
TEST_F(NativeAudioRenderTests, NativeMidiPlaybackFollowsMeterEditsWhileOfflineRenderingKeepsItsSnapshot)
{
    auto& signature = ProjectTimeSignature::Get(project());
    signature.SetTempo(120);
    signature.SetUpperTimeSignature(4);
    signature.SetLowerTimeSignature(4);
    const auto track = MidiTrack::Create();
    track->SetTempo(120);
    // The fixture's diagnostic note emits transport values, never sent to a device.
    ASSERT_TRUE(track->SetClips({ { 1, "Context", 0, 4, 1, { { 1, 0, 4, 127, 127 } }, {} } }));
    TrackList::Get(project()).Add(track);
    const auto path = std::string(AU_NATIVE_TEST_INSTRUMENT_PATH) + ";F5A81297A6724B218E434FA1A4050C01";
    wxArrayString description;
    for (const auto* part : { "Effect", "VST3", "Audacity", "Native fixture" }) {
        description.Add(part);
    }
    description.Add(wxString::FromUTF8(path));
    track->SetInstrument(wxJoin(description, '_'), {});
    options.midiTracks = { track };
    auto offline = AudioRenderSource::Create(project(), {}, options);
    auto instrument = createVstInstrument(path, {});
    ASSERT_TRUE(instrument);
    MidiPlayableSequence sequence(track, instrument, 48000, true, true, 4, 4, 0, &NativeTimeSignature::Get(project()));
    sequence.SetPlaybackLoop(true, 0, 2);
    std::array<float, 64> left {}, right {};
    float* output[] { left.data(), right.data() };
    const auto process = [&] {
        std::thread audio([&] { EXPECT_TRUE(sequence.GetFloats(0, 2, output, 0, left.size())); });
        audio.join();
    };
    process();
    EXPECT_FLOAT_EQ(left[0], 120);
    EXPECT_FLOAT_EQ(left[1], 4);
    EXPECT_FLOAT_EQ(left[2], 4);
    signature.SetUpperTimeSignature(7);
    signature.SetLowerTimeSignature(8);
    signature.SetTempo(150);
    process(); // The loop restarts its MIDI events and observes the newly published meter.
    EXPECT_FLOAT_EQ(left[0], 120);
    EXPECT_FLOAT_EQ(left[1], 7);
    EXPECT_FLOAT_EQ(left[2], 8);
    EXPECT_FLOAT_EQ(left[7], 1);
    EXPECT_FLOAT_EQ(left[8], 1);
    ASSERT_GE(offline->Process(), 12u);
    const auto* samples = reinterpret_cast<const float*>(offline->GetBuffer(0));
    EXPECT_FLOAT_EQ(samples[0], 120);
    EXPECT_FLOAT_EQ(samples[1], 4);
    EXPECT_FLOAT_EQ(samples[2], 4);
    EXPECT_FLOAT_EQ(samples[7], 1);
    EXPECT_FLOAT_EQ(samples[8], 0);
}

TEST_F(NativeAudioRenderTests, StoppedNativeRestartPreservesEffectHistoryPedalVoicesAndQueuedKeysWithoutChangingTheEditor)
{
    DelayedToneControls effect;
    const RealtimeEffectState::EffectFactory::Scope factory([&](const PluginID&) -> const EffectInstanceFactory* { return &effect; });
    const auto track = MidiTrack::Create();
    TrackList::Get(project()).Add(track);
    const auto target = bus("bus");
    TrackRouting::Get(*track).output = "bus";
    insert(RealtimeEffectList::Get(*track), 0.0);
    const auto state = insert(RealtimeEffectList::Get(*target), 0.0);
    insert(RealtimeEffectList::Get(project()), 0.0);
    const auto processor = createVstInstrument(std::string(AU_NATIVE_TEST_INSTRUMENT_PATH)
                                               + ";F5A81297A6724B218E434FA1A4050C04", R"({"parameters":{"0":0.6}})");
    const auto session = std::make_shared<LiveInstrumentSession>(track, processor);
    const auto source = std::make_shared<NativeMonitorSource>(project().shared_from_this(), track, session, 0.0);
    source->Prepare(48000);
    const auto editor = state->PeekInstance();
    MonitorCallback callback;
    callback.SetLiveOutput(source);
    std::array<float, 128> output {};
    const auto render = [&] {
        output.fill(0.0f);
        std::thread audio([&] { callback.render(output.data(), 64); });
        audio.join();
    };
    session->sendMidi(0xb0, 64, 127);
    session->sendMidi(0x90, 60, 32);
    session->sendMidi(0x80, 60, 0);
    session->sendMidi(0x90, 64, 64);
    render();
    ASSERT_NEAR(output.back(), 0.6 * 96.0 / 127, 1e-6);
    // Initial processing controls request a measurement even when native delay
    // remains zero. Service it without rebuilding the effect history.
    ASSERT_TRUE(source->NeedsRefresh(0.0));
    callback.SetLiveOutput({});
    ASSERT_TRUE(source->Refresh(0.0));
    EXPECT_EQ(session->latency(), 0u);
    EXPECT_FALSE(source->NeedsRefresh(0.0));
    callback.SetLiveOutput(source);
    ASSERT_TRUE(session->setParameter("2", 7.0 / 31));
    ASSERT_TRUE(source->NeedsRefresh(0.0));
    session->sendMidi(0x90, 67, 32); // Stays queued while the main thread reactivates native DSP.
    callback.SetLiveOutput({});
    ASSERT_TRUE(source->Refresh(0.0));
    EXPECT_FALSE(source->NeedsRefresh(0.0));
    EXPECT_EQ(session->latency(), 7u);
    EXPECT_EQ(state->PeekInstance(), editor);
    callback.SetLiveOutput(source);
    render();
    // Preserve the three existing 3-frame effect delay lines. The restarted
    // instrument then contributes its own causal 7-frame delay, with all three
    // voices resumed/received on the first block after ownership is returned.
    for (size_t frame = 0; frame < 64; ++frame) {
        const auto expected = frame < 9 ? 0.6 * 96.0 / 127 : frame < 16 ? 0.0 : 0.6 * 128.0 / 127;
        EXPECT_NEAR(output[2 * frame], expected, 1e-6) << frame;
        EXPECT_NEAR(output[2 * frame + 1], expected, 1e-6) << frame;
    }
    session->sendMidi(0xb0, 64, 0);
    render();
    EXPECT_NEAR(output.back(), 0.6 * 96.0 / 127, 1e-6);
    session->sendMidi(0x80, 64, 0);
    session->sendMidi(0x80, 67, 0);
    render();
    EXPECT_NEAR(output.back(), 0.0, 1e-6);
    callback.StopLiveOutput();
}

TEST_F(NativeAudioRenderTests, NativeResumeFailureCanBeRetriedWithoutConfiguringAnActiveComponentOrLosingQueuedKeys)
{
    QLibrary fixture(QString::fromUtf8(AU_NATIVE_TEST_INSTRUMENT_BINARY));
    ASSERT_TRUE(fixture.load());
    const auto refuse = reinterpret_cast<void (*)(bool)>(fixture.resolve("nativeFixtureRefuseNextResume"));
    ASSERT_NE(refuse, nullptr);
    const auto cleanup = finally([&] { refuse(false); });
    const auto track = MidiTrack::Create();
    TrackList::Get(project()).Add(track);
    const auto processor = createVstInstrument(std::string(AU_NATIVE_TEST_INSTRUMENT_PATH)
                                               + ";F5A81297A6724B218E434FA1A4050C04", {});
    const auto session = std::make_shared<LiveInstrumentSession>(track, processor);
    NativeMonitorSource source(project().shared_from_this(), track, session, 0.0);
    source.Prepare(48000);
    std::array<float, 128> output {};
    session->sendMidi(0x90, 60, 64);
    source.Render(output.data(), 2, 64);
    ASSERT_NEAR(output.back(), 0.5 * 64.0 / 127, 1e-6);
    ASSERT_TRUE(session->setParameter("2", 7.0 / 31));
    session->sendMidi(0x90, 67, 32);
    refuse(true);
    EXPECT_FALSE(source.Refresh(0.0));
    EXPECT_FALSE(source.ready());
    source.Render(output.data(), 2, 64);
    for (const auto sample : output) {
        EXPECT_FLOAT_EQ(sample, 0.0f);
    }
    source.Stop();
    session->reportProcessingFailure("Native resume failed.");
    ASSERT_NO_THROW(source.Prepare(48000));
    ASSERT_TRUE(source.ready());
    source.Render(output.data(), 2, 64);
    for (size_t frame = 0; frame < 64; ++frame) {
        const auto expected = frame < 7 ? 0.0 : 0.5 * 96.0 / 127;
        EXPECT_NEAR(output[frame * 2], expected, 1e-6) << frame;
        EXPECT_NEAR(output[frame * 2 + 1], expected, 1e-6) << frame;
    }
}

TEST_F(NativeAudioRenderTests, NativeInitialResumeFailureDeactivatesTheComponentBeforeAnExplicitPreparationRetry)
{
    QLibrary fixture(QString::fromUtf8(AU_NATIVE_TEST_INSTRUMENT_BINARY));
    ASSERT_TRUE(fixture.load());
    const auto refuse = reinterpret_cast<void (*)(bool)>(fixture.resolve("nativeFixtureRefuseNextResume"));
    ASSERT_NE(refuse, nullptr);
    const auto cleanup = finally([&] { refuse(false); });
    const auto track = MidiTrack::Create();
    TrackList::Get(project()).Add(track);
    const auto processor = createVstInstrument(std::string(AU_NATIVE_TEST_INSTRUMENT_PATH)
                                               + ";F5A81297A6724B218E434FA1A4050C04", {});
    const auto session = std::make_shared<LiveInstrumentSession>(track, processor);
    NativeMonitorSource source(project().shared_from_this(), track, session, 0.0);
    session->sendMidi(0x90, 60, 64);
    refuse(true);
    EXPECT_THROW(source.Prepare(48000), std::runtime_error);
    EXPECT_FALSE(source.ready());
    session->reportProcessingFailure("Native initial resume failed.");
    ASSERT_NO_THROW(source.Prepare(48000));
    ASSERT_TRUE(source.ready());
    std::array<float, 128> output;
    source.Render(output.data(), 2, 64);
    for (const auto sample : output) {
        EXPECT_NEAR(sample, 0.5 * 64.0 / 127, 1e-6);
    }
}

TEST_F(NativeAudioRenderTests, RunningEngineServicesInstrumentRestartsOnMainAndStopsARefusedRestartWithoutAutoMonitoring)
{
    if (qEnvironmentVariableIsEmpty("AU_NATIVE_TEST_AUDIO_HOST")) {
        GTEST_SKIP() << "Opt-in output-only transport test";
    }
    const auto previousOutput = AudioIOPlaybackDevice.Read();
    const auto previousHost = AudioIOHost.Read();
    const auto restoreOutput = finally([&] {
        AudioIOPlaybackDevice.Write(previousOutput);
        AudioIOHost.Write(previousHost);
    });
    if (const auto name = qgetenv("AU_NATIVE_TEST_AUDIO_OUTPUT_DEVICE"); !name.isEmpty()) {
        int selected = paNoDevice;
        for (int index = 0; index < Pa_GetDeviceCount(); ++index) {
            const auto* info = Pa_GetDeviceInfo(index);
            if (info && info->maxOutputChannels >= 2 && name == info->name) {
                ASSERT_EQ(selected, paNoDevice) << "Output device name must be unique";
                selected = index;
            }
        }
        ASSERT_NE(selected, paNoDevice) << name.constData();
        const auto* host = Pa_GetHostApiInfo(Pa_GetDeviceInfo(selected)->hostApi);
        ASSERT_NE(host, nullptr);
        AudioIOHost.Write(wxString::FromUTF8(host->name));
        AudioIOPlaybackDevice.Write(wxString::FromUTF8(name.constData()));
    }
    const auto config = std::make_shared<testing::NiceMock<record::RecordConfigurationMock> >();
    muse::modularity::globalIoc()->registerExport<record::IRecordConfiguration>("native-transport-test", config);
    const auto unregister
        = finally([] { muse::modularity::globalIoc()->unregister<record::IRecordConfiguration>("native-transport-test"); });
    Au3AudioEngine engine;
    engine.init();
    bool initialized = true;
    const auto cleanup = finally([&] {
        if (initialized) {
            engine.deinit();
        }
    });
    registerNativePlaybackProcessor();
    const auto track = MidiTrack::Create();
    track->SetGain(0); // Exercise the physical output stream without audible test tones.
    ASSERT_TRUE(track->SetClips({ { 1, "Native transport", 0, 16, 1, { { 1, 0, 16, 60, 64 } }, {} } }));
    wxArrayString description;
    for (const auto* part : { "Effect", "VST3", "Audacity", "Native Restart Instrument Fixture" }) {
        description.Add(part);
    }
    description.Add(wxString::FromUTF8(std::string(AU_NATIVE_TEST_INSTRUMENT_PATH) + ";F5A81297A6724B218E434FA1A4050C04"));
    track->SetInstrument(wxJoin(description, '_'), {});
    TrackList::Get(project()).Add(track);
    TransportSequences sequences;
    sequences.otherPlayableSequences.push_back(track);
    audio::IAudioEngine::StartStreamOptions start;
    start.sampleRate = 48000;
    start.isDefaultPolicy = false;
    ASSERT_GT(engine.startStream(sequences, 0, 8, 8, project(), start), 0) << engine.lastErrorString().toStdString();
    const auto opened = engine.openInstrument(project(), *track);
    ASSERT_TRUE(opened.ret) << opened.ret.toString();
    const auto session = std::dynamic_pointer_cast<LiveInstrumentSession>(opened.val);
    ASSERT_TRUE(session);
    QLibrary fixture(QString::fromUtf8(AU_NATIVE_TEST_INSTRUMENT_BINARY));
    ASSERT_TRUE(fixture.load());
    const auto refuse = reinterpret_cast<void (*)(bool)>(fixture.resolve("nativeFixtureRefuseNextResume"));
    const auto offMain = reinterpret_cast<uint64_t (*)()>(fixture.resolve("nativeFixtureOffMainLatencyQueries"));
    ASSERT_TRUE(refuse && offMain);
    const auto resetFailure = finally([&] { refuse(false); });
    const auto before = offMain();
    const auto wait = [](const auto& predicate, std::chrono::milliseconds timeout = std::chrono::seconds(2)) {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (!predicate() && std::chrono::steady_clock::now() < deadline) {
            QCoreApplication::processEvents();
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return predicate();
    };
    for (const auto delay : { 7u, 31u, 3u, 0u }) {
        ASSERT_TRUE(session->setParameter("2", delay / 31.0));
        ASSERT_TRUE(wait([&] { return session->latency() == delay && !session->latencyUpdatePending(); }));
        EXPECT_TRUE(engine.isStreamActive(project()));
    }
    refuse(true);
    session->sendMidi(0x90, 67, 32);
    ASSERT_TRUE(session->setParameter("2", 11.0 / 31));
    ASSERT_TRUE(wait([&] { return !engine.isBusy(); }));
    EXPECT_NE(engine.lastErrorString().toStdString().find("could not resume"), std::string::npos);
    const auto stopped = std::chrono::steady_clock::now();
    while (std::chrono::steady_clock::now() - stopped < std::chrono::milliseconds(150)) {
        QCoreApplication::processEvents();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    EXPECT_FALSE(engine.isBusy());
    EXPECT_FALSE(engine.isMonitoring());
    std::array<float, 64> gated;
    float* output[] { gated.data() };
    session->process(output, 1, gated.size(), {}, {});
    for (const auto sample : gated) {
        EXPECT_FLOAT_EQ(sample, 0.0f);
    }
    // A user-requested transport start performs real preparation after failure.
    ASSERT_GT(engine.startStream(sequences, 0, 8, 8, project(), start), 0) << engine.lastErrorString().toStdString();
    EXPECT_TRUE(engine.isStreamActive(project()));
    EXPECT_EQ(engine.lastErrorString().toStdString().find("could not resume"), std::string::npos);
    EXPECT_EQ(offMain(), before);
    engine.stopStream();
    engine.closeInstrument(session);
    engine.deinit();
    initialized = false;
    // Stop queues idle-time recording cleanup even for output-only streams.
    // It must not retain a raw AudioIO pointer after engine shutdown.
    muse::async::processMessages();
    QCoreApplication::processEvents();
}

TEST_F(NativeAudioRenderTests, StoppedMonitoringUsesTrackBusMasterEffectsAndTheExistingEditorInstances)
{
    ToneControls effect;
    const RealtimeEffectState::EffectFactory::Scope factory([&](const PluginID&) -> const EffectInstanceFactory* { return &effect; });
    const auto track = MidiTrack::Create();
    TrackList::Get(project()).Add(track);
    const auto target = bus("bus");
    TrackRouting::Get(*track).output = "bus";
    track->SetGain(0.8);
    track->SetPan(0.25);
    target->SetVolume(0.5f);
    insert(RealtimeEffectList::Get(*track), -6.0);
    const auto busEffect = insert(RealtimeEffectList::Get(*target), -6.0);
    insert(RealtimeEffectList::Get(project()), -6.0);
    const auto editorInstance = busEffect->GetInstance();
    const auto session = nativeMonitorInstrument(track);
    const auto meter = std::make_shared<MonitorCapture>();
    NativeMonitorSource source(project().shared_from_this(), track, session, 0.0, meter);
    source.Prepare(48000.0);
    ASSERT_TRUE(source.ready());
    EXPECT_EQ(busEffect->PeekInstance(), editorInstance);
    session->sendMidi(0x90, 60, 127);
    std::vector<float> output(777 * 2);
    source.Render(output.data(), 2, 777);
    const auto gain = std::pow(10.0, -6.0 / 20.0);
    for (size_t frame = 0; frame < 777; ++frame) {
        EXPECT_NEAR(output[frame * 2], 0.5 * 0.8 * 0.75 * 0.5 * gain * gain * gain, 1e-6);
        EXPECT_NEAR(output[frame * 2 + 1], 0.5 * 0.8 * 0.5 * gain * gain * gain, 1e-6);
    }
    ASSERT_EQ(meter->entries.size(), 8u);
    for (const auto& entry : meter->entries) {
        const bool busMeter = entry.track == static_cast<int64_t>(target->GetId());
        EXPECT_TRUE(busMeter || entry.track == static_cast<int64_t>(track->GetId()));
        const auto expected = 0.5 * 0.8 * (entry.channel == 0 ? 0.75 : 1.0) * gain * (busMeter ? 0.5 * gain : 1.0);
        for (const auto sample : entry.samples) {
            EXPECT_NEAR(sample, expected, 1e-6);
        }
    }
    EXPECT_NEAR(std::chrono::duration<double>(meter->entries[4].time - meter->entries[0].time).count(), 512.0 / 48000.0, 1e-6);
    source.Stop();
    EXPECT_FALSE(source.ready());
    EXPECT_EQ(source.RealtimeInitialization(), nullptr);
    EXPECT_FALSE(RealtimeEffectManager::Get(project()).IsActive());
    EXPECT_EQ(busEffect->PeekInstance(), editorInstance);
    source.Render(output.data(), 2, 777);
    EXPECT_TRUE(std::all_of(output.begin(), output.end(), [](float value) { return value == 0.0f; }));
}

TEST_F(NativeAudioRenderTests, StoppedAutomationRefreshAndBypassKeepTheNativeMidiSustainController)
{
    auto track = MidiTrack::Create();
    TrackList::Get(project()).Add(track);
    auto& automation = TrackAutomation::Get(*track);
    ASSERT_TRUE(automation.SetLane({ "instrument/0", true, { { 0.0, 0.2, 0.0 } } }));
    const auto session = nativeMonitorInstrument(track);
    NativeMonitorSource source(project().shared_from_this(), track, session, 0.0);
    source.Prepare(48000.0);
    session->sendMidi(0xb0, 64, 127);
    session->sendMidi(0x90, 60, 127);
    session->sendMidi(0x80, 60, 0);
    std::array<float, 128> output {};
    source.Render(output.data(), 2, 64);
    EXPECT_NEAR(output[100], 0.2, 1e-6);
    ASSERT_TRUE(source.Refresh(1.0));
    source.Render(output.data(), 2, 64);
    EXPECT_NEAR(output[100], 0.2, 1e-6);
    ASSERT_TRUE(automation.SetLane({ "instrument/0", false, { { 0.0, 0.2, 0.0 } } }));
    ASSERT_TRUE(source.Refresh(1.0));
    source.Render(output.data(), 2, 64);
    EXPECT_NEAR(output[100], 0.5, 1e-6);
    session->sendMidi(0xb0, 64, 0);
    source.Render(output.data(), 2, 64);
    EXPECT_NEAR(output[100], 0.0, 1e-6);
}

TEST_F(NativeAudioRenderTests, MonitorReconfigurationIsRequestedByTheCallbackAndCompletedOnMain)
{
    ToneControls effect;
    const RealtimeEffectState::EffectFactory::Scope factory([&](const PluginID&) -> const EffectInstanceFactory* { return &effect; });
    const auto track = MidiTrack::Create();
    TrackList::Get(project()).Add(track);
    const auto unused = wave(0.0f);
    auto& effects = RealtimeEffectList::Get(*track);
    insert(effects, -6.0);
    insert(effects, 0.0);
    const auto session = nativeMonitorInstrument(track);
    NativeMonitorSource source(project().shared_from_this(), track, session, 0.0);
    source.Prepare(48000.0);
    session->sendMidi(0x90, 60, 127);
    std::array<float, 1024> output {};
    source.Render(output.data(), 2, 512);
    EXPECT_GT(output[100], 0.0f);
    effects.MoveEffect(0, 1);
    source.Render(output.data(), 2, 512);
    EXPECT_TRUE(source.NeedsRebuild());
    EXPECT_TRUE(std::all_of(output.begin(), output.end(), [](float value) { return value == 0.0f; }));
    ASSERT_TRUE(source.RefreshRealtimeEffects());
    EXPECT_FALSE(source.NeedsRebuild());
    source.Render(output.data(), 2, 512);
    EXPECT_NEAR(output[100], 0.5 * std::pow(10.0, -6.0 / 20.0), 1e-6);
    auto& manager = RealtimeEffectManager::Get(project());
    const auto added = manager.AddState(source.RealtimeInitialization(), track.get(), "native-test-tone-controls");
    ASSERT_TRUE(added);
    ASSERT_TRUE(source.RefreshRealtimeEffects());
    source.Render(output.data(), 2, 512);
    EXPECT_NEAR(output[100], 0.5 * std::pow(10.0, -6.0 / 20.0), 1e-6);
    manager.RemoveState(source.RealtimeInitialization(), track.get(), added);
    ASSERT_TRUE(source.RefreshRealtimeEffects());
    const auto inactive = manager.AddState(source.RealtimeInitialization(), unused.get(), "native-test-tone-controls");
    ASSERT_TRUE(inactive);
    EXPECT_FALSE(inactive->PeekInstance());
    source.Stop();
    EXPECT_FALSE(manager.IsActive());
}

TEST_F(NativeAudioRenderTests, StoppedMonitoringRetainsRealEffectLatencyAndAlignedTrackMeters)
{
    DelayedToneControls effect;
    const RealtimeEffectState::EffectFactory::Scope factory([&](const PluginID&) -> const EffectInstanceFactory* { return &effect; });
    const auto track = MidiTrack::Create();
    TrackList::Get(project()).Add(track);
    const auto target = bus("bus");
    TrackRouting::Get(*track).output = "bus";
    insert(RealtimeEffectList::Get(*track), 0.0);
    insert(RealtimeEffectList::Get(*target), 0.0);
    insert(RealtimeEffectList::Get(project()), 0.0);
    const auto session = nativeMonitorInstrument(track);
    const auto meter = std::make_shared<MonitorCapture>();
    NativeMonitorSource source(project().shared_from_this(), track, session, 0.0, meter);
    source.Prepare(48000.0);
    session->sendMidi(0x90, 60, 127);
    std::array<float, 128> output {};
    source.Render(output.data(), 2, 64);
    for (size_t frame = 0; frame < 64; ++frame) {
        EXPECT_NEAR(output[frame * 2], frame < 9 ? 0.0 : 0.5, 1e-6) << frame;
        EXPECT_NEAR(output[frame * 2 + 1], output[frame * 2], 1e-6);
    }
    ASSERT_EQ(meter->entries.size(), 4u);
    for (const auto& entry : meter->entries) {
        for (size_t frame = 0; frame < 64; ++frame) {
            EXPECT_NEAR(entry.samples[frame], frame < 9 ? 0.0 : 0.5, 1e-6);
        }
    }
    session->releaseNotes();
    source.Render(output.data(), 2, 64);
    for (size_t frame = 0; frame < 64; ++frame) {
        EXPECT_NEAR(output[frame * 2], frame < 9 ? 0.5 : 0.0, 1e-6);
    }
    source.Stop();
}
TEST_F(NativeAudioRenderTests, OriginalEffectScopesAndEditorsSurviveMonitorPlaybackMonitorHandoff)
{
    ToneControls effect;
    const RealtimeEffectState::EffectFactory::Scope factory([&](const PluginID&) -> const EffectInstanceFactory* { return &effect; });
    const auto track = MidiTrack::Create();
    TrackList::Get(project()).Add(track);
    const auto state = insert(RealtimeEffectList::Get(*track), -6.0);
    const auto editor = state->GetInstance();
    const auto session = nativeMonitorInstrument(track);
    const auto monitor = std::make_shared<NativeMonitorSource>(project().shared_from_this(), track, session, 0.0);
    monitor->Prepare(48000.0);
    MonitorCallback callback;
    callback.SetLiveOutput(monitor);
    session->sendMidi(0x90, 60, 127);
    std::array<float, 1024> monitored {};
    callback.render(monitored.data(), 512);
    const auto expected = 0.5 * std::pow(10.0, -6.0 / 20.0);
    EXPECT_NEAR(monitored[100], expected, 1e-6);
    callback.StopLiveOutput();
    EXPECT_FALSE(monitor->ready());
    EXPECT_FALSE(RealtimeEffectManager::Get(project()).IsActive());
    session->beginTimeline();
    {
        AudioPlaybackProcessor::Sequences sequences { std::make_shared<MidiPlayableSequence>(track, session, 48000.0, true, false) };
        NativePlaybackProcessor processor(project(), sequences, 48000.0, 2, 512);
        RealtimeEffects::InitializationScope initialization(project().shared_from_this(), 48000.0, 2, 512);
        initialization.AddGroup(*track, 2, 48000.0f, 512);
        EXPECT_EQ(state->PeekInstance(), editor);
        Mixer mixer({ { sequences[0] } }, std::nullopt, true, Mixer::WarpOptions { &project() }, 0.0, 1.0,
                    2, 512, false, 48000.0, floatSample, false, nullptr, Mixer::ApplyVolume::Discard);
        ASSERT_EQ(mixer.Process(512), 512u);
        const float* inputs[] { reinterpret_cast<const float*>(mixer.GetBuffer(0)), reinterpret_cast<const float*>(mixer.GetBuffer(1)) };
        std::array<double, 512> times {};
        for (size_t i = 0; i < times.size(); ++i) {
            times[i] = i / 48000.0;
        }
        RealtimeEffects::ProcessingScope processing(initialization, project().shared_from_this());
        ASSERT_EQ(processor.Process(processing, inputs, times), 0u);
        EXPECT_NEAR(processor.Output(0)[100], expected, 1e-6);
    }
    session->endTimeline();
    EXPECT_FALSE(RealtimeEffectManager::Get(project()).IsActive());
    monitor->Prepare(48000.0);
    EXPECT_EQ(state->PeekInstance(), editor);
    callback.SetLiveOutput(monitor);
    monitored.fill(0.0f);
    callback.render(monitored.data(), 512);
    EXPECT_NEAR(monitored[100], expected, 1e-6);
    callback.StopLiveOutput();
    EXPECT_FALSE(RealtimeEffectManager::Get(project()).IsActive());
    EXPECT_TRUE(session->active());
}

#endif

TEST_F(NativeAudioRenderTests, AnOpenEffectEditorThatRefusesInitializationIsNeverUsedForAudioProcessing)
{
    RefusingToneControls effect;
    const RealtimeEffectState::EffectFactory::Scope factory([&](const PluginID&) -> const EffectInstanceFactory* { return &effect; });
    const auto track = wave(0.5f);
    const auto state = insert(RealtimeEffectList::Get(*track), 0.0);
    const auto instance = std::dynamic_pointer_cast<RefusingToneControls::RefusingInstance>(state->GetInstance());
    ASSERT_TRUE(instance);
    RealtimeEffects::InitializationScope initialization(project().shared_from_this(), 48000.0, 2, 512);
    initialization.AddGroup(*track, 2, 48000.0f, 512);
    std::array<float, 32> left, right, a {}, b {}, dummy {};
    left.fill(0.5f);
    right.fill(0.25f);
    float* buffers[] { left.data(), right.data() };
    float* scratch[] { a.data(), b.data() };
    {
        RealtimeEffects::ProcessingScope scope(initialization, project().shared_from_this());
        scope.ProbeRaw(track.get(), buffers, scratch, dummy.data(), 2, 32);
        EXPECT_EQ(scope.ProcessRaw(track.get(), buffers, scratch, dummy.data(), 2, 32), 0u);
    }
    EXPECT_EQ(instance->starts, 0);
    EXPECT_EQ(instance->processes, 0);
    EXPECT_EQ(instance->ends, 0);
    for (size_t i = 0; i < 32; ++i) {
        EXPECT_FLOAT_EQ(left[i], 0.5f);
        EXPECT_FLOAT_EQ(right[i], 0.25f);
    }
}
}
