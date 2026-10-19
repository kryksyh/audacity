/*
* Audacity: A Digital Audio Editor
*/
#include <tracktion_engine/tracktion_engine.h>

#include <gtest/gtest.h>

#include <future>
#include <optional>

#include <sndfile.h>

#include <QCoreApplication>

#if JUCE_MAC
#include <CoreFoundation/CoreFoundation.h>
#endif

#include "../internal/tracktionmixsource.h"
#include "../internal/tracktionruntime.h"
#include "au3-basic-ui/BasicUI.h"
#include "au3-effects/MixAndRender.h"
#include "au3-import-export/ExportPluginHelpers.h"
#include "au3-import-export/ExportUtils.h"
#include "au3-math/Dither.h"
#include "au3-mixer/Mix.h"
#include "au3-project/Project.h"
#include "au3-realtime-effects/RealtimeEffectList.h"
#include "au3-realtime-effects/RealtimeEffectState.h"
#include "au3-stretching-sequence/StretchingSequence.h"
#include "au3-tags/Tags.h"
#include "au3-transactions/TransactionScope.h"
#include "au3-wave-track/WaveTrack.h"
#include "au3-playable-track/TrackAutomation.h"
#include "au3-wave-track/WaveClip.h"
#include "au3wrap/internal/au3project.h"
#include "effects/builtin_collection/dynamics/compressor/compressoreffect.h"
#include "extensions/native/bytebuffer.h"
#include "global/defer.h"
#include "project/tests/testtools.h"
#include "mod-pcm/ExportPCM.h"
#include "mod-flac/ExportFLAC.h"
#include "trackedit/api/projectaudio.h"

namespace au::tracktionaudio {
namespace {
struct ExportOptions : AudioRenderSource::Options {
    bool selectionOnly = false;
};

class RenderProgress final : public BasicUI::ProgressDialog
{
public:
    explicit RenderProgress(BasicUI::ProgressResult result)
        : m_result(result) {}
    BasicUI::ProgressResult Poll(unsigned long long, unsigned long long, const TranslatableString&) override { return m_result; }
    bool Cancelled() const override { return m_result == BasicUI::ProgressResult::Cancelled; }
    void SetMessage(const TranslatableString&) override {}
    void SetDialogTitle(const TranslatableString&) override {}
    void Reinit() override {}
private:
    BasicUI::ProgressResult m_result;
};

class RenderUI final : public BasicUI::Services
{
public:
    RenderUI()
        : m_previous(BasicUI::Install(this)) {}
    ~RenderUI() override { BasicUI::Install(m_previous); }
    void DoCallAfter(const BasicUI::Action& action) override { action(); }
    void DoYield() override {}
    void DoShowErrorDialog(const BasicUI::WindowPlacement&, const TranslatableString&, const TranslatableString&,
                           const ManualPageID&, const BasicUI::ErrorDialogOptions&) override {}
    BasicUI::MessageBoxResult DoMessageBox(const TranslatableString&, BasicUI::MessageBoxOptions) override { return {}; }
    std::unique_ptr<BasicUI::ProgressDialog> DoMakeProgress(const TranslatableString&, const TranslatableString&, unsigned,
                                                            const TranslatableString&) override
    {
        return std::make_unique<RenderProgress>(result);
    }

    std::unique_ptr<BasicUI::GenericProgressDialog> DoMakeGenericProgress(const BasicUI::WindowPlacement&, const TranslatableString&,
                                                                          const TranslatableString&, int) override { return {}; }
    int DoMultiDialog(const TranslatableString&, const TranslatableString&, const TranslatableStrings&, const ManualPageID&,
                      const TranslatableString&, bool) override { return 0; }
    bool DoOpenInDefaultBrowser(const wxString&) override { return false; }
    std::unique_ptr<BasicUI::WindowPlacement> DoFindFocus() override { return {}; }
    void DoSetFocus(const BasicUI::WindowPlacement&) override {}
    bool IsUsingRtlLayout() const override { return false; }
    bool IsUiThread() const override { return true; }
    BasicUI::ProgressResult result = BasicUI::ProgressResult::Success;
private:
    BasicUI::Services* m_previous;
};

class ExportDelegate final : public ExportProcessorDelegate
{
public:
    bool IsCancelled() const override { return cancelled; }
    bool IsStopped() const override { return stopped; }
    void SetStatusString(const TranslatableString&) override {}
    void OnProgress(double value) override { progress = value; }

    bool cancelled = false;
    bool stopped = false;
    double progress = 0.0;
};
}

class ExportSourceTests : public testing::Test
{
public:
    void SetUp() override
    {
        m_project = std::make_shared<au3::Au3ProjectAccessor>(muse::modularity::globalCtx());
        const auto fixture = std::string(tracktionaudio_tests_DATA_ROOT) + "/../../trackedit/tests/data/empty.aup4";
        m_directory = juce::File::getSpecialLocation(juce::File::tempDirectory)
                      .getChildFile("audacity-tracktion-export-" + juce::Uuid().toString());
        ASSERT_TRUE(m_directory.createDirectory().wasOk());
        const auto projectFile = m_directory.getChildFile("source.aup4");
        ASSERT_TRUE(testtools::copyFile(fixture, projectFile.getFullPathName().toStdString()));
        ASSERT_TRUE(m_project->load(muse::io::path_t(projectFile.getFullPathName().toStdString()), false));
        m_runtime.init(m_directory.getChildFile("cache").getFullPathName().toStdString(),
                       m_directory.getChildFile("settings").getFullPathName().toStdString());
        m_factory = std::make_unique<AudioRenderSource::Factory::Scope>([this](const AudacityProject& project,
                                                                               const AudioRenderSource::Tracks& tracks,
                                                                               const AudioRenderSource::Options& options) {
            ++m_factoryCalls;
            return std::make_unique<TracktionMixSource>(m_runtime, project, tracks, options);
        });
        m_effectFactory = std::make_unique<RealtimeEffectState::EffectFactory::Scope>([](const PluginID& id) -> const EffectInstanceFactory* {
            static effects::CompressorEffect compressor;
            return id == wxT(
                "export-compressor") ? &compressor : nullptr;
        });
        m_previousDither = gHighQualityDither;
        gHighQualityDither = DitherType::none;
    }

    void TearDown() override
    {
        gHighQualityDither = m_previousDither;
        m_effectFactory.reset();
        m_factory.reset();
        m_runtime.deinit();
        m_project->clearSavedState();
        m_project->close();
        m_project.reset();
        m_directory.deleteRecursively();
    }

    AudacityProject& project() const
    {
        return *reinterpret_cast<AudacityProject*>(m_project->au3ProjectPtr());
    }

    std::shared_ptr<WaveTrack> addTrack(std::vector<float> values)
    {
        auto track = WaveTrackFactory::Get(project()).Create(values.size(), floatSample, 48000.0);
        auto clip = track->CreateClip(0.0, "source");
        std::vector<std::vector<float> > audio(values.size(), std::vector<float>(48000));
        std::vector<constSamplePtr> channels;
        for (size_t channel = 0; channel < values.size(); ++channel) {
            std::fill(audio[channel].begin(), audio[channel].end(), values[channel]);
            channels.push_back(reinterpret_cast<constSamplePtr>(audio[channel].data()));
        }
        clip->Append(channels.data(), floatSample, audio[0].size(), 1, floatSample);
        clip->Flush();
        track->InsertInterval(clip, true);
        TrackList::Get(project()).Add(track);
        return track;
    }

    void addCompressor(RealtimeEffectList& list, double lookahead = 10.0, double gain = 0.0)
    {
        const auto state = std::make_shared<RealtimeEffectState>(wxT("export-compressor"));
        ASSERT_TRUE(list.AddState(state));
        const auto access = state->GetAccess();
        access->ModifySettings([lookahead, gain](EffectSettings& settings) {
            auto& value = effects::CompressorEffect::GetSettings(settings);
            value.compressionRatio = 1.0;
            value.makeupGainDb = gain;
            value.lookaheadMs = lookahead;
            return nullptr;
        });
        access->Flush();
    }

    template<typename Source>
    std::vector<std::vector<float> > read(Source& source, const AudioRenderSource::Options& options)
    {
        std::vector<std::vector<float> > result(options.channels);
        while (const size_t frames = source.Process()) {
            std::vector<float> audio(frames);
            for (unsigned channel = 0; channel < options.channels; ++channel) {
                const auto* data = options.interleaved ? source.GetBuffer() + channel * SAMPLE_SIZE(options.format)
                                   : source.GetBuffer(channel);
                SamplesToFloats(data, options.format, audio.data(), frames, options.interleaved ? options.channels : 1);
                result[channel].insert(result[channel].end(), audio.begin(), audio.end());
            }
        }
        return result;
    }

    void compareWithLegacy(ExportOptions options)
    {
        auto source = ExportPluginHelpers::CreateMixer(project(), options.selectionOnly, options.startTime, options.stopTime,
                                                       options.channels, options.bufferSize, options.interleaved,
                                                       options.sampleRate, options.format,
                                                       const_cast<MixerOptions::Downmix*>(options.channelMap));
        const auto actual = read(*source, options);
        Mixer::Inputs inputs;
        for (const auto* track : ExportUtils::FindExportWaveTracks(TrackList::Get(project()), options.selectionOnly)) {
            inputs.emplace_back(StretchingSequence::Create(*track, track->GetClipInterfaces()), GetEffectStages(*track));
        }
        Mixer legacy(std::move(inputs), GetMasterEffectStages(project()), true, Mixer::WarpOptions { &project() },
                     options.startTime, options.stopTime, options.channels, options.bufferSize, options.interleaved,
                     options.sampleRate, options.format, true, const_cast<MixerOptions::Downmix*>(options.channelMap),
                     options.channelMap ? Mixer::ApplyVolume::MapChannels : Mixer::ApplyVolume::Mixdown);
        const auto expected = read(legacy, options);
        for (unsigned channel = 0; channel < options.channels; ++channel) {
            ASSERT_EQ(actual[channel].size(), expected[channel].size());
            const float tolerance = options.format == int16Sample ? 1.0f / 32768.0f
                                    : options.format == int24Sample ? 1.0f / 8388608.0f : 1e-5f;
            for (size_t frame = 0; frame < actual[channel].size(); ++frame) {
                // The constant input permits comparison with the steady output of either resampler.
                const size_t referenceFrame = options.sampleRate == 48000.0 ? frame : expected[channel].size() / 2;
                ASSERT_NEAR(actual[channel][frame], expected[channel][referenceFrame], tolerance) << channel << ", " << frame;
            }
        }
        EXPECT_NEAR(source->MixGetCurrentTime(), legacy.MixGetCurrentTime(), 1.0 / options.sampleRate);
    }

    ExportResult runProcessor(ExportProcessor& processor, ExportDelegate& delegate)
    {
        auto result = std::async(std::launch::async, [&] { return processor.Process(delegate); });
        while (result.wait_for(std::chrono::milliseconds(1)) != std::future_status::ready) {
            QCoreApplication::processEvents();
#if JUCE_MAC
            CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.0, true);
#endif
        }
        return result.get();
    }

    std::shared_ptr<au3::Au3ProjectAccessor> m_project;
    juce::File m_directory;
    TracktionRuntime m_runtime;
    std::unique_ptr<AudioRenderSource::Factory::Scope> m_factory;
    std::unique_ptr<RealtimeEffectState::EffectFactory::Scope> m_effectFactory;
    DitherType m_previousDither = DitherType::none;
    int m_factoryCalls = 0;
};

TEST_F(ExportSourceTests, MixAndRenderKeepsExplicitInputsAndTrackEffects)
{
    RenderUI ui;
    const auto left = addTrack({ 0.25f });
    const auto right = addTrack({ -0.125f });
    left->SetName("left");
    left->SetPan(-1.0f);
    right->SetPan(1.0f);
    left->SetMute(true);
    right->SetMute(true);
    addTrack({ 0.75f })->SetSolo(true);
    ASSERT_NO_FATAL_FAILURE(addCompressor(RealtimeEffectList::Get(*left), 10.0, 6.0));
    ASSERT_NO_FATAL_FAILURE(addCompressor(RealtimeEffectList::Get(project()), 10.0, 12.0));
    auto& tracks = TrackList::Get(project());
    const auto range = TrackIterRange {
        tracks.Any<const WaveTrack>().find(left.get()), ++tracks.Any<const WaveTrack>().find(right.get())
    };
    const auto rendered = MixAndRender(range, Mixer::WarpOptions { &project() }, "combined", &WaveTrackFactory::Get(project()),
                                       48000.0, floatSample, 0.2, 0.3);
    const auto* mix = dynamic_cast<const WaveTrack*>(rendered.get());
    ASSERT_TRUE(mix);
    EXPECT_EQ(m_factoryCalls, 1);
    EXPECT_EQ(mix->GetName(), "combined");
    EXPECT_EQ(mix->NChannels(), 2u);
    EXPECT_FLOAT_EQ(mix->GetVolume(), 1.0f);
    EXPECT_FLOAT_EQ(mix->GetPan(), 0.0f);
    EXPECT_EQ(RealtimeEffectList::Get(*mix).GetStatesCount(), 0u);
    EXPECT_EQ(RealtimeEffectList::Get(*left).GetStatesCount(), 1u);
    std::array<std::vector<float>, 2> audio { std::vector<float>(4800), std::vector<float>(4800) };
    float* channels[] { audio[0].data(), audio[1].data() };
    ASSERT_TRUE(mix->GetFloats(0, 2, channels, sampleCount { 0 }, audio[0].size()));
    for (size_t sample = 0; sample < audio[0].size(); ++sample) {
        ASSERT_NEAR(audio[0][sample], 0.25f * std::pow(10.0f, 6.0f / 20.0f), 1e-5f) << sample;
        ASSERT_NEAR(audio[1][sample], -0.125f, 1e-5f) << sample;
    }
}

TEST_F(ExportSourceTests, MixAndRenderKeepsMonoFormatNameAndCancellation)
{
    RenderUI ui;
    const auto input = addTrack({ 0.125f });
    input->SetName("source name");
    input->ConvertToSampleFormat(int16Sample);
    input->MoveTo(0.25);
    const auto range = TrackList::Get(project()).Any<const WaveTrack>();
    const auto render = [&] {
        return MixAndRender(range, Mixer::WarpOptions { &project() }, "unused", &WaveTrackFactory::Get(project()),
                            48000.0, int16Sample, 0.0, 0.0);
    };
    const auto rendered = render();
    const auto* mix = dynamic_cast<const WaveTrack*>(rendered.get());
    ASSERT_TRUE(mix);
    EXPECT_EQ(mix->NChannels(), 1u);
    EXPECT_EQ(mix->GetSampleFormat(), int16Sample);
    EXPECT_EQ(mix->WidestEffectiveFormat(), int16Sample);
    EXPECT_EQ(mix->GetName(), "source name");
    EXPECT_DOUBLE_EQ(mix->GetStartTime(), 0.25);
    EXPECT_DOUBLE_EQ(mix->GetEndTime(), 1.25);
    ui.result = BasicUI::ProgressResult::Cancelled;
    EXPECT_FALSE(render());
    EXPECT_EQ(TrackList::Get(project()).Any<WaveTrack>().size(), 1u);
    EXPECT_DOUBLE_EQ(input->GetStartTime(), 0.25);
    EXPECT_DOUBLE_EQ(input->GetEndTime(), 1.25);
}

TEST_F(ExportSourceTests, ExtensionReaderConvertsSnapshotAudioWithoutRealtimeEffects)
{
    using namespace trackedit::api::detail;
    const auto input = addTrack({ 0.25f, -0.0625f });
    input->SetVolume(0.5f);
    input->SetPan(0.5f);
    input->SetMute(true);
    ASSERT_TRUE(TrackAutomation::Get(*input).SetLane({ "volume", true, { { 0.0, 0.0, 0.0 } } }));
    ASSERT_NO_FATAL_FAILURE(addCompressor(RealtimeEffectList::Get(*input), 10.0, 6.0));
    ASSERT_NO_FATAL_FAILURE(addCompressor(RealtimeEffectList::Get(project()), 10.0, 12.0));
    addTrack({ 0.75f })->SetSolo(true);
    const auto snapshot = std::static_pointer_cast<WaveTrack>(input->Duplicate());
    const auto state = std::make_shared<EditState>();
    state->project = project().shared_from_this();
    AudioReaderObject reader(state, snapshot, { 1, 24000.0 }, 0.2, 0.3, nullptr);
    std::vector<float> audio;
    const size_t requests[] { 1, 17, 513, AUDIO_CHUNK_SAMPLES };
    for (size_t index = 0;; ++index) {
        const auto chunk = qobject_cast<AudioChunkObject*>(reader.read(requests[index % std::size(requests)]));
        if (!chunk) {
            break;
        }
        ASSERT_LE(chunk->sampleCount(), requests[index % std::size(requests)]);
        ASSERT_EQ(chunk->channelCount(), 1u);
        const auto* samples = static_cast<const float*>(chunk->buffer(0)->data());
        audio.insert(audio.end(), samples, samples + chunk->sampleCount());
        chunk->release();
    }
    EXPECT_EQ(m_factoryCalls, 1);
    ASSERT_EQ(audio.size(), 2400u);
    for (const auto sample : audio) {
        ASSERT_NEAR(sample, (0.25f * 0.25f - 0.0625f * 0.5f) / 2.0f, 1e-5f);
    }
}

TEST_F(ExportSourceTests, ExtensionReaderKeepsDirectReadsAndStretchedReads)
{
    using namespace trackedit::api::detail;
    const auto input = addTrack({ 0.25f });
    input->SetVolume(0.5f);
    const auto state = std::make_shared<EditState>();
    state->project = project().shared_from_this();
    {
        AudioReaderObject reader(state, input, { 1, 48000.0 }, 0.2, 0.3, nullptr);
        const auto chunk = qobject_cast<AudioChunkObject*>(reader.read(113));
        ASSERT_TRUE(chunk);
        ASSERT_EQ(chunk->sampleCount(), 113u);
        const auto* samples = static_cast<const float*>(chunk->buffer(0)->data());
        for (size_t frame = 0; frame < chunk->sampleCount(); ++frame) {
            ASSERT_FLOAT_EQ(samples[frame], 0.25f);
        }
        chunk->release();
        EXPECT_EQ(m_factoryCalls, 0);
    }
    (*input->Intervals().begin())->StretchBy(1.5);
    AudioReaderObject reader(state, input, { 1, 48000.0 }, 0.2, 0.3, nullptr);
    size_t total = 0;
    while (const auto chunk = qobject_cast<AudioChunkObject*>(reader.read(257))) {
        total += chunk->sampleCount();
        const auto* samples = static_cast<const float*>(chunk->buffer(0)->data());
        for (size_t frame = 0; frame < chunk->sampleCount(); ++frame) {
            ASSERT_NEAR(samples[frame], 0.125f, 1e-4f);
        }
        chunk->release();
    }
    EXPECT_EQ(total, 4800u);
    EXPECT_EQ(m_factoryCalls, 1);
}

TEST_F(ExportSourceTests, MatchesAudacityMixdownFormatsAndLayouts)
{
    const auto stereo = addTrack({ 0.25f, -0.0625f });
    stereo->SetVolume(0.75f);
    stereo->SetPan(0.5f);
    const auto mono = addTrack({ 0.125f });
    mono->SetVolume(0.5f);
    mono->SetPan(-0.5f);
    for (const unsigned channels : { 1u, 2u }) {
        for (const bool interleaved : { false, true }) {
            for (const auto format : { int16Sample, int24Sample, floatSample }) {
                for (const double rate : { 44100.0, 48000.0 }) {
                    SCOPED_TRACE(testing::Message() << channels << ", " << interleaved << ", " << static_cast<int>(format) << ", " << rate);
                    ExportOptions options;
                    options.startTime = 1003.0 / 48000.0;
                    options.stopTime = 5803.0 / 48000.0;
                    options.channels = channels;
                    options.bufferSize = 257;
                    options.interleaved = interleaved;
                    options.sampleRate = rate;
                    options.format = format;
                    ASSERT_NO_FATAL_FAILURE(compareWithLegacy(options));
                }
            }
        }
    }
}

TEST_F(ExportSourceTests, MatchesAudacityCustomChannelMappingAndPan)
{
    const auto stereo = addTrack({ 0.25f, -0.125f });
    stereo->SetPan(0.5f);
    const auto mono = addTrack({ 0.125f });
    mono->SetPan(-0.5f);
    MixerOptions::Downmix mapping(3, 4);
    ASSERT_TRUE(mapping.SetNumChannels(4));
    for (unsigned input = 0; input < 3; ++input) {
        std::fill_n(mapping.mMap[input].get(), 4, false);
    }
    mapping.mMap[0][1] = true;
    mapping.mMap[0][3] = true;
    mapping.mMap[1][0] = true;
    mapping.mMap[2][2] = true;
    ExportOptions options;
    options.stopTime = 0.1;
    options.channels = 4;
    options.bufferSize = 257;
    options.sampleRate = 48000.0;
    options.channelMap = &mapping;
    ASSERT_NO_FATAL_FAILURE(compareWithLegacy(options));
}

TEST_F(ExportSourceTests, KeepsSelectionMuteAndSoloRules)
{
    const auto selected = addTrack({ 0.125f });
    selected->SetSelected(true);
    const auto other = addTrack({ 0.25f });
    other->SetSelected(false);
    ExportOptions options;
    options.stopTime = 0.1;
    options.selectionOnly = true;
    options.sampleRate = 48000.0;
    const auto check = [&](std::optional<float> expected) {
        auto source = ExportPluginHelpers::CreateMixer(project(), options.selectionOnly, options.startTime, options.stopTime,
                                                       options.channels, options.bufferSize, options.interleaved, options.sampleRate,
                                                       options.format, const_cast<MixerOptions::Downmix*>(options.channelMap));
        const auto audio = read(*source, options);
        for (const auto& channel : audio) {
            ASSERT_EQ(channel.size(), expected ? 4800u : 0u);
            for (const auto sample : channel) {
                ASSERT_FLOAT_EQ(sample, *expected);
            }
        }
    };
    ASSERT_NO_FATAL_FAILURE(check(0.125f));
    other->SetSolo(true);
    ASSERT_NO_FATAL_FAILURE(check(std::nullopt));
    selected->SetSolo(true);
    selected->SetMute(true);
    ASSERT_NO_FATAL_FAILURE(check(0.125f));
    options.selectionOnly = false;
    ASSERT_NO_FATAL_FAILURE(check(0.375f));
}

TEST_F(ExportSourceTests, MatchesTrackAndMasterEffectRoutingIncludingMonoMixdown)
{
    const auto track = addTrack({ 0.25f, -0.0625f });
    track->SetPan(0.5f);
    ASSERT_NO_FATAL_FAILURE(addCompressor(RealtimeEffectList::Get(*track), 0.0));
    ASSERT_NO_FATAL_FAILURE(addCompressor(RealtimeEffectList::Get(project()), 0.0));
    for (const unsigned channels : { 1u, 2u }) {
        ExportOptions options;
        options.stopTime = 0.1;
        options.channels = channels;
        options.bufferSize = 257;
        options.sampleRate = 48000.0;
        ASSERT_NO_FATAL_FAILURE(compareWithLegacy(options));
    }
}

TEST_F(ExportSourceTests, CompensatesTrackAndMasterDelayWithinTheRequestedRange)
{
    const auto track = addTrack({ 0.25f, -0.0625f });
    track->SetPan(0.5f);
    ASSERT_NO_FATAL_FAILURE(addCompressor(RealtimeEffectList::Get(*track)));
    ASSERT_NO_FATAL_FAILURE(addCompressor(RealtimeEffectList::Get(project())));
    for (const unsigned channels : { 1u, 2u }) {
        ExportOptions options;
        options.stopTime = 0.1;
        options.channels = channels;
        options.sampleRate = 48000.0;
        options.bufferSize = 257;
        auto source = ExportPluginHelpers::CreateMixer(project(), options.selectionOnly, options.startTime, options.stopTime,
                                                       options.channels, options.bufferSize, options.interleaved, options.sampleRate,
                                                       options.format, const_cast<MixerOptions::Downmix*>(options.channelMap));
        const auto audio = read(*source, options);
        for (unsigned channel = 0; channel < channels; ++channel) {
            ASSERT_EQ(audio[channel].size(), 4800u);
            const float expected = channels == 1 ? 0.0625f : channel == 0 ? 0.125f : -0.0625f;
            for (const auto sample : audio[channel]) {
                ASSERT_NEAR(sample, expected, 1e-5f);
            }
        }
    }
}

TEST_F(ExportSourceTests, ExistingEncodersWriteTracktionAudioAndMetadata)
{
    addTrack({ 0.125f, -0.25f });
    ExportPCM pcm;
    ExportFLAC flac;
    const int encodings[] { SF_FORMAT_PCM_16, SF_FORMAT_PCM_24, SF_FORMAT_FLOAT, 0 };
    for (const int encoding : encodings) {
        ExportPlugin* plugin = encoding == 0 ? static_cast<ExportPlugin*>(&flac) : &pcm;
        int format = 0;
        if (plugin == &pcm) {
            format = -1;
            for (int index = 0; index < plugin->GetFormatCount(); ++index) {
                if (plugin->GetFormatInfo(index).format == wxT("WAV")) {
                    format = index;
                    break;
                }
            }
        }
        ASSERT_GE(format, 0);
        const auto filename = m_directory.getChildFile("output-" + juce::String(encoding) + (plugin == &pcm ? ".wav" : ".flac"));
        auto processor = plugin->CreateProcessor(format);
        ExportProcessor::Parameters parameters;
        if (plugin == &flac) {
            ASSERT_TRUE(plugin->ParseConfig(format, R"({"level":5,"bit_depth":16})", parameters));
        } else {
            parameters.emplace_back(SF_FORMAT_WAV, encoding);
        }
        Tags tags;
        tags.SetTag(TAG_TITLE, wxT("Tracktion export test"));
        const int before = m_factoryCalls;
        ASSERT_TRUE(processor->Initialize(project(), parameters, wxString::FromUTF8(filename.getFullPathName().toRawUTF8()),
                                          0.0, 0.1, false, 48000.0, 2, nullptr, &tags));
        EXPECT_EQ(m_factoryCalls, before + 1);
        ExportDelegate delegate;
        EXPECT_EQ(runProcessor(*processor, delegate), ExportResult::Success);
        EXPECT_DOUBLE_EQ(delegate.progress, 1.0);
        processor.reset();
        juce::AudioFormatManager formats;
        formats.registerBasicFormats();
        std::unique_ptr<juce::AudioFormatReader> reader(formats.createReaderFor(filename));
        ASSERT_TRUE(reader);
        EXPECT_DOUBLE_EQ(reader->sampleRate, 48000.0);
        EXPECT_EQ(reader->lengthInSamples, 4800);
        EXPECT_EQ(reader->numChannels, 2u);
        juce::AudioBuffer<float> audio(2, 4800);
        ASSERT_TRUE(reader->read(&audio, 0, 4800, 0, true, true));
        for (int frame = 0; frame < 4800; ++frame) {
            ASSERT_FLOAT_EQ(audio.getSample(0, frame), 0.125f);
            ASSERT_FLOAT_EQ(audio.getSample(1, frame), -0.25f);
        }
        SF_INFO info {};
        auto* file = sf_open(filename.getFullPathName().toRawUTF8(), SFM_READ, &info);
        ASSERT_TRUE(file);
        DEFER { sf_close(file);
        };
        EXPECT_STREQ(sf_get_string(file, SF_STR_TITLE), "Tracktion export test");
    }
}

TEST_F(ExportSourceTests, ExistingEncoderKeepsCancelAndStopResults)
{
    addTrack({ 0.125f });
    ExportFLAC plugin;
    for (const bool stop : { false, true }) {
        const auto filename = m_directory.getChildFile(stop ? "stopped.flac" : "cancelled.flac");
        auto processor = plugin.CreateProcessor(0);
        ExportProcessor::Parameters parameters;
        ASSERT_TRUE(plugin.ParseConfig(0, R"({"level":5,"bit_depth":16})", parameters));
        ASSERT_TRUE(processor->Initialize(project(), parameters, wxString::FromUTF8(filename.getFullPathName().toRawUTF8()),
                                          0.0, 0.1, false, 48000.0, 2));
        ExportDelegate delegate;
        delegate.cancelled = !stop;
        delegate.stopped = stop;
        EXPECT_EQ(runProcessor(*processor, delegate), stop ? ExportResult::Stopped : ExportResult::Cancelled);
    }
}

TEST_F(ExportSourceTests, ExistingEncoderWritesTheCustomFourChannelMap)
{
    const auto stereo = addTrack({ 0.25f, -0.125f });
    stereo->SetPan(0.5f);
    const auto mono = addTrack({ 0.125f });
    mono->SetPan(-0.5f);
    MixerOptions::Downmix mapping(3, 4);
    ASSERT_TRUE(mapping.SetNumChannels(4));
    for (unsigned input = 0; input < 3; ++input) {
        std::fill_n(mapping.mMap[input].get(), 4, false);
    }
    mapping.mMap[0][1] = true;
    mapping.mMap[0][3] = true;
    mapping.mMap[1][0] = true;
    mapping.mMap[2][2] = true;
    ExportPCM plugin;
    int format = -1;
    for (int index = 0; index < plugin.GetFormatCount(); ++index) {
        if (plugin.GetFormatInfo(index).format == wxT("WAV")) {
            format = index;
            break;
        }
    }
    ASSERT_GE(format, 0);
    const auto filename = m_directory.getChildFile("mapped.wav");
    auto processor = plugin.CreateProcessor(format);
    ASSERT_TRUE(processor->Initialize(project(), {}, wxString::FromUTF8(filename.getFullPathName().toRawUTF8()),
                                      0.0, 0.1, false, 48000.0, 4, &mapping));
    ExportDelegate delegate;
    ASSERT_EQ(runProcessor(*processor, delegate), ExportResult::Success);
    processor.reset();
    juce::WavAudioFormat wav;
    std::unique_ptr<juce::AudioFormatReader> reader(wav.createReaderFor(filename.createInputStream().release(), true));
    ASSERT_TRUE(reader);
    ASSERT_EQ(reader->numChannels, 4u);
    ASSERT_EQ(reader->lengthInSamples, 4800);
    juce::AudioBuffer<float> audio(4, 4800);
    ASSERT_TRUE(reader->read(&audio, 0, 4800, 0, true, true));
    const float expected[] { -0.0625f, 0.25f, 0.125f, 0.25f };
    for (int channel = 0; channel < 4; ++channel) {
        for (int frame = 0; frame < 4800; ++frame) {
            ASSERT_FLOAT_EQ(audio.getSample(channel, frame), expected[channel]);
        }
    }
}
}
