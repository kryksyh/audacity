/*
* Audacity: A Digital Audio Editor
*/
#include <tracktion_engine/tracktion_engine.h>
#include <tracktion_graph/tracktion_graph.h>
#include <juce_dsp/juce_dsp.h>

#include "../internal/audiosourceregistry.h"

#include <gtest/gtest.h>
#include <portaudio.h>

#include <chrono>
#include <cmath>
#include <numbers>
#include <thread>
#include <future>

#include <QCoreApplication>

#if JUCE_MAC
#include <CoreFoundation/CoreFoundation.h>
#endif

#if AU_TRACKTION_TEST_AUDIO_UNIT
#include "au3-audio-unit/AudioUnitEffectBase.h"
#include <AudioUnit/AudioUnitParameters.h>
#endif

#if AU_TRACKTION_TEST_VST3
#include "au3-vst3/VST3EffectsModule.h"
#include "au3-vst3/VST3Wrapper.h"
#include <pluginterfaces/vst/ivsteditcontroller.h>
#endif

#include "../internal/audacityaudiosource.h"
#include "../internal/tracktionruntime.h"
#include "../internal/tracktionsession.h"
#include "../internal/tracktionautomation.h"
#include "../internal/tracktioninstrumenteditor.h"
#include "au3-playable-track/TrackAutomation.h"
#include "au3-playable-track/TrackRouting.h"
#include "../internal/tracktionrecording.h"
#include "../internal/tracktionrendersource.h"
#include "../internal/tracktionmixsource.h"
#include "../internal/tracktionaudiocallbacks.h"
#include "../internal/tracktiontrackmeters.h"
#include "../internal/tracktionaudioengine.h"
#include "../internal/tracktioneffectinstance.h"
#include "../internal/tracktionpluginscanner.h"
#include "effects/audio_unit/internal/audiouniteffectloader.h"
#include "global/defer.h"
#include "global/async/processevents.h"
#include "au3wrap/au3types.h"
#include "au3wrap/internal/au3project.h"
#include "au3-wave-track/WaveClip.h"
#include "au3-wave-track/WaveTrack.h"
#include "au3-wave-track/Sequence.h"
#include "au3-exceptions/AudacityException.h"
#include "au3-mixer/Envelope.h"
#include "au3-mixer/MixerOptions.h"
#include "au3-numeric-formats/ProjectTimeSignature.h"
#include "au3-stretching-sequence/StretchingSequence.h"
#include "au3-realtime-effects/RealtimeEffectList.h"
#include "au3-realtime-effects/RealtimeEffectState.h"
#include "au3-basic-ui/BasicUI.h"
#include "au3-project-history/ProjectHistory.h"
#include "au3-import-export/ExportUtils.h"
#include "au3-import-export/ExportPluginHelpers.h"
#include "au3-project-history/UndoManager.h"
#include "au3-module-manager/ConfigInterface.h"
#include "au3-audio-devices/AudioIOBase.h"
#if AU_TRACKTION_TEST_NYQUIST
#include "au3-nyquist-effects/LoadNyquist.h"
#endif
#include "effects/builtin_collection/basstreble/basstrebleeffect.h"
#include "effects/builtin_collection/dynamics/compressor/compressoreffect.h"
#include "effects/builtin_collection/reverb/reverbeffect.h"
#include "project/tests/testtools.h"
#include "audio/tests/mocks/audiodrivercontrollermock.h"
#include "record/tests/mocks/recordconfigurationmock.h"
#include "global/tests/mocks/globalconfigurationmock.h"
#include "audioplugins/tests/mocks/knownaudiopluginsregistermock.h"

namespace au::tracktionaudio {
namespace {
class RecordingStorageException final : public AudacityException
{
public:
    explicit RecordingStorageException(std::shared_ptr<int> handled)
        : m_handled(std::move(handled)) {}

    void DelayedHandlerAction() override { ++*m_handled; }

private:
    std::shared_ptr<int> m_handled;
};

class FailingRecordingDestination final : public RecordableSequence
{
public:
    enum class Failure {
        Append, Flush
    };

    FailingRecordingDestination(std::shared_ptr<WaveTrack> track, Failure failure)
        : m_track(std::move(track)), m_failure(failure) {}

    sampleFormat GetSampleFormat() const override { return m_track->GetSampleFormat(); }
    double GetRate() const override { return m_track->GetRate(); }
    size_t NChannels() const override { return m_track->NChannels(); }
    int64_t GetRecordableSequenceId() const override { return m_track->GetRecordableSequenceId(); }
    void InsertSilence(double time, double length) override { m_track->InsertSilence(time, length); }

    bool Append(size_t channel, constSamplePtr buffer, sampleFormat format, size_t length,
                unsigned stride, sampleFormat effectiveFormat) override
    {
        ++appendCalls;
        if (m_failure == Failure::Append && channel == 1) {
            throw RecordingStorageException(handled);
        }
        return m_track->Append(channel, buffer, format, length, stride, effectiveFormat);
    }

    void Flush() override
    {
        ++flushCalls;
        m_track->Flush();
        if (m_failure == Failure::Flush) {
            throw RecordingStorageException(handled);
        }
    }

    void RepairChannels() override
    {
        ++repairCalls;
        m_track->RepairChannels();
    }

    void waitForErrorHandler() const
    {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (*handled == 0 && std::chrono::steady_clock::now() < deadline) {
            muse::async::processMessages();
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }

    int appendCalls = 0;
    int flushCalls = 0;
    int repairCalls = 0;
    const std::shared_ptr<int> handled = std::make_shared<int>(0);

private:
    const std::shared_ptr<WaveTrack> m_track;
    const Failure m_failure;
};

class MeterCapture final : public IMeterSender
{
public:
    struct Entry {
        uint8_t channel;
        LevelData levels;
        std::optional<TrackId> track;
    };

    void start(double) override {}
    void stop() override {}
    void push(uint8_t channel, const InterleavedSampleData& data, const std::optional<TrackId>& track) override
    {
        float peak = 0.0f;
        float squareSum = 0.0f;
        for (size_t frame = 0; frame < data.frames; ++frame) {
            const float sample = data.buffer[frame * data.nChannels];
            peak = std::max(peak, std::abs(sample));
            squareSum += sample * sample;
        }
        pushLevels(channel, { peak, std::sqrt(squareSum / data.frames), data.frames, data.dacTime }, track);
    }

    void pushLevels(uint8_t channel, const LevelData& levels, const std::optional<TrackId>& track) override
    {
        entries.push_back({ channel, levels, track });
    }

    const Entry* latest(int64_t track, uint8_t channel) const
    {
        for (auto entry = entries.rbegin(); entry != entries.rend(); ++entry) {
            if (entry->track && entry->track->value == track && entry->channel == channel) {
                return &*entry;
            }
        }
        return nullptr;
    }

    std::vector<Entry> entries;
};

class HostedPlayback
{
public:
    explicit HostedPlayback(tracktion::Engine& engine, int inputChannels = 0, int inputLatency = 0, int outputChannels = 2,
                            std::shared_ptr<AudioDeviceCallback> callback = {})
        : m_engine(engine), m_audioIO(engine.getDeviceManager().getHostedAudioDeviceInterface()),
        m_callback(std::move(callback)), m_inputChannels(inputChannels), m_outputChannels(outputChannels)
    {
        m_engine.getDeviceManager().setCpuLimitBeforeMuting(1000.0);
        tracktion::HostedAudioDeviceInterface::Parameters parameters;
        parameters.sampleRate = 48000.0;
        parameters.blockSize = 256;
        parameters.inputChannels = inputChannels;
        parameters.inputLatencyNumSamples = inputLatency;
        parameters.outputChannels = outputChannels;
        m_audioIO.initialise(parameters);
        m_audioIO.prepareToPlay(parameters.sampleRate, parameters.blockSize);
        m_engine.getDeviceManager().dispatchPendingUpdates();
        if (m_callback) {
            m_callback->prepare(parameters.sampleRate, parameters.blockSize, inputChannels, outputChannels);
        }
    }

    ~HostedPlayback()
    {
        m_engine.getDeviceManager().deviceManager.closeAudioDevice();
        m_engine.getDeviceManager().removeHostedAudioDeviceInterface();
    }

    juce::AudioBuffer<float> process(int blocks, bool dispatchTimers = true)
    {
        juce::AudioBuffer<float> result(2, blocks * 256);
        for (int block = 0; block < blocks; ++block) {
            juce::AudioBuffer<float> audio(std::max({ 2, m_inputChannels, m_outputChannels }), 256);
            audio.clear();
            juce::MidiBuffer midi;
            const auto time = std::chrono::steady_clock::now();
            if (m_callback) {
                m_callback->processInput(audio.getArrayOfReadPointers(), m_inputChannels, 256, time);
            }
            m_audioIO.processBlock(audio, midi);
            if (m_callback) {
                m_callback->processOutput(audio.getArrayOfWritePointers(), m_outputChannels, 256, time);
            }
            for (int channel = 0; channel < 2; ++channel) {
                result.copyFrom(channel, block * 256, audio, channel, 0, 256);
            }
            if (dispatchTimers) {
                juce::Timer::callPendingTimersSynchronously();
            }
            std::this_thread::sleep_for(std::chrono::microseconds(5334));
        }
        return result;
    }

    void processInput(const juce::AudioBuffer<float>& input)
    {
        for (int offset = 0; offset < input.getNumSamples(); offset += 256) {
            const int count = std::min(256, input.getNumSamples() - offset);
            juce::AudioBuffer<float> audio(std::max({ 2, input.getNumChannels(), m_outputChannels }), count);
            audio.clear();
            for (int channel = 0; channel < input.getNumChannels(); ++channel) {
                audio.copyFrom(channel, 0, input, channel, offset, count);
            }
            juce::MidiBuffer midi;
            const auto time = std::chrono::steady_clock::now();
            if (m_callback) {
                m_callback->processInput(audio.getArrayOfReadPointers(), input.getNumChannels(), count, time);
            }
            m_audioIO.processBlock(audio, midi);
            if (m_callback) {
                m_callback->processOutput(audio.getArrayOfWritePointers(), m_outputChannels, count, time);
            }
            juce::Timer::callPendingTimersSynchronously();
            std::this_thread::sleep_for(std::chrono::microseconds(5334));
        }
    }

private:
    tracktion::Engine& m_engine;
    tracktion::HostedAudioDeviceInterface& m_audioIO;
    const std::shared_ptr<AudioDeviceCallback> m_callback;
    int m_inputChannels;
    int m_outputChannels;
};

std::shared_ptr<RealtimeEffectState> addBuiltinEffect(RealtimeEffectList& list, const wxString& id)
{
    static effects::BassTrebleEffect bassTreble;
    static effects::CompressorEffect compressor;
    static effects::ReverbEffect reverb;
    const RealtimeEffectState::EffectFactory::Scope factory([](const PluginID& effectId) -> const EffectInstanceFactory* {
        if (effectId == wxT("tracktion-bass-treble")) {
            return &bassTreble;
        }
        if (effectId == wxT("tracktion-compressor")) {
            return &compressor;
        }
        if (effectId == wxT("tracktion-reverb")) {
            return &reverb;
        }
        return nullptr;
    });
    auto state = std::make_shared<RealtimeEffectState>(id);
    return list.AddState(state) ? state : nullptr;
}

#if AU_TRACKTION_TEST_VST3
std::unique_ptr<ComponentInterface> loadTestVst3(const char* file = AU_TRACKTION_TEST_VST3_PATH)
{
    VST3EffectsModule module;
    EXPECT_TRUE(module.Initialize());
    PluginPath path;
    PluginID id;
    TranslatableString error;
    const auto count = module.DiscoverPluginsAtPath(wxString::FromUTF8(file), error,
                                                    [&](PluginProvider*, ComponentInterface* effect) -> const PluginID& {
        path = effect->GetPath();
        return id;
    });
    EXPECT_EQ(count, 1u) << error.debugStr().toStdString();
    return module.LoadPlugin(path);
}

std::unique_ptr<VST3Wrapper> makeVst3Wrapper(const VST3EffectBase& effect)
{
    auto module = effect.vstModule();
    for (const auto& info : module->getFactory().classInfos()) {
        if (info.ID().toString() == effect.GetPath().AfterLast(';').ToStdString()) {
            auto wrapper = std::make_unique<VST3Wrapper>(*module, info);
            wrapper->InitializeComponents();
            return wrapper;
        }
    }
    return nullptr;
}

Steinberg::Vst::ParamID vst3GainID(const VST3Wrapper& instance)
{
    const auto controller = instance.mEditController;
    for (Steinberg::int32 index = 0; index < controller->getParameterCount(); ++index) {
        Steinberg::Vst::ParameterInfo info {};
        controller->getParameterInfo(index, info);
        if (info.title[0] == u'G' && info.title[1] == u'a' && info.title[2] == u'i' && info.title[3] == u'n') {
            return info.id;
        }
    }
    ADD_FAILURE() << "The test plugin has no gain parameter.";
    return 0;
}

void setVst3Gain(const VST3EffectBase& effect, EffectSettings& settings, Steinberg::Vst::ParamID id, float gain)
{
    CommandParameters parameters;
    EXPECT_TRUE(effect.SaveSettings(settings, parameters));
    parameters.Write("Parameters", wxString::Format("%lu=%f;", static_cast<unsigned long>(id), gain));
    EXPECT_TRUE(effect.LoadSettings(parameters, settings));
}

#endif
}

class AudioSourceTests : public testing::Test
{
public:
    void SetUp() override
    {
        m_project = std::make_shared<au3::Au3ProjectAccessor>(muse::modularity::globalCtx());
        const auto fixture = std::string(tracktionaudio_tests_DATA_ROOT) + "/../../trackedit/tests/data/empty.aup4";
        m_projectFile = juce::File::getSpecialLocation(juce::File::tempDirectory)
                        .getChildFile("audacity-tracktion-test-" + juce::Uuid().toString() + ".aup4");
        ASSERT_TRUE(testtools::copyFile(fixture, m_projectFile.getFullPathName().toStdString()));
        ASSERT_TRUE(m_project->load(muse::io::path_t(m_projectFile.getFullPathName().toStdString()), false));
    }

    void TearDown() override
    {
        m_project->clearSavedState();
        m_project->close();
        m_project.reset();
        testtools::removeProjectIfExists(m_projectFile.getFullPathName().toStdString());
    }

    static std::vector<float> makeVoiceSamples(size_t frames)
    {
        std::vector<float> samples(frames);
        for (size_t frame = 0; frame < samples.size(); ++frame) {
            for (int harmonic = 1; harmonic <= 60; ++harmonic) {
                const double frequency = harmonic * 100.0;
                const double firstFormant = std::exp(-0.5 * std::pow((frequency - 1000.0) / 100.0, 2.0));
                const double secondFormant = 0.4 * std::exp(-0.5 * std::pow((frequency - 2500.0) / 200.0, 2.0));
                const double amplitude = 0.01 * (0.04 / harmonic + firstFormant + secondFormant);
                samples[frame] += amplitude * std::sin(2.0 * std::numbers::pi * frequency * frame / 48000.0);
            }
        }
        return samples;
    }

    static double peakFrequency(const juce::AudioBuffer<float>& audio, double lower, double upper, int offset = 12000)
    {
        constexpr int order = 14;
        constexpr int size = 1 << order;
        std::vector<float> spectrum(2 * size);
        for (int frame = 0; frame < size; ++frame) {
            const double window = 0.5 - 0.5 * std::cos(2.0 * std::numbers::pi * frame / (size - 1));
            spectrum[frame] = audio.getSample(0, offset + frame) * window;
        }
        juce::dsp::FFT fft(order);
        fft.performFrequencyOnlyForwardTransform(spectrum.data());
        const int first = static_cast<int>(lower * size / 48000.0);
        const int last = static_cast<int>(upper * size / 48000.0);
        const auto peak = std::max_element(spectrum.begin() + first, spectrum.begin() + last);
        return (peak - spectrum.begin()) * 48000.0 / size;
    }

    std::shared_ptr<WaveClip> makeClip(const std::vector<std::vector<float> >& data)
    {
        auto& project = *reinterpret_cast<au3::Au3Project*>(m_project->au3ProjectPtr());
        auto track = WaveTrackFactory::Get(project).Create(data.size(), floatSample, 48000.0);
        auto clip = track->CreateClip(0.0, "source");
        std::vector<constSamplePtr> channels;
        for (const auto& channel : data) {
            channels.push_back(reinterpret_cast<constSamplePtr>(channel.data()));
        }
        clip->Append(channels.data(), floatSample, data.front().size(), 1, floatSample);
        clip->Flush();
        return clip;
    }

    std::shared_ptr<WaveTrack> makeTrack(const std::vector<std::vector<float> >& data)
    {
        auto& project = *reinterpret_cast<au3::Au3Project*>(m_project->au3ProjectPtr());
        auto track = WaveTrackFactory::Get(project).Create(data.size(), floatSample, 48000.0);
        track->InsertInterval(makeClip(data), true);
        return track;
    }

    std::shared_ptr<WaveTrack> makeCaptureTrack(int channels, double rate = 48000.0)
    {
        auto& project = *reinterpret_cast<au3::Au3Project*>(m_project->au3ProjectPtr());
        return WaveTrackFactory::Get(project).Create(channels, floatSample, rate);
    }

    void applyOfflineEffect(PerTrackEffect& effect, EffectSettings& settings, const std::vector<std::vector<float> >& input,
                            juce::AudioBuffer<float>& output, bool success = true)
    {
        auto& project = *reinterpret_cast<au3::Au3Project*>(m_project->au3ProjectPtr());
        const auto tracks = TrackList::Create(nullptr);
        const auto track = makeTrack(input);
        track->SetSelected(true);
        tracks->Add(track);
        effect.SetTracks(tracks.get());
        DEFER { effect.SetTracks(nullptr);
        };
        effect.mFactory = &WaveTrackFactory::Get(project);
        effect.mProjectRate = 48000.0;
        effect.mT0 = 101.0 / 48000.0;
        effect.mT1 = (input.front().size() - 97.0) / 48000.0;
        effect.CountWaveTracks();
        const auto dialogInstance = std::dynamic_pointer_cast<EffectInstanceEx>(effect.MakeInstance());
        ASSERT_TRUE(dialogInstance);
        ASSERT_TRUE(dialogInstance->Init());
        const auto instance = std::dynamic_pointer_cast<EffectInstanceEx>(effect.MakeProcessingInstance(dialogInstance));
        ASSERT_TRUE(instance);
        ASSERT_TRUE(instance->Init()) << instance->GetLastError();
        ASSERT_EQ(instance->Process(settings), success) << instance->GetLastError();
        const auto* result = *tracks->Any<const WaveTrack>().begin();
        ASSERT_EQ(result->NChannels(), input.size());
        ASSERT_EQ(result->NIntervals(), 1u);
        const AudacityAudioSource source(**result->Intervals().begin());
        ASSERT_EQ(source.sampleCount(), input.front().size());
        output.setSize(input.size(), source.sampleCount());
        for (size_t channel = 0; channel < input.size(); ++channel) {
            ASSERT_TRUE(source.read(channel, output.getWritePointer(channel), 0, source.sampleCount()));
        }
    }

    void recordInput(const juce::AudioBuffer<float>& input, const std::vector<std::shared_ptr<RecordableSequence> >& destinations,
                     TracktionRecording::Options options = {}, int inputLatency = 0, double startTime = 0.0, int outputChannels = 2)
    {
        const auto directory = m_projectFile.getSiblingFile("tracktion-recording-input-" + juce::Uuid().toString());
        DEFER { directory.deleteRecursively();
        };
        TracktionRuntime runtime;
        runtime.init(directory.getChildFile("cache").getFullPathName().toStdString(),
                     directory.getChildFile("settings").getFullPathName().toStdString());
        HostedPlayback playback(runtime.engine(), input.getNumChannels(), inputLatency, outputChannels);
        const auto tracks = TrackList::Create(nullptr);
        TracktionSession session(runtime, *tracks, TracktionSession::Role::Playback);
        TracktionRecording recording(session.edit(), destinations, input.getNumChannels(), directory.getFullPathName().toStdString(),
                                     options);
        const auto startError = recording.start(startTime);
        ASSERT_TRUE(startError.empty()) << startError;
        playback.processInput(input);
        const auto stopError = recording.stop();
        ASSERT_TRUE(stopError.empty()) << stopError;
    }

    void checkPlaybackSpeeds(bool stretched)
    {
        const auto directory = m_projectFile.getSiblingFile("tracktion-playback-speed-" + juce::Uuid().toString());
        DEFER { directory.deleteRecursively();
        };
        TracktionRuntime runtime;
        runtime.init(directory.getChildFile("cache").getFullPathName().toStdString(),
                     directory.getChildFile("settings").getFullPathName().toStdString());
        HostedPlayback playback(runtime.engine());
        for (const double speed : { 0.01, 0.5, 1.0, 1.5, 2.0, 32.0 }) {
            SCOPED_TRACE(speed);
            // Keep the measured tone in the audible range.
            const double sourceFrequency = speed == 0.01 ? 2200.0 : speed == 32.0 ? 110.0 : 220.0;
            std::vector<float> samples(32 * 48000);
            for (size_t frame = 0; frame < samples.size(); ++frame) {
                samples[frame] = 0.25f * std::sin(2.0 * std::numbers::pi * sourceFrequency * frame / 48000.0);
            }
            const auto track = makeTrack({ samples });
            if (stretched) {
                const auto clip = *track->Intervals().begin();
                clip->StretchBy(1.5);
                ASSERT_TRUE(clip->SetCentShift(1200));
            }
            const auto tracks = TrackList::Create(nullptr);
            tracks->Add(track);
            TracktionSession session(runtime, *tracks, TracktionSession::Role::Playback);
            session.setPlaybackSpeed(speed);
            auto& transport = session.edit().getTransport();
            transport.play(false);
            const auto audio = playback.process(128);
            const auto* context = transport.getCurrentPlaybackContext();
            ASSERT_TRUE(context->getSyncPoint());
            EXPECT_NEAR(context->getSyncPoint()->time.inSeconds() * 48000.0, audio.getNumSamples() * speed, 1.0);
            constexpr int firstFrame = 4096;
            const int count = audio.getNumSamples() - firstFrame;
            constexpr int fftOrder = 15;
            constexpr int fftSize = 1 << fftOrder;
            std::vector<float> spectrum(fftSize * 2);
            std::copy_n(audio.getReadPointer(0, firstFrame), count, spectrum.data());
            const juce::dsp::WindowingFunction<float> window(count, juce::dsp::WindowingFunction<float>::hann);
            window.multiplyWithWindowingTable(spectrum.data(), count);
            const juce::dsp::FFT fft(fftOrder);
            fft.performFrequencyOnlyForwardTransform(spectrum.data());
            const auto peak = std::max_element(spectrum.begin() + 1, spectrum.begin() + fftSize / 2);
            const double frequency = (peak - spectrum.begin()) * 48000.0 / fftSize;
            const double expectedFrequency = sourceFrequency * speed * (stretched ? 2.0 : 1.0);
            EXPECT_NEAR(frequency, expectedFrequency, 48000.0 / fftSize);
            EXPECT_GT(audio.getRMSLevel(0, firstFrame, count), 0.05f);
            transport.stop(false, true, false);
        }
    }

    void renderTracks(TrackList& tracks, juce::AudioBuffer<float>& rendered, int frameCount,
                      TracktionSession::Selection selection = TracktionSession::Selection::All)
    {
        const auto directory = m_projectFile.getSiblingFile("tracktion-render-" + juce::Uuid().toString());
        DEFER { directory.deleteRecursively();
        };
        TracktionRuntime runtime;
        runtime.init(directory.getChildFile("cache").getFullPathName().toStdString(),
                     directory.getChildFile("settings").getFullPathName().toStdString());

        TracktionSession session(runtime, tracks, TracktionSession::Role::Render, selection);
        auto& edit = session.edit();
        const juce::TemporaryFile output(".wav");
        tracktion::Renderer::Parameters parameters(edit);
        parameters.destFile = output.getFile();
        parameters.audioFormat = runtime.engine().getAudioFileFormatManager().getWavFormat();
        parameters.sampleRateForAudio = 48000.0;
        parameters.bitDepth = 32;
        parameters.time = { tracktion::TimePosition::fromSeconds(0.0),
                            tracktion::TimePosition::fromSeconds(frameCount / 48000.0) };
        parameters.canRenderInMono = false;
        parameters.useMasterPlugins = true;

        {
            const tracktion::Edit::ScopedRenderStatus rendering(edit, true);
            tracktion::Renderer::RenderTask task("Render test", parameters, nullptr, nullptr);
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
            while (task.runJob() == juce::ThreadPoolJob::jobNeedsRunningAgain) {
                ASSERT_LT(std::chrono::steady_clock::now(), deadline);
            }
            ASSERT_TRUE(task.errorMessage.isEmpty()) << task.errorMessage.toStdString();
        }

        std::unique_ptr<juce::AudioFormatReader> reader(runtime.engine().getAudioFileFormatManager().readFormatManager
                                                        .createReaderFor(output.getFile()));
        ASSERT_TRUE(reader);
        ASSERT_EQ(reader->numChannels, 2u);
        rendered.setSize(2, frameCount);
        ASSERT_TRUE(reader->read(&rendered, 0, frameCount, 0, true, true));
    }

    std::shared_ptr<au3::Au3ProjectAccessor> m_project;
    juce::File m_projectFile;
};

TEST_F(AudioSourceTests, StereoSourceKeepsSeparateChannelsAndFrameCount)
{
    const auto clip = makeClip({ { 0.25f, 0.5f, 0.75f }, { -0.25f, -0.5f, -0.75f } });
    const AudacityAudioSource source(*clip);
    EXPECT_EQ(source.channelCount(), 2u);
    EXPECT_EQ(source.sampleCount(), 3);
    EXPECT_DOUBLE_EQ(source.sampleRate(), 48000.0);

    std::vector<float> samples(3);
    ASSERT_TRUE(source.read(0, samples.data(), 0, 3));
    EXPECT_EQ(samples, (std::vector<float> { 0.25f, 0.5f, 0.75f }));
    ASSERT_TRUE(source.read(1, samples.data(), 0, 3));
    EXPECT_EQ(samples, (std::vector<float> { -0.25f, -0.5f, -0.75f }));
}

TEST_F(AudioSourceTests, SourcePadsReadsOutsideTheStoredRange)
{
    const auto clip = makeClip({ { 0.25f, 0.5f, 0.75f } });
    const AudacityAudioSource source(*clip);
    std::vector<float> samples(7, 1.0f);
    ASSERT_TRUE(source.read(0, samples.data(), -2, 7));
    EXPECT_EQ(samples, (std::vector<float> { 0.0f, 0.0f, 0.25f, 0.5f, 0.75f, 0.0f, 0.0f }));
}

TEST_F(AudioSourceTests, SourceKeepsSamplesAfterTheDocumentChanges)
{
    auto clip = makeClip({ { 0.25f, 0.5f, 0.75f } });
    const AudacityAudioSource source(*clip);
    const float replacement[] = { -0.25f, -0.5f, -0.75f };
    clip->SetSamples(0, reinterpret_cast<constSamplePtr>(replacement), floatSample, 0, 3, floatSample);
    clip.reset();

    std::vector<float> samples(3);
    ASSERT_TRUE(source.read(0, samples.data(), 0, 3));
    EXPECT_EQ(samples, (std::vector<float> { 0.25f, 0.5f, 0.75f }));
}

TEST_F(AudioSourceTests, EnvelopeSnapshotsStayValidDuringConcurrentReads)
{
    const auto clip = makeClip({ { 0.25f, 0.5f, 0.75f } });
    AudacityAudioSource source(*clip);
    const std::weak_ptr<const Envelope> initial = source.envelope();
    std::promise<void> acquired;
    std::promise<void> release;
    auto allowRelease = release.get_future();
    auto reader = std::async(std::launch::async, [&] {
        const auto held = source.envelope();
        acquired.set_value();
        bool valid = true;
        for (int iteration = 0; iteration < 1000; ++iteration) {
            const auto snapshot = source.envelope();
            std::array<double, 32> values;
            snapshot->GetValues(values.data(), values.size(), 0.0, 1.0 / 48000.0);
            for (const auto value : values) {
                valid &= value == 1.0 || value == 0.25 || value == 0.75;
                valid &= value == values[0];
            }
            std::this_thread::yield();
        }
        allowRelease.wait();
        return valid && held->GetDefaultValue() == 1.0;
    });
    acquired.get_future().wait();
    for (int iteration = 0; iteration < 1000; ++iteration) {
        clip->GetEnvelope().Flatten(iteration % 2 == 0 ? 0.25 : 0.75);
        source.setEnvelope(clip->GetEnvelope());
        std::this_thread::yield();
    }
    EXPECT_FALSE(initial.expired());
    release.set_value();
    EXPECT_TRUE(reader.get());
    source.setEnvelope(clip->GetEnvelope());
    EXPECT_TRUE(initial.expired());
}

TEST_F(AudioSourceTests, SourceReadsAcrossSampleBlockBoundaries)
{
    std::vector<float> samples(2 * Sequence::GetMaxDiskBlockSize() / sizeof(float) + 32);
    for (size_t sample = 0; sample < samples.size(); ++sample) {
        samples[sample] = static_cast<float>(sample % 1000) / 1000.0f;
    }
    const auto clip = makeClip({ samples });
    const auto& blocks = clip->GetSequence(0)->GetBlockArray();
    ASSERT_GT(blocks.size(), 1u);
    const auto start = blocks[1].start.as_long_long() - 5;

    const AudacityAudioSource source(*clip);
    std::vector<float> read(12);
    ASSERT_TRUE(source.read(0, read.data(), start, read.size()));
    EXPECT_EQ(read, (std::vector<float>(samples.begin() + start, samples.begin() + start + read.size())));
}

TEST_F(AudioSourceTests, JuceReaderKeepsTheSourceAfterRegistrationIsRemoved)
{
    const auto clip = makeClip({ { 0.25f, 0.5f, 0.75f }, { -0.25f, -0.5f, -0.75f } });
    auto registry = std::make_shared<AudioSourceRegistry>();
    const auto file = registry->add(std::make_shared<AudacityAudioSource>(*clip));
    ASSERT_TRUE(file.existsAsFile());

    juce::AudioFormatManager formats;
    formats.registerFormat(registry->createFormat().release(), false);
    std::unique_ptr<juce::AudioFormatReader> reader(formats.createReaderFor(file));
    ASSERT_TRUE(reader);
    EXPECT_EQ(reader->lengthInSamples, 3);
    EXPECT_EQ(reader->numChannels, 2u);
    registry->remove(file);

    juce::AudioBuffer<float> buffer(2, 5);
    ASSERT_TRUE(reader->read(&buffer, 0, 5, 0, true, true));
    EXPECT_FLOAT_EQ(buffer.getSample(0, 0), 0.25f);
    EXPECT_FLOAT_EQ(buffer.getSample(1, 2), -0.75f);
    EXPECT_FLOAT_EQ(buffer.getSample(0, 4), 0.0f);
    EXPECT_FLOAT_EQ(buffer.getSample(1, 4), 0.0f);
    EXPECT_FALSE(file.exists());
}

TEST_F(AudioSourceTests, TracktionCacheReadsAudacityProjectStorage)
{
    const auto directory = m_projectFile.getSiblingFile("tracktion-runtime-" + juce::Uuid().toString());
    DEFER { directory.deleteRecursively();
    };
    TracktionRuntime runtime;
    runtime.init(directory.getChildFile("cache").getFullPathName().toStdString(),
                 directory.getChildFile("settings").getFullPathName().toStdString());

    const auto clip = makeClip({ { 0.25f, 0.5f, 0.75f }, { -0.25f, -0.5f, -0.75f } });
    const auto file = runtime.sources()->add(std::make_shared<AudacityAudioSource>(*clip));
    const tracktion::AudioFile audioFile(runtime.engine(), file);
    ASSERT_TRUE(audioFile.isValid());
    EXPECT_EQ(audioFile.getLengthInSamples(), 3);
    EXPECT_EQ(audioFile.getNumChannels(), 2);

    const auto reader = runtime.engine().getAudioFileManager().cache.createReader(audioFile);
    ASSERT_TRUE(reader);
    reader->setReadPosition(0);
    juce::AudioBuffer<float> samples(2, 3);
    ASSERT_TRUE(reader->readSamples(3, samples, tracktion::ChannelConfiguration::stereo(), 0,
                                    tracktion::ChannelConfiguration::stereo(), 2000));
    EXPECT_FLOAT_EQ(samples.getSample(0, 1), 0.5f);
    EXPECT_FLOAT_EQ(samples.getSample(1, 2), -0.75f);
}

TEST_F(AudioSourceTests, TracktionRenderPreservesTrimOffsetEnvelopeVolumeAndPan)
{
    std::vector<float> left(4800), right(4800);
    for (size_t sample = 0; sample < left.size(); ++sample) {
        left[sample] = sample / 9600.0f;
        right[sample] = -left[sample];
    }
    const auto track = makeTrack({ left, right });
    track->SetVolume(0.5f);
    track->SetPan(0.5f);
    const auto clip = *track->Intervals().begin();
    clip->SetSequenceStartTime(0.1);
    clip->SetTrimLeft(0.02);
    clip->SetTrimRight(0.01);
    clip->GetEnvelope().Flatten(0.5);

    const auto tracks = TrackList::Create(nullptr);
    tracks->Add(track);
    juce::AudioBuffer<float> rendered;
    ASSERT_NO_FATAL_FAILURE(renderTracks(*tracks, rendered, 12000));
    EXPECT_FLOAT_EQ(rendered.getSample(0, 4800), 0.0f);
    EXPECT_NEAR(rendered.getSample(0, 6240), 0.01875f, 1e-5f);
    EXPECT_NEAR(rendered.getSample(1, 6240), -0.0375f, 1e-5f);
    EXPECT_NEAR(rendered.getSample(0, 7200), 0.03125f, 1e-5f);
    EXPECT_FLOAT_EQ(rendered.getSample(0, 10000), 0.0f);
    EXPECT_FLOAT_EQ(rendered.getSample(1, 10000), 0.0f);
}

TEST_F(AudioSourceTests, TracktionRenderPreservesMonoAndGainAboveSixDecibels)
{
    const auto track = makeTrack({ std::vector<float>(4800, 0.05f) });
    track->SetVolume(3.9810717f);
    track->SetPan(-0.5f);
    const auto tracks = TrackList::Create(nullptr);
    tracks->Add(track);

    juce::AudioBuffer<float> rendered;
    ASSERT_NO_FATAL_FAILURE(renderTracks(*tracks, rendered, 4800));
    EXPECT_NEAR(rendered.getSample(0, 2400), 0.19905359f, 1e-5f);
    EXPECT_NEAR(rendered.getSample(1, 2400), 0.09952679f, 1e-5f);
}

TEST_F(AudioSourceTests, TracktionRenderKeepsEnvelopeTimeOnTrimmedAndStretchedClips)
{
    for (const double stretch : { 1.0, 1.5 }) {
        SCOPED_TRACE(stretch);
        const auto track = makeTrack({ std::vector<float>(48000, 0.25f) });
        const auto clip = *track->Intervals().begin();
        clip->SetSequenceStartTime(0.4);
        clip->StretchBy(stretch);
        ASSERT_TRUE(clip->SetTrimLeft(0.1));
        ASSERT_TRUE(clip->SetTrimRight(0.1));
        auto& envelope = clip->GetEnvelope();
        envelope.InsertOrReplace(clip->GetPlayStartTime(), 0.25);
        envelope.InsertOrReplace(clip->GetPlayEndTime(), 1.0);
        const auto tracks = TrackList::Create(nullptr);
        tracks->Add(track);
        juce::AudioBuffer<float> rendered;
        ASSERT_NO_FATAL_FAILURE(renderTracks(*tracks, rendered, 96000));
        const int first = static_cast<int>(std::ceil((clip->GetPlayStartTime() + 0.1) * 48000.0));
        const int last = static_cast<int>(std::floor((clip->GetPlayEndTime() - 0.1) * 48000.0));
        for (int frame = first; frame < last; ++frame) {
            const float expected = 0.25f * envelope.GetValue(frame / 48000.0);
            for (int channel = 0; channel < 2; ++channel) {
                ASSERT_NEAR(rendered.getSample(channel, frame), expected, 1e-4f) << frame;
            }
        }
    }
}

TEST_F(AudioSourceTests, BusRoutingKeepsFadersSendsAndExportLevels)
{
    auto& project = *reinterpret_cast<AudacityProject*>(m_project->au3ProjectPtr());
    auto& tracks = TrackList::Get(project);
    const auto source = makeTrack({ std::vector<float>(48000, 0.25f) });
    const auto bus = makeCaptureTrack(2);
    const auto finalBus = makeCaptureTrack(2);
    TrackRouting::Get(*bus).busId = "bus";
    TrackRouting::Get(*finalBus).busId = "final";
    source->SetVolume(0.5f);
    bus->SetVolume(0.25f);
    bus->SetPan(0.5f);
    finalBus->SetVolume(0.5f);
    tracks.Add(source);
    tracks.Add(bus);
    tracks.Add(finalBus);
    TrackRouting::Get(*bus).output = "final";
    TrackRouting::Get(*source).output = "bus";
    juce::AudioBuffer<float> rendered;
    ASSERT_NO_FATAL_FAILURE(renderTracks(tracks, rendered, 48000));
    EXPECT_NEAR(rendered.getSample(0, 24000), 0.0078125f, 1e-5f);
    EXPECT_NEAR(rendered.getSample(1, 24000), 0.015625f, 1e-5f);

    const auto directory = m_projectFile.getSiblingFile("routing-export-" + juce::Uuid().toString());
    DEFER { directory.deleteRecursively();
    };
    TracktionRuntime runtime;
    runtime.init(directory.getChildFile("cache").getFullPathName().toStdString(),
                 directory.getChildFile("settings").getFullPathName().toStdString());
    AudioRenderSource::Options options;
    options.stopTime = 1.0;
    options.sampleRate = 48000;
    TracktionMixSource exported(runtime, project, { source }, options);
    ASSERT_GT(exported.Process(512), 0u);
    EXPECT_NEAR(reinterpret_cast<const float*>(exported.GetBuffer(0))[256], 0.0078125f, 1e-5f);
    EXPECT_NEAR(reinterpret_cast<const float*>(exported.GetBuffer(1))[256], 0.015625f, 1e-5f);

    TrackRouting::Get(*source).output.clear();
    for (const bool preFader : { false, true }) {
        SCOPED_TRACE(preFader);
        TrackRouting::Get(*source).sends = { { "bus", 0.5, preFader } };
        ASSERT_NO_FATAL_FAILURE(renderTracks(tracks, rendered, 48000));
        const float sendLeft = preFader ? 0.0078125f : 0.00390625f;
        const float sendRight = preFader ? 0.015625f : 0.0078125f;
        EXPECT_NEAR(rendered.getSample(0, 24000), 0.125f + sendLeft, 1e-5f);
        EXPECT_NEAR(rendered.getSample(1, 24000), 0.125f + sendRight, 1e-5f);
    }
    bus->SetMute(true);
    ASSERT_NO_FATAL_FAILURE(renderTracks(tracks, rendered, 48000));
    EXPECT_NEAR(rendered.getSample(0, 24000), 0.125f, 1e-5f);
}

TEST_F(AudioSourceTests, PreFaderSendsStayAfterEffectsWhenTheChainChanges)
{
    const auto directory = m_projectFile.getSiblingFile("routing-live-effects-" + juce::Uuid().toString());
    DEFER { directory.deleteRecursively();
    };
    TracktionRuntime runtime;
    runtime.init(directory.getChildFile("cache").getFullPathName().toStdString(),
                 directory.getChildFile("settings").getFullPathName().toStdString());
    HostedPlayback playback(runtime.engine());
    const auto tracks = TrackList::Create(nullptr);
    const auto source = makeTrack({ std::vector<float>(96000, 0.125f) });
    source->SetVolume(0.0f);
    const auto bus = makeCaptureTrack(2);
    TrackRouting::Get(*bus).busId = "send-return";
    TrackRouting::Get(*source).sends = { { "send-return", 1.0, true } };
    tracks->Add(source);
    tracks->Add(bus);
    TracktionSession session(runtime, *tracks, TracktionSession::Role::Playback);
    auto& transport = session.edit().getTransport();
    transport.play(false);
    DEFER { transport.stop(false, true, false);
    };
    const auto before = playback.process(12);
    EXPECT_NEAR(before.getSample(1, before.getNumSamples() - 1), 0.125f, 1e-5f);
    auto& list = RealtimeEffectList::Get(*source);
    const auto effect = addBuiltinEffect(list, wxT("tracktion-bass-treble"));
    ASSERT_TRUE(effect);
    session.edit().dispatchPendingUpdatesSynchronously();
    const auto access = effect->GetAccess();
    access->ModifySettings([](EffectSettings& settings) {
        effects::BassTrebleEffect::GetSettings(settings).mGain = 6.020599913;
        return nullptr;
    });
    auto processing = std::async(std::launch::async, [&playback] { return playback.process(32, false); });
    access->Flush();
    const auto after = processing.get();
    EXPECT_NEAR(after.getSample(0, after.getNumSamples() - 1), 0.25f, 1e-5f);
    EXPECT_NEAR(after.getSample(1, after.getNumSamples() - 1), 0.25f, 1e-5f);
    list.RemoveState(effect);
    session.edit().dispatchPendingUpdatesSynchronously();
    const auto removed = playback.process(12);
    EXPECT_NEAR(removed.getSample(1, removed.getNumSamples() - 1), 0.125f, 1e-5f);
}

TEST_F(AudioSourceTests, SelectedAndSoloBusesIncludeTheirInputsAndMapTheBusOutput)
{
    auto& project = *reinterpret_cast<AudacityProject*>(m_project->au3ProjectPtr());
    auto& tracks = TrackList::Get(project);
    const auto first = makeTrack({ std::vector<float>(48000, 0.125f) });
    const auto second = makeTrack({ std::vector<float>(48000, 0.25f) });
    const auto bus = makeCaptureTrack(2);
    TrackRouting::Get(*bus).busId = "mix-bus";
    TrackRouting::Get(*first).output = "mix-bus";
    TrackRouting::Get(*second).output = "mix-bus";
    bus->SetVolume(0.5f);
    tracks.Add(first);
    tracks.Add(second);
    tracks.Add(bus);
    bus->SetSelected(true);
    ASSERT_EQ(ExportUtils::FindExportWaveTracks(tracks, true).size(), 3u);
    bus->SetSolo(true);
    ASSERT_EQ(ExportUtils::FindExportWaveTracks(tracks, false).size(), 3u);
    juce::AudioBuffer<float> rendered;
    ASSERT_NO_FATAL_FAILURE(renderTracks(tracks, rendered, 48000));
    EXPECT_NEAR(rendered.getSample(0, 24000), 0.1875f, 1e-5f);
    bus->SetSolo(false);
    first->SetSolo(true);
    ASSERT_EQ(ExportUtils::FindExportWaveTracks(tracks, false).size(), 2u);
    ASSERT_NO_FATAL_FAILURE(renderTracks(tracks, rendered, 48000));
    EXPECT_NEAR(rendered.getSample(0, 24000), 0.0625f, 1e-5f);
    first->SetSolo(false);
    const auto directory = m_projectFile.getSiblingFile("routing-mapped-export-" + juce::Uuid().toString());
    DEFER { directory.deleteRecursively();
    };
    TracktionRuntime runtime;
    runtime.init(directory.getChildFile("cache").getFullPathName().toStdString(),
                 directory.getChildFile("settings").getFullPathName().toStdString());
    AudioRenderSource::Factory::Scope hook([&](const AudacityProject& project, const AudioRenderSource::Tracks& sources,
                                               const AudioRenderSource::Options& options) {
        return std::make_unique<TracktionMixSource>(runtime, project, sources, options);
    });
    MixerOptions::Downmix mapping(2, 4);
    mapping.SetNumChannels(4);
    for (unsigned input = 0; input < 2; ++input) {
        for (unsigned output = 0; output < 4; ++output) {
            mapping.mMap[input][output] = output == input + 2;
        }
    }
    auto exportAudio = ExportPluginHelpers::CreateMixer(project, true, 0.0, 1.0, 4, 512, false, 48000.0, floatSample, &mapping);
    ASSERT_GT(exportAudio->Process(), 0u);
    EXPECT_FLOAT_EQ(reinterpret_cast<const float*>(exportAudio->GetBuffer(0))[256], 0.0f);
    EXPECT_FLOAT_EQ(reinterpret_cast<const float*>(exportAudio->GetBuffer(1))[256], 0.0f);
    EXPECT_NEAR(reinterpret_cast<const float*>(exportAudio->GetBuffer(2))[256], 0.1875f, 1e-5f);
    EXPECT_NEAR(reinterpret_cast<const float*>(exportAudio->GetBuffer(3))[256], 0.1875f, 1e-5f);
    exportAudio.reset();
    bus->SetSelected(false);
    first->SetSelected(true);
    ASSERT_EQ(ExportUtils::FindExportWaveTracks(tracks, true).size(), 2u);
    exportAudio = ExportPluginHelpers::CreateMixer(project, true, 0.0, 1.0, 1, 512, false, 48000.0, floatSample, nullptr);
    ASSERT_GT(exportAudio->Process(), 0u);
    EXPECT_NEAR(reinterpret_cast<const float*>(exportAudio->GetBuffer(0))[256], 0.0625f, 1e-5f);
}

TEST_F(AudioSourceTests, RoutingRejectsFeedbackAndKeepsMissingDestinationsPlayable)
{
    const auto tracks = TrackList::Create(nullptr);
    const auto first = makeCaptureTrack(2);
    const auto second = makeCaptureTrack(2);
    TrackRouting::Get(*first).busId = "first";
    TrackRouting::Get(*second).busId = "second";
    tracks->Add(first);
    tracks->Add(second);
    TrackRouting::Get(*first).output = "second";
    ASSERT_TRUE(TrackRouting::Validate(*tracks));
    TrackRouting::Get(*second).sends = { { "first", 0.0, true } };
    EXPECT_FALSE(TrackRouting::Validate(*tracks));
    TrackRouting::Get(*second).sends.clear();
    TrackRouting::Get(*second).busId = "first";
    EXPECT_FALSE(TrackRouting::Validate(*tracks));
    TrackRouting::Get(*second).busId = "second";
    TrackRouting::Get(*first).output = "deleted";
    EXPECT_TRUE(TrackRouting::Validate(*tracks));
    const auto audio = makeTrack({ std::vector<float>(4800, 0.25f) });
    TrackRouting::Get(*audio).output = "deleted";
    tracks->Add(audio);
    juce::AudioBuffer<float> rendered;
    ASSERT_NO_FATAL_FAILURE(renderTracks(*tracks, rendered, 4800));
    EXPECT_NEAR(rendered.getSample(0, 2400), 0.25f, 1e-5f);
}

TEST_F(AudioSourceTests, TracktionRenderPreservesMuteAndSolo)
{
    const auto first = makeTrack({ std::vector<float>(4800, 0.125f) });
    const auto second = makeTrack({ std::vector<float>(4800, 0.25f) });
    const auto tracks = TrackList::Create(nullptr);
    tracks->Add(first);
    tracks->Add(second);
    first->SetMute(true);

    juce::AudioBuffer<float> rendered;
    ASSERT_NO_FATAL_FAILURE(renderTracks(*tracks, rendered, 4800));
    EXPECT_NEAR(rendered.getSample(0, 2400), 0.25f, 1e-5f);
    EXPECT_NEAR(rendered.getSample(1, 2400), 0.25f, 1e-5f);

    first->SetMute(false);
    second->SetSolo(true);
    ASSERT_NO_FATAL_FAILURE(renderTracks(*tracks, rendered, 4800));
    EXPECT_NEAR(rendered.getSample(0, 2400), 0.25f, 1e-5f);
    EXPECT_NEAR(rendered.getSample(1, 2400), 0.25f, 1e-5f);
}

TEST_F(AudioSourceTests, TracktionRenderResamplesTheStoredClipRate)
{
    std::vector<float> samples(44100);
    for (size_t sample = 0; sample < samples.size(); ++sample) {
        samples[sample] = 0.2f * std::sin(2.0 * std::numbers::pi * 440.0 * sample / 44100.0);
    }
    const auto track = makeTrack({ samples });
    track->SetRate(44100);
    const auto tracks = TrackList::Create(nullptr);
    tracks->Add(track);

    juce::AudioBuffer<float> rendered;
    ASSERT_NO_FATAL_FAILURE(renderTracks(*tracks, rendered, 52800));
    int crossings = 0;
    for (int sample = 12001; sample < 36000; ++sample) {
        crossings += rendered.getSample(0, sample - 1) <= 0.0f && rendered.getSample(0, sample) > 0.0f;
    }
    EXPECT_NEAR(crossings, 220, 2);
    EXPECT_GT(rendered.getRMSLevel(0, 12000, 24000), 0.1f);
    EXPECT_FLOAT_EQ(rendered.getRMSLevel(0, 50400, 2400), 0.0f);
}

TEST_F(AudioSourceTests, TracktionRenderIncludesOnlySelectedTracksWhenRequested)
{
    const auto first = makeTrack({ std::vector<float>(4800, 0.125f) });
    const auto second = makeTrack({ std::vector<float>(4800, 0.25f) });
    const auto tracks = TrackList::Create(nullptr);
    tracks->Add(first);
    tracks->Add(second);
    first->SetSelected(false);
    first->SetSolo(true);
    second->SetSelected(true);

    juce::AudioBuffer<float> rendered;
    ASSERT_NO_FATAL_FAILURE(renderTracks(*tracks, rendered, 4800, TracktionSession::Selection::Selected));
    EXPECT_NEAR(rendered.getSample(0, 2400), 0.25f, 1e-5f);
    EXPECT_NEAR(rendered.getSample(1, 2400), 0.25f, 1e-5f);
}

TEST_F(AudioSourceTests, TracktionRenderProcessesMonoBuiltinEffectOnBothOutputChannels)
{
    const auto track = makeTrack({ std::vector<float>(4800, 0.125f) });
    const auto tracks = TrackList::Create(nullptr);
    tracks->Add(track);
    const auto state = addBuiltinEffect(RealtimeEffectList::Get(*track), wxT("tracktion-bass-treble"));
    ASSERT_TRUE(state);
    const auto access = state->GetAccess();
    access->ModifySettings([](EffectSettings& settings) {
        effects::BassTrebleEffect::GetSettings(settings).mGain = 20.0 * std::log10(2.0);
        return nullptr;
    });
    access->Flush();
    juce::AudioBuffer<float> rendered;
    ASSERT_NO_FATAL_FAILURE(renderTracks(*tracks, rendered, 4800));
    for (int channel = 0; channel < 2; ++channel) {
        for (int sample = 0; sample < 4800; ++sample) {
            ASSERT_NEAR(rendered.getSample(channel, sample), 0.25f, 1e-5f);
        }
    }
}

#if AU_TRACKTION_TEST_NYQUIST
TEST_F(AudioSourceTests, NyquistProcessingKeepsSelectionAndAudacityUndoWithTracktionPlayback)
{
    const auto directory = m_projectFile.getSiblingFile("tracktion-nyquist-" + juce::Uuid().toString());
    DEFER { directory.deleteRecursively();
    };
    const juce::File runtimeDirectory(AU_TRACKTION_TEST_NYQUIST_RUNTIME);
    ASSERT_TRUE(runtimeDirectory.getChildFile("nyquist.lsp").existsAsFile());
    const auto previousPaths = FileNames::AudacityPathList();
    FileNames::SetAudacityPathList({ wxString::FromUTF8(runtimeDirectory.getParentDirectory().getFullPathName().toRawUTF8()) });
    DEFER { FileNames::SetAudacityPathList(previousPaths);
    };
    NyquistEffectsModule module;
    ASSERT_TRUE(module.Initialize());
    DEFER { module.Terminate();
    };

    auto& project = *reinterpret_cast<au3::Au3Project*>(m_project->au3ProjectPtr());
    auto& tracks = TrackList::Get(project);
    const auto track = makeTrack({ std::vector<float>(4800, 0.25f), std::vector<float>(4800, -0.5f) });
    track->SetSelected(true);
    tracks.Add(track);
    auto& history = ProjectHistory::Get(project);
    history.InitialState();
    auto& undo = UndoManager::Get(project);
    const auto initialState = undo.GetCurrentState();

    TracktionRuntime runtime;
    runtime.init(directory.getChildFile("cache").getFullPathName().toStdString(),
                 directory.getChildFile("settings").getFullPathName().toStdString());
    HostedPlayback playback(runtime.engine());
    const EffectPlugin::ProcessingInstanceFactory::Scope factory([&](const EffectPlugin& effect) {
        return createTracktionEffectInstance(runtime, effect);
    });
    const wxString script = wxT("$nyquist plug-in\n$version 4\n$type process\n$name \"Test gain\"\n")
                            wxT("$control gain \"Gain\" real \"\" 1.0 0.0 2.0\n(mult *track* gain)\n");
    NyquistBase definition(NYQUIST_WORKER_ID);
    definition.SetCommand(script);
    auto definitionSettings = definition.MakeSettings();
    CommandParameters requested;
    requested.Write(wxT("gain"), 0.5);
    ASSERT_TRUE(definition.LoadSettings(requested, definitionSettings));
    CommandParameters saved;
    ASSERT_TRUE(definition.SaveSettings(definitionSettings, saved));
    wxString encoded;
    ASSERT_TRUE(saved.GetParameters(encoded));
    NyquistBase effect(NYQUIST_WORKER_ID);
    effect.SetCommand(script);
    ASSERT_TRUE(effect.IsOk());
    effect.SetTracks(&tracks);
    DEFER { effect.SetTracks(nullptr);
    };
    effect.mFactory = &WaveTrackFactory::Get(project);
    effect.mProjectRate = 48000.0;
    effect.mT0 = 101.0 / 48000.0;
    effect.mT1 = 4703.0 / 48000.0;
    effect.CountWaveTracks();
    const auto editor = std::dynamic_pointer_cast<EffectInstanceEx>(effect.MakeInstance());
    ASSERT_TRUE(editor);
    const auto processor = std::dynamic_pointer_cast<EffectInstanceEx>(effect.MakeProcessingInstance(editor));
    ASSERT_EQ(processor, editor);
    ASSERT_TRUE(processor->Init());
    auto settings = effect.MakeSettings();
    ASSERT_TRUE(effect.LoadSettings(CommandParameters(encoded), settings));
    const auto checkAudio = [&](TrackList& source, float gain, float outsideGain) {
        TracktionSession session(runtime, source, TracktionSession::Role::Playback);
        ASSERT_TRUE(session.startPlayback());
        const auto audio = playback.process(20);
        for (int channel = 0; channel < 2; ++channel) {
            const float original = channel == 0 ? 0.25f : -0.5f;
            EXPECT_NEAR(audio.getSample(channel, 100), original * outsideGain, 1e-5f);
            EXPECT_NEAR(audio.getSample(channel, 1000), original * gain, 1e-5f);
            EXPECT_NEAR(audio.getSample(channel, 4703), original * outsideGain, 1e-5f);
        }
        session.edit().getTransport().stop(false, true, false);
    };

    {
        const auto previewTracks = TrackList::Create(&project);
        const auto previewTrack = std::static_pointer_cast<WaveTrack>(track->Copy(effect.mT0, effect.mT1));
        previewTrack->ShiftBy(0.0, effect.mT0);
        previewTrack->SetSelected(true);
        previewTracks->Add(previewTrack);
        effect.SetTracks(previewTracks.get());
        effect.mIsPreview = true;
        effect.CountWaveTracks();
        ASSERT_TRUE(processor->Process(settings)) << processor->GetLastError();
        ASSERT_NO_FATAL_FAILURE(checkAudio(*previewTracks, 0.5f, 0.0f));
        EXPECT_EQ(*tracks.Any<WaveTrack>().begin(), track.get());
        EXPECT_EQ(undo.GetCurrentState(), initialState);
        std::vector<float> unchanged(4800);
        float* data[] { unchanged.data() };
        ASSERT_TRUE(track->GetFloats(0, 1, data, sampleCount { 0 }, unchanged.size()));
        EXPECT_EQ(unchanged, std::vector<float>(4800, 0.25f));
        effect.SetTracks(&tracks);
        effect.mIsPreview = false;
        effect.CountWaveTracks();
    }
    ASSERT_TRUE(processor->Process(settings)) << processor->GetLastError();
    EXPECT_EQ(undo.GetCurrentState(), initialState);
    history.PushState(::TranslatableString::untranslatable("Test gain"), ::TranslatableString::untranslatable("Test gain"));
    EXPECT_EQ(undo.GetCurrentState(), initialState + 1);

    ASSERT_NO_FATAL_FAILURE(checkAudio(tracks, 0.5f, 1.0f));
    ASSERT_TRUE(history.UndoAvailable());
    undo.Undo([&](const UndoStackElem& element) { history.PopState(element.state); });
    EXPECT_EQ(undo.GetCurrentState(), initialState);
    ASSERT_NO_FATAL_FAILURE(checkAudio(tracks, 1.0f, 1.0f));
    ASSERT_TRUE(history.RedoAvailable());
    undo.Redo([&](const UndoStackElem& element) { history.PopState(element.state); });
    EXPECT_EQ(undo.GetCurrentState(), initialState + 1);
    ASSERT_NO_FATAL_FAILURE(checkAudio(tracks, 0.5f, 1.0f));
}
#endif

#if AU_TRACKTION_TEST_VST3
TEST_F(AudioSourceTests, TrackAutomationUsesTheSameVolumeAndPanInPlaybackAndExport)
{
    for (const int channelCount : { 1, 2 }) {
        SCOPED_TRACE(channelCount);
        const auto track = makeTrack(std::vector<std::vector<float> >(channelCount, std::vector<float>(96000, 0.25f)));
        track->SetVolume(0.1f);
        track->SetPan(0.8f);
        auto& data = TrackAutomation::Get(*track);
        ASSERT_TRUE(data.SetLane({ "volume", true, { { 0.0, 0.5 / TrackAutomation::MaximumVolume, 0.0 },
                                       { 1.0, 1.0 / TrackAutomation::MaximumVolume, 0.0 },
                                       { 2.0, 2.0 / TrackAutomation::MaximumVolume, 0.0 } } }));
        ASSERT_TRUE(data.SetLane({ "pan", true, { { 0.0, 0.0, 0.0 }, { 2.0, 1.0, 0.0 } } }));
        const auto tracks = TrackList::Create(nullptr);
        tracks->Add(track);
        juce::AudioBuffer<float> rendered;
        renderTracks(*tracks, rendered, 96000);
        EXPECT_NEAR(rendered.getSample(0, 24000), 0.1875f, 0.001f);
        EXPECT_NEAR(rendered.getSample(1, 24000), 0.09375f, 0.001f);
        EXPECT_NEAR(rendered.getSample(0, 72000), 0.1875f, 0.001f);
        EXPECT_NEAR(rendered.getSample(1, 72000), 0.375f, 0.001f);
        const auto directory = m_projectFile.getSiblingFile("automation-export-" + juce::Uuid().toString());
        DEFER { directory.deleteRecursively();
        };
        TracktionRuntime runtime;
        runtime.init(directory.getChildFile("cache").getFullPathName().toStdString(),
                     directory.getChildFile("settings").getFullPathName().toStdString());
        AudioRenderSource::Options options;
        options.stopTime = 2.0;
        options.sampleRate = 48000;
        options.channels = 2;
        auto& project = *reinterpret_cast<AudacityProject*>(m_project->au3ProjectPtr());
        TracktionMixSource exportSource(runtime, project, { track }, options);
        std::vector<float> exported[2];
        while (const auto count = exportSource.Process()) {
            for (int channel = 0; channel < 2; ++channel) {
                const auto* samples = reinterpret_cast<const float*>(exportSource.GetBuffer(channel));
                exported[channel].insert(exported[channel].end(), samples, samples + count);
            }
        }
        ASSERT_EQ(exported[0].size(), 96000u);
        for (int channel = 0; channel < 2; ++channel) {
            EXPECT_NEAR(exported[channel][24000], rendered.getSample(channel, 24000), 1e-6f);
            EXPECT_NEAR(exported[channel][72000], rendered.getSample(channel, 72000), 1e-6f);
        }
        options.channels = 1;
        options.channelMode = AudioRenderSource::ChannelMode::Mixdown;
        TracktionMixSource monoExport(runtime, project, { track }, options);
        std::vector<float> mono;
        while (const auto count = monoExport.Process()) {
            const auto* samples = reinterpret_cast<const float*>(monoExport.GetBuffer(0));
            mono.insert(mono.end(), samples, samples + count);
        }
        ASSERT_EQ(mono.size(), 96000u);
        EXPECT_NEAR(mono[24000], (rendered.getSample(0, 24000) + rendered.getSample(1, 24000)) / 2, 1e-6f);
        EXPECT_NEAR(mono[72000], (rendered.getSample(0, 72000) + rendered.getSample(1, 72000)) / 2, 1e-6f);
    }
}

TEST_F(AudioSourceTests, ExternalEffectAutomationReachesEveryAudacityChannel)
{
    for (const auto* file : { AU_TRACKTION_TEST_VST3_PATH, AU_TRACKTION_TEST_VST3_MONO_PATH }) {
        SCOPED_TRACE(file);
        const auto component = loadTestVst3(file);
        const auto* effect = dynamic_cast<const VST3EffectBase*>(component.get());
        ASSERT_TRUE(effect);
        const RealtimeEffectState::EffectFactory::Scope factory([&](const PluginID&) -> const EffectInstanceFactory* { return effect; });
        const auto directory = m_projectFile.getSiblingFile("automation-effect-" + juce::Uuid().toString());
        DEFER { directory.deleteRecursively();
        };
        TracktionRuntime runtime;
        runtime.init(directory.getChildFile("cache").getFullPathName().toStdString(),
                     directory.getChildFile("settings").getFullPathName().toStdString());
        const EffectInstanceFactory::EditorInstanceFactory::Scope editorFactory([&](const EffectInstanceFactory& effect) {
            return createTracktionEffectInstance(runtime, effect);
        });
        const auto track = makeTrack({ std::vector<float>(9600, 0.25f), std::vector<float>(9600, 0.5f) });
        const auto tracks = TrackList::Create(nullptr);
        tracks->Add(track);
        const auto state = std::make_shared<RealtimeEffectState>(wxT("automation-vst3"));
        ASSERT_TRUE(RealtimeEffectList::Get(*track).AddState(state));
        TracktionInstrumentEditorFactory parameterFactory;
        const auto parameters = parameterFactory.parameters(*state);
        const auto gain
            = std::find_if(parameters.begin(), parameters.end(), [](const auto& parameter) { return parameter.name == "Gain"; });
        ASSERT_NE(gain, parameters.end());
        ASSERT_TRUE(state->Automation().SetLane({ gain->id, true, { { 0.0, 0.25, 0.0 }, { 0.2, 0.75, 0.0 } } }));
        juce::AudioBuffer<float> rendered;
        renderTracks(*tracks, rendered, 9600);
        ASSERT_EQ(rendered.getNumSamples(), 9600);
        EXPECT_GT(rendered.getSample(0, 7200), rendered.getSample(0, 2400) * 1.5f);
        EXPECT_NEAR(rendered.getSample(1, 2400), rendered.getSample(0, 2400) * 2.0f, 0.002f);
        EXPECT_NEAR(rendered.getSample(1, 7200), rendered.getSample(0, 7200) * 2.0f, 0.002f);
        const auto second = std::make_shared<RealtimeEffectState>(wxT("automation-vst3"));
        ASSERT_TRUE(RealtimeEffectList::Get(*track).AddState(second));
        RealtimeEffectList::Get(*track).MoveEffect(0, 1);
        EXPECT_EQ(RealtimeEffectList::Get(*track).GetStateAt(1)->Automation().Lanes(), state->Automation().Lanes());
        EXPECT_TRUE(RealtimeEffectList::Get(*track).GetStateAt(0)->Automation().Lanes().empty());
    }
}

TEST_F(AudioSourceTests, TracktionVst3RenderKeepsAudacityChannelAssignment)
{
    struct Case {
        const char* file;
        int channels;
        float left;
        float right;
    };
    const Case cases[] {
        { AU_TRACKTION_TEST_VST3_MONO_PATH, 2, 0.125f, -0.0625f },
        { AU_TRACKTION_TEST_VST3_STEREO_PATH, 1, 0.125f, 0.125f },
        { AU_TRACKTION_TEST_VST3_MONO_STEREO_PATH, 2, 0.125f, -0.125f },
    };
    for (const auto& test : cases) {
        SCOPED_TRACE(test.file);
        const auto component = loadTestVst3(test.file);
        const auto* effect = dynamic_cast<const VST3EffectBase*>(component.get());
        ASSERT_TRUE(effect);
        const RealtimeEffectState::EffectFactory::Scope factory([&](const PluginID&) -> const EffectInstanceFactory* {
            return effect;
        });
        std::vector<std::vector<float> > samples(test.channels, std::vector<float>(4800, 0.25f));
        if (test.channels == 2) {
            std::fill(samples[1].begin(), samples[1].end(), -0.125f);
        }
        const auto track = makeTrack(samples);
        const auto tracks = TrackList::Create(nullptr);
        tracks->Add(track);
        const auto state = std::make_shared<RealtimeEffectState>(wxT("tracktion-vst3-channel-test"));
        ASSERT_TRUE(RealtimeEffectList::Get(*track).AddState(state));
        juce::AudioBuffer<float> rendered;
        ASSERT_NO_FATAL_FAILURE(renderTracks(*tracks, rendered, 4800));
        for (int channel = 0; channel < 2; ++channel) {
            const float expected = channel == 0 ? test.left : test.right;
            for (int sample = 0; sample < 4800; ++sample) {
                ASSERT_NEAR(rendered.getSample(channel, sample), expected, 1e-5f);
            }
        }
        EXPECT_FALSE(state->PeekInstance());
    }
}

TEST_F(AudioSourceTests, TracktionVst3MonoProcessorsFollowLiveSettingsAndBypass)
{
    const auto component = loadTestVst3(AU_TRACKTION_TEST_VST3_MONO_PATH);
    const auto* effect = dynamic_cast<const VST3EffectBase*>(component.get());
    ASSERT_TRUE(effect);
    const auto uiInstance = makeVst3Wrapper(*effect);
    ASSERT_TRUE(uiInstance);
    const auto gainID = vst3GainID(*uiInstance);
    const RealtimeEffectState::EffectFactory::Scope factory([&](const PluginID&) -> const EffectInstanceFactory* {
        return effect;
    });
    auto& project = *reinterpret_cast<au3::Au3Project*>(m_project->au3ProjectPtr());
    auto& tracks = TrackList::Get(project);
    const auto track = makeTrack({ std::vector<float>(480000, 0.25f), std::vector<float>(480000, 0.375f) });
    tracks.Add(track);
    const auto trackState = std::make_shared<RealtimeEffectState>(wxT("tracktion-vst3-mono-track"));
    const auto masterState = std::make_shared<RealtimeEffectState>(wxT("tracktion-vst3-mono-master"));
    auto& trackEffects = RealtimeEffectList::Get(*track);
    auto& masterEffects = RealtimeEffectList::Get(project);
    ASSERT_TRUE(trackEffects.AddState(trackState));
    ASSERT_TRUE(masterEffects.AddState(masterState));
    const auto directory = m_projectFile.getSiblingFile("tracktion-mono-playback-" + juce::Uuid().toString());
    DEFER { directory.deleteRecursively();
    };
    TracktionRuntime runtime;
    runtime.init(directory.getChildFile("cache").getFullPathName().toStdString(),
                 directory.getChildFile("settings").getFullPathName().toStdString());
    const EffectInstanceFactory::EditorInstanceFactory::Scope editorFactory([&](const EffectInstanceFactory& plugin) {
        return createTracktionEffectInstance(runtime, plugin);
    });
    HostedPlayback playback(runtime.engine());
    TracktionSession session(runtime, tracks, TracktionSession::Role::Playback);
    for (auto* plugin : tracktion::getAudioTracks(session.edit())[0]->pluginList) {
        auto* external = dynamic_cast<tracktion::ExternalPlugin*>(plugin);
        if (external) {
            juce::StringArray inputs, outputs;
            external->getChannelNames(&inputs, &outputs);
            EXPECT_EQ(inputs.size(), 2);
            EXPECT_EQ(outputs.size(), 2);
            EXPECT_FALSE(external->canSidechain());
        }
    }
    auto& transport = session.edit().getTransport();
    transport.play(false);
    const auto checkGain = [&](float gain) {
        const auto rendered = playback.process(64);
        for (int channel = 0; channel < 2; ++channel) {
            const float input = channel == 0 ? 0.25f : 0.375f;
            for (int sample = 8192; sample < 16384; ++sample) {
                ASSERT_NEAR(rendered.getSample(channel, sample), input * gain, 1e-5f);
            }
        }
    };
    ASSERT_NO_FATAL_FAILURE(checkGain(0.25f));
    const auto access = trackState->GetAccess();
    access->ModifySettings([&](EffectSettings& settings) {
        setVst3Gain(*effect, settings, gainID, 0.125f);
        return nullptr;
    });
    access->Flush();
    ASSERT_NO_FATAL_FAILURE(checkGain(0.0625f));
    trackState->SetActive(false);
    ASSERT_NO_FATAL_FAILURE(checkGain(0.5f));
    trackState->SetActive(true);
    masterState->SetActive(false);
    ASSERT_NO_FATAL_FAILURE(checkGain(0.125f));
    trackEffects.SetActive(false);
    ASSERT_NO_FATAL_FAILURE(checkGain(1.0f));
    transport.stop(false, true, false);
    EXPECT_TRUE(std::dynamic_pointer_cast<TracktionHostedEffectInstance>(trackState->PeekInstance()));
    EXPECT_TRUE(std::dynamic_pointer_cast<TracktionHostedEffectInstance>(masterState->PeekInstance()));
}

TEST_F(AudioSourceTests, TracktionVst3MasterProcessorsKeepFourChannelExportAssignment)
{
    struct Case {
        const char* file;
        std::array<float, 4> expected;
    };
    const Case cases[] {
        { AU_TRACKTION_TEST_VST3_MONO_PATH, { 0.125f, 0.1875f, 0.125f, 0.1875f } },
        { AU_TRACKTION_TEST_VST3_STEREO_PATH, { 0.125f, -0.1875f, 0.125f, -0.1875f } },
        { AU_TRACKTION_TEST_VST3_MONO_STEREO_PATH, { 0.125f, -0.125f, 0.1875f, -0.1875f } },
    };
    auto& project = *reinterpret_cast<au3::Au3Project*>(m_project->au3ProjectPtr());
    auto& tracks = TrackList::Get(project);
    const auto inputTrack = makeTrack({ std::vector<float>(4800, 0.25f), std::vector<float>(4800, 0.375f) });
    tracks.Add(inputTrack);
    const auto bus = makeCaptureTrack(2);
    TrackRouting::Get(*bus).busId = "master-export-bus";
    for (const bool routed : { false, true }) {
        SCOPED_TRACE(routed);
        AudioRenderSource::Tracks renderTracks { inputTrack };
        if (routed) {
            tracks.Add(bus);
            TrackRouting::Get(*inputTrack).output = "master-export-bus";
            renderTracks.push_back(bus);
        }
        for (const auto& test : cases) {
            SCOPED_TRACE(test.file);
            const auto component = loadTestVst3(test.file);
            const auto* effect = dynamic_cast<const VST3EffectBase*>(component.get());
            ASSERT_TRUE(effect);
            const RealtimeEffectState::EffectFactory::Scope factory([&](const PluginID&) -> const EffectInstanceFactory* {
                return effect;
            });
            const auto state = std::make_shared<RealtimeEffectState>(wxT("tracktion-vst3-master-channel-test"));
            auto& master = RealtimeEffectList::Get(project);
            ASSERT_TRUE(master.AddState(state));
            DEFER { master.RemoveState(state);
            };
            const auto directory = m_projectFile.getSiblingFile("tracktion-mono-export-" + juce::Uuid().toString());
            DEFER { directory.deleteRecursively();
            };
            TracktionRuntime runtime;
            runtime.init(directory.getChildFile("cache").getFullPathName().toStdString(),
                         directory.getChildFile("settings").getFullPathName().toStdString());
            MixerOptions::Downmix mapping(2, 4);
            ASSERT_TRUE(mapping.SetNumChannels(4));
            for (int input = 0; input < 2; ++input) {
                for (int output = 0; output < 4; ++output) {
                    mapping.mMap[input][output] = input == output % 2;
                }
            }
            AudioRenderSource::Options options;
            options.stopTime = 0.1;
            options.channels = 4;
            options.bufferSize = 257;
            options.sampleRate = 48000.0;
            options.channelMap = &mapping;
            options.masterEffects = true;
            TracktionMixSource source(runtime, project, renderTracks, options);
            size_t total = 0;
            while (const auto frames = source.Process()) {
                for (int channel = 0; channel < 4; ++channel) {
                    const auto* samples = reinterpret_cast<const float*>(source.GetBuffer(channel));
                    for (size_t sample = 0; sample < frames; ++sample) {
                        ASSERT_NEAR(samples[sample], test.expected[channel], 1e-5f);
                    }
                }
                total += frames;
            }
            EXPECT_EQ(total, 4800u);
        }
    }
}

TEST_F(AudioSourceTests, TracktionOfflineVst3KeepsAudacityChannelsSelectionAndLatency)
{
    const auto directory = m_projectFile.getSiblingFile("tracktion-offline-vst3-" + juce::Uuid().toString());
    DEFER { directory.deleteRecursively();
    };
    TracktionRuntime runtime;
    runtime.init(directory.getChildFile("cache").getFullPathName().toStdString(),
                 directory.getChildFile("settings").getFullPathName().toStdString());
    for (const auto* file : { AU_TRACKTION_TEST_VST3_PATH, AU_TRACKTION_TEST_VST3_MONO_PATH,
                              AU_TRACKTION_TEST_VST3_STEREO_PATH, AU_TRACKTION_TEST_VST3_MONO_STEREO_PATH }) {
        SCOPED_TRACE(file);
        const auto component = loadTestVst3(file);
        auto* effect = dynamic_cast<VST3EffectBase*>(component.get());
        ASSERT_TRUE(effect);
        const auto wrapper = makeVst3Wrapper(*effect);
        ASSERT_TRUE(wrapper);
        auto settings = effect->MakeSettings();
        setVst3Gain(*effect, settings, vst3GainID(*wrapper), 0.25f);
        for (const int channels : { 1, 2 }) {
            SCOPED_TRACE(channels);
            std::vector<std::vector<float> > input(channels, std::vector<float>(4800));
            for (int channel = 0; channel < channels; ++channel) {
                for (size_t frame = 0; frame < input[channel].size(); ++frame) {
                    input[channel][frame] = (channel + 1) * ((frame % 101) / 404.0f);
                }
            }
            juce::AudioBuffer<float> expected;
            ASSERT_NO_FATAL_FAILURE(applyOfflineEffect(*effect, settings, input, expected));
            size_t processors = 0;
            const EffectPlugin::ProcessingInstanceFactory::Scope factory([&](const EffectPlugin& plugin) {
                ++processors;
                return createTracktionEffectInstance(runtime, plugin);
            });
            juce::AudioBuffer<float> actual;
            ASSERT_NO_FATAL_FAILURE(applyOfflineEffect(*effect, settings, input, actual));
            EXPECT_GT(processors, 0u);
            for (int channel = 0; channel < channels; ++channel) {
                for (int frame = 0; frame < actual.getNumSamples(); ++frame) {
                    ASSERT_NEAR(actual.getSample(channel, frame), expected.getSample(channel, frame), 1e-6f) << channel << ":" << frame;
                    if (frame < 101 || frame >= actual.getNumSamples() - 97) {
                        ASSERT_FLOAT_EQ(actual.getSample(channel, frame), input[channel][frame]);
                    }
                }
            }
        }
    }
}

TEST_F(AudioSourceTests, TracktionOfflineVst3KeepsBlockSizeAndLatencyOptions)
{
    const auto component = loadTestVst3();
    auto* effect = dynamic_cast<VST3EffectBase*>(component.get());
    ASSERT_TRUE(effect);
    bool previousLatency = true;
    int previousBlockSize = 8192;
    GetConfig(*effect, PluginSettings::Shared, wxT("Options"), wxT("UseLatency"), previousLatency, true);
    GetConfig(*effect, PluginSettings::Shared, wxT("Options"), wxT("BufferSize"), previousBlockSize, 8192);
    DEFER {
        SetConfig(*effect, PluginSettings::Shared, wxT("Options"), wxT("UseLatency"), previousLatency);
        SetConfig(*effect, PluginSettings::Shared, wxT("Options"), wxT("BufferSize"), previousBlockSize);
    };
    const auto directory = m_projectFile.getSiblingFile("tracktion-offline-options-" + juce::Uuid().toString());
    DEFER { directory.deleteRecursively();
    };
    TracktionRuntime runtime;
    runtime.init(directory.getChildFile("cache").getFullPathName().toStdString(),
                 directory.getChildFile("settings").getFullPathName().toStdString());
    auto settings = effect->MakeSettings();
    const std::vector<std::vector<float> > input(2, std::vector<float>(4800, 0.25f));
    for (const int blockSize : { 1, 257, 8192 }) {
        SCOPED_TRACE(blockSize);
        ASSERT_TRUE(SetConfig(*effect, PluginSettings::Shared, wxT("Options"), wxT("BufferSize"), blockSize));
        for (const bool useLatency : { false, true }) {
            SCOPED_TRACE(useLatency);
            ASSERT_TRUE(SetConfig(*effect, PluginSettings::Shared, wxT("Options"), wxT("UseLatency"), useLatency));
            juce::AudioBuffer<float> expected;
            ASSERT_NO_FATAL_FAILURE(applyOfflineEffect(*effect, settings, input, expected));
            const EffectPlugin::ProcessingInstanceFactory::Scope factory([&](const EffectPlugin& plugin) {
                auto instance = createTracktionEffectInstance(runtime, plugin);
                EXPECT_EQ(instance->SetBlockSize(10000), blockSize);
                return instance;
            });
            juce::AudioBuffer<float> actual;
            ASSERT_NO_FATAL_FAILURE(applyOfflineEffect(*effect, settings, input, actual));
            for (int channel = 0; channel < actual.getNumChannels(); ++channel) {
                for (int frame = 0; frame < actual.getNumSamples(); ++frame) {
                    ASSERT_NEAR(actual.getSample(channel, frame), expected.getSample(channel, frame), 1e-6f) << frame;
                }
                EXPECT_FLOAT_EQ(actual.getSample(channel, 101), useLatency ? 0.125f : 0.0f);
                EXPECT_FLOAT_EQ(actual.getSample(channel, 134), 0.125f);
            }
        }
    }
}

TEST_F(AudioSourceTests, TracktionOfflineVst3RestoresSavedComponentState)
{
    const auto component = loadTestVst3();
    auto* effect = dynamic_cast<VST3EffectBase*>(component.get());
    ASSERT_TRUE(effect);
    const auto wrapper = makeVst3Wrapper(*effect);
    ASSERT_TRUE(wrapper);
    auto settings = effect->MakeSettings();
    setVst3Gain(*effect, settings, vst3GainID(*wrapper), 0.75f);
    ASSERT_TRUE(wrapper->Initialize(settings, 48000.0, Steinberg::Vst::kOffline, 512));
    wrapper->Finalize(&settings);
    CommandParameters parameters;
    ASSERT_TRUE(effect->SaveSettings(settings, parameters));
    ASSERT_TRUE(parameters.HasEntry("ProcessorState"));
    EXPECT_FALSE(parameters.HasEntry("Parameters"));
    const std::vector<std::vector<float> > input(2, std::vector<float>(4800, 0.25f));
    juce::AudioBuffer<float> expected;
    ASSERT_NO_FATAL_FAILURE(applyOfflineEffect(*effect, settings, input, expected));
    const auto directory = m_projectFile.getSiblingFile("tracktion-offline-state-" + juce::Uuid().toString());
    DEFER { directory.deleteRecursively();
    };
    TracktionRuntime runtime;
    runtime.init(directory.getChildFile("cache").getFullPathName().toStdString(),
                 directory.getChildFile("settings").getFullPathName().toStdString());
    const EffectPlugin::ProcessingInstanceFactory::Scope factory([&](const EffectPlugin& plugin) {
        return createTracktionEffectInstance(runtime, plugin);
    });
    juce::AudioBuffer<float> actual;
    ASSERT_NO_FATAL_FAILURE(applyOfflineEffect(*effect, settings, input, actual));
    for (int channel = 0; channel < actual.getNumChannels(); ++channel) {
        for (int frame = 0; frame < actual.getNumSamples(); ++frame) {
            ASSERT_NEAR(actual.getSample(channel, frame), expected.getSample(channel, frame), 1e-6f) << frame;
        }
        EXPECT_FLOAT_EQ(actual.getSample(channel, 101), 0.1875f);
    }
}

TEST_F(AudioSourceTests, TracktionOfflineFactoryKeepsBuiltinInstancesAndReportsExternalLoadFailure)
{
    const auto directory = m_projectFile.getSiblingFile("tracktion-offline-factory-" + juce::Uuid().toString());
    DEFER { directory.deleteRecursively();
    };
    TracktionRuntime runtime;
    runtime.init(directory.getChildFile("cache").getFullPathName().toStdString(),
                 directory.getChildFile("settings").getFullPathName().toStdString());
    const EffectPlugin::ProcessingInstanceFactory::Scope factory([&](const EffectPlugin& plugin) {
        return createTracktionEffectInstance(runtime, plugin);
    });
    effects::BassTrebleEffect builtin;
    const auto current = builtin.MakeInstance();
    EXPECT_EQ(builtin.MakeProcessingInstance(current), current);

    class MissingExternalEffect final : public effects::BassTrebleEffect
    {
    public:
        EffectFamilySymbol GetFamily() const override { return wxT("VST3"); }
        PluginPath GetPath() const override { return wxT("missing;invalid"); }
    } missing;
    const auto dialogInstance = missing.MakeInstance();
    const auto processing = std::dynamic_pointer_cast<EffectInstanceEx>(missing.MakeProcessingInstance(dialogInstance));
    ASSERT_TRUE(processing);
    EXPECT_NE(processing, dialogInstance);
    EXPECT_FALSE(processing->Init());
    EXPECT_FALSE(processing->GetLastError().empty());
}

TEST_F(AudioSourceTests, TracktionOfflineVst3KeepsProjectAudioWhenCancelled)
{
    class CancelProgress final : public BasicUI::ProgressDialog
    {
    public:
        BasicUI::ProgressResult Poll(unsigned long long, unsigned long long, const TranslatableString&) override
        {
            return BasicUI::ProgressResult::Cancelled;
        }

        bool Cancelled() const override { return true; }
        void SetMessage(const TranslatableString&) override {}
        void SetDialogTitle(const TranslatableString&) override {}
        void Reinit() override {}
    } progress;
    const auto component = loadTestVst3();
    auto* effect = dynamic_cast<VST3EffectBase*>(component.get());
    ASSERT_TRUE(effect);
    effect->mProgress = &progress;
    DEFER { effect->mProgress = nullptr;
    };
    const auto directory = m_projectFile.getSiblingFile("tracktion-offline-cancel-" + juce::Uuid().toString());
    DEFER { directory.deleteRecursively();
    };
    TracktionRuntime runtime;
    runtime.init(directory.getChildFile("cache").getFullPathName().toStdString(),
                 directory.getChildFile("settings").getFullPathName().toStdString());
    const EffectPlugin::ProcessingInstanceFactory::Scope factory([&](const EffectPlugin& plugin) {
        return createTracktionEffectInstance(runtime, plugin);
    });
    auto settings = effect->MakeSettings();
    const std::vector<std::vector<float> > input(2, std::vector<float>(4800, 0.25f));
    juce::AudioBuffer<float> output;
    ASSERT_NO_FATAL_FAILURE(applyOfflineEffect(*effect, settings, input, output, false));
    for (int channel = 0; channel < output.getNumChannels(); ++channel) {
        for (int frame = 0; frame < output.getNumSamples(); ++frame) {
            ASSERT_FLOAT_EQ(output.getSample(channel, frame), input[channel][frame]);
        }
    }
}

TEST_F(AudioSourceTests, TracktionVst3RenderRestoresParametersAndCompensatesLatency)
{
    const auto component = loadTestVst3();
    const auto* effect = dynamic_cast<const VST3EffectBase*>(component.get());
    ASSERT_TRUE(effect);
    const auto uiInstance = makeVst3Wrapper(*effect);
    ASSERT_TRUE(uiInstance);
    const auto gainID = vst3GainID(*uiInstance);
    const RealtimeEffectState::EffectFactory::Scope factory([&](const PluginID&) -> const EffectInstanceFactory* {
        return effect;
    });
    for (const int channels : { 1, 2 }) {
        SCOPED_TRACE(channels);
        const auto tracks = TrackList::Create(nullptr);
        const auto track = makeTrack(std::vector<std::vector<float> >(channels, std::vector<float>(4800, 0.25f)));
        tracks->Add(track);
        const auto state = std::make_shared<RealtimeEffectState>(wxT("tracktion-vst3-test"));
        ASSERT_TRUE(RealtimeEffectList::Get(*track).AddState(state));
        const auto access = state->GetAccess();
        access->ModifySettings([&](EffectSettings& settings) {
            setVst3Gain(*effect, settings, gainID, 0.25f);
            return nullptr;
        });
        access->Flush();
        juce::AudioBuffer<float> rendered;
        ASSERT_NO_FATAL_FAILURE(renderTracks(*tracks, rendered, 4800));
        for (int channel = 0; channel < 2; ++channel) {
            for (int sample = 0; sample < 4800; ++sample) {
                ASSERT_NEAR(rendered.getSample(channel, sample), 0.0625f, 1e-5f);
            }
        }
        EXPECT_FALSE(state->PeekInstance());
    }
}

TEST_F(AudioSourceTests, TracktionVst3RenderRestoresAudacityComponentState)
{
    const auto component = loadTestVst3();
    const auto* effect = dynamic_cast<const VST3EffectBase*>(component.get());
    ASSERT_TRUE(effect);
    const auto uiInstance = makeVst3Wrapper(*effect);
    ASSERT_TRUE(uiInstance);
    const auto gainID = vst3GainID(*uiInstance);
    const RealtimeEffectState::EffectFactory::Scope factory([&](const PluginID&) -> const EffectInstanceFactory* {
        return effect;
    });
    const auto tracks = TrackList::Create(nullptr);
    const auto track = makeTrack({ std::vector<float>(4800, 0.25f), std::vector<float>(4800, -0.125f) });
    tracks->Add(track);
    const auto state = std::make_shared<RealtimeEffectState>(wxT("tracktion-vst3-test"));
    ASSERT_TRUE(RealtimeEffectList::Get(*track).AddState(state));
    const auto access = state->GetAccess();
    for (const float gain : { 0.25f, 0.75f }) {
        SCOPED_TRACE(gain);
        auto savedSettings = effect->MakeSettings();
        setVst3Gain(*effect, savedSettings, gainID, gain);
        auto& wrapper = *uiInstance;
        ASSERT_TRUE(wrapper.Initialize(savedSettings, 48000.0, Steinberg::Vst::kOffline, 512));
        wrapper.Finalize(&savedSettings);
        CommandParameters parameters;
        ASSERT_TRUE(effect->SaveSettings(savedSettings, parameters));
        ASSERT_TRUE(parameters.HasEntry("ProcessorState"));
        EXPECT_FALSE(parameters.HasEntry("Parameters"));
        access->ModifySettings([&](EffectSettings& settings) {
            EXPECT_TRUE(effect->LoadSettings(parameters, settings));
            return nullptr;
        });
        access->Flush();
        juce::AudioBuffer<float> rendered;
        ASSERT_NO_FATAL_FAILURE(renderTracks(*tracks, rendered, 4800));
        for (int channel = 0; channel < 2; ++channel) {
            const float input = channel == 0 ? 0.25f : -0.125f;
            for (int sample = 0; sample < 4800; ++sample) {
                ASSERT_NEAR(rendered.getSample(channel, sample), input * gain, 1e-5f);
            }
        }
    }
    EXPECT_FALSE(state->PeekInstance());
}

TEST_F(AudioSourceTests, TracktionPluginClockUsesProjectTempoWithoutMovingAudio)
{
    auto& project = *reinterpret_cast<au3::Au3Project*>(m_project->au3ProjectPtr());
    auto& signature = ProjectTimeSignature::Get(project);
    const auto oldTempo = signature.GetTempo();
    const auto oldUpper = signature.GetUpperTimeSignature();
    const auto oldLower = signature.GetLowerTimeSignature();
    DEFER {
        signature.SetTempo(oldTempo);
        signature.SetUpperTimeSignature(oldUpper);
        signature.SetLowerTimeSignature(oldLower);
    };
    signature.SetTempo(137.0);
    signature.SetUpperTimeSignature(7);
    signature.SetLowerTimeSignature(8);
    const auto component = loadTestVst3();
    const auto* effect = dynamic_cast<const VST3EffectBase*>(component.get());
    ASSERT_TRUE(effect);
    const RealtimeEffectState::EffectFactory::Scope factory([&](const PluginID&) -> const EffectInstanceFactory* {
        return effect;
    });
    auto& tracks = TrackList::Get(project);
    const auto track = makeTrack({ std::vector<float>(480000, 0.25f) });
    const auto clip = *track->Intervals().begin();
    clip->SetPlayStartTime(0.1);
    tracks.Add(track);
    const auto state = std::make_shared<RealtimeEffectState>(wxT("tracktion-project-clock"));
    ASSERT_TRUE(RealtimeEffectList::Get(*track).AddState(state));
    const auto directory = m_projectFile.getSiblingFile("tracktion-project-clock-" + juce::Uuid().toString());
    DEFER { directory.deleteRecursively();
    };
    TracktionRuntime runtime;
    runtime.init(directory.getChildFile("cache").getFullPathName().toStdString(),
                 directory.getChildFile("settings").getFullPathName().toStdString());
    const EffectInstanceFactory::EditorInstanceFactory::Scope editorFactory([&](const EffectInstanceFactory& plugin) {
        return createTracktionEffectInstance(runtime, plugin);
    });
    HostedPlayback playback(runtime.engine());
    TracktionSession session(runtime, tracks, TracktionSession::Role::Playback);
    TracktionSession rendered(runtime, tracks, TracktionSession::Role::Render);
    auto& transport = session.edit().getTransport();
    transport.play(false);
    DEFER { transport.stop(false, true, false);
    };
    playback.process(8);
    const auto editor = std::dynamic_pointer_cast<TracktionHostedEffectInstance>(state->GetInstance());
    ASSERT_TRUE(editor);
    auto* processor = editor->tracktionPlugin().getAudioPluginInstance();
    ASSERT_TRUE(processor);
    ASSERT_TRUE(processor->getPlayHead());
    const auto checkClock = [&](double tempo, int upper, int lower) {
        const auto position = processor->getPlayHead()->getPosition();
        ASSERT_TRUE(position);
        ASSERT_TRUE(position->getBpm());
        EXPECT_DOUBLE_EQ(*position->getBpm(), tempo);
        ASSERT_TRUE(position->getTimeSignature());
        EXPECT_EQ(position->getTimeSignature()->numerator, upper);
        EXPECT_EQ(position->getTimeSignature()->denominator, lower);
        const auto* nativeClip = tracktion::getAudioTracks(session.edit())[0]->getClips()[0];
        EXPECT_DOUBLE_EQ(nativeClip->getPosition().getStart().inSeconds(), clip->GetPlayStartTime());
        EXPECT_DOUBLE_EQ(nativeClip->getPosition().getEnd().inSeconds(), clip->GetPlayEndTime());
    };
    ASSERT_NO_FATAL_FAILURE(checkClock(137.0, 7, 8));
    signature.SetTempo(450.0);
    signature.SetUpperTimeSignature(5);
    signature.SetLowerTimeSignature(4);
    playback.process(8);
    ASSERT_NO_FATAL_FAILURE(checkClock(450.0, 5, 4));
    EXPECT_DOUBLE_EQ(rendered.edit().tempoSequence.getTempo(0)->getBpm(), 137.0);
    EXPECT_EQ(rendered.edit().tempoSequence.getTimeSig(0)->numerator.get(), 7);
    EXPECT_EQ(rendered.edit().tempoSequence.getTimeSig(0)->denominator.get(), 8);
}

TEST_F(AudioSourceTests, TracktionVst3PlaybackFollowsLiveSettingsAndBypass)
{
    const auto component = loadTestVst3();
    const auto* effect = dynamic_cast<const VST3EffectBase*>(component.get());
    ASSERT_TRUE(effect);
    const auto uiInstance = makeVst3Wrapper(*effect);
    ASSERT_TRUE(uiInstance);
    const auto gainID = vst3GainID(*uiInstance);
    const RealtimeEffectState::EffectFactory::Scope factory([&](const PluginID&) -> const EffectInstanceFactory* {
        return effect;
    });
    const auto tracks = TrackList::Create(nullptr);
    const auto track = makeTrack({ std::vector<float>(480000, 0.25f), std::vector<float>(480000, -0.125f) });
    tracks->Add(track);
    const auto state = std::make_shared<RealtimeEffectState>(wxT("tracktion-vst3-test"));
    auto& list = RealtimeEffectList::Get(*track);
    ASSERT_TRUE(list.AddState(state));
    const auto access = state->GetAccess();
    const auto directory = m_projectFile.getSiblingFile("tracktion-vst3-playback-" + juce::Uuid().toString());
    DEFER { directory.deleteRecursively();
    };
    TracktionRuntime runtime;
    runtime.init(directory.getChildFile("cache").getFullPathName().toStdString(),
                 directory.getChildFile("settings").getFullPathName().toStdString());
    const EffectInstanceFactory::EditorInstanceFactory::Scope editorFactory([&](const EffectInstanceFactory& plugin) {
        return createTracktionEffectInstance(runtime, plugin);
    });
    HostedPlayback playback(runtime.engine());
    TracktionSession session(runtime, *tracks, TracktionSession::Role::Playback);
    int externalCount = 0;
    for (auto* plugin : tracktion::getAudioTracks(session.edit())[0]->pluginList) {
        auto* external = dynamic_cast<tracktion::ExternalPlugin*>(plugin);
        if (external) {
            ASSERT_TRUE(external->getAudioPluginInstance());
            EXPECT_TRUE(external->isVST3());
            ++externalCount;
        }
    }
    ASSERT_EQ(externalCount, 1);
    auto& transport = session.edit().getTransport();
    transport.play(false);
    for (auto* plugin : tracktion::getAudioTracks(session.edit())[0]->pluginList) {
        auto* external = dynamic_cast<tracktion::ExternalPlugin*>(plugin);
        if (external) {
            EXPECT_DOUBLE_EQ(external->getAudioPluginInstance()->getLatencySamples(), 33);
        }
    }
    const auto checkGain = [&](float gain) {
        const auto rendered = playback.process(64);
        for (int channel = 0; channel < 2; ++channel) {
            const float input = channel == 0 ? 0.25f : -0.125f;
            for (int sample = 8192; sample < 16384; ++sample) {
                ASSERT_NEAR(rendered.getSample(channel, sample), input * gain, 1e-5f);
            }
        }
    };
    ASSERT_NO_FATAL_FAILURE(checkGain(0.5f));
    access->ModifySettings([&](EffectSettings& settings) {
        setVst3Gain(*effect, settings, gainID, 0.25f);
        return nullptr;
    });
    access->Flush();
    ASSERT_NO_FATAL_FAILURE(checkGain(0.25f));
    auto savedSettings = effect->MakeSettings();
    setVst3Gain(*effect, savedSettings, gainID, 0.75f);
    ASSERT_TRUE(uiInstance->Initialize(savedSettings, 48000.0, Steinberg::Vst::kOffline, 512));
    uiInstance->Finalize(&savedSettings);
    CommandParameters savedParameters;
    ASSERT_TRUE(effect->SaveSettings(savedSettings, savedParameters));
    access->ModifySettings([&](EffectSettings& settings) {
        EXPECT_TRUE(effect->LoadSettings(savedParameters, settings));
        return nullptr;
    });
    access->Flush();
    ASSERT_NO_FATAL_FAILURE(checkGain(0.75f));
    access->ModifySettings([&](EffectSettings& settings) {
        setVst3Gain(*effect, settings, gainID, 0.125f);
        return nullptr;
    });
    access->Flush();
    ASSERT_NO_FATAL_FAILURE(checkGain(0.125f));
    access->ModifySettings([&](EffectSettings& settings) {
        EXPECT_TRUE(effect->LoadSettings(CommandParameters {}, settings));
        return nullptr;
    });
    access->Flush();
    ASSERT_NO_FATAL_FAILURE(checkGain(0.5f));
    state->SetActive(false);
    ASSERT_NO_FATAL_FAILURE(checkGain(1.0f));
    state->SetActive(true);
    ASSERT_NO_FATAL_FAILURE(checkGain(0.5f));
    list.SetActive(false);
    ASSERT_NO_FATAL_FAILURE(checkGain(1.0f));
    transport.stop(false, true, false);
    EXPECT_TRUE(std::dynamic_pointer_cast<TracktionHostedEffectInstance>(state->PeekInstance()));
}
#endif

#if AU_TRACKTION_TEST_AUDIO_UNIT
TEST_F(AudioSourceTests, TracktionAudioUnitRenderUsesSavedParametersAndBypass)
{
    AudioComponentDescription description {};
    description.componentType = kAudioUnitType_Effect;
    description.componentSubType = kAudioUnitSubType_LowPassFilter;
    description.componentManufacturer = kAudioUnitManufacturer_Apple;
    const auto component = AudioComponentFindNext(nullptr, &description);
    ASSERT_TRUE(component);
    AudioUnitEffectBase effect("appl/aufx/lpas/Apple: AULowpass", "Apple: AULowpass", component);
    ASSERT_TRUE(effect.InitializePlugin());
    const RealtimeEffectState::EffectFactory::Scope factory([&](const PluginID&) -> const EffectInstanceFactory* {
        return &effect;
    });
    std::vector<float> sine(24000);
    for (size_t sample = 0; sample < sine.size(); ++sample) {
        sine[sample] = 0.2f * std::sin(2.0 * std::numbers::pi * 10000.0 * sample / 48000.0);
    }
    for (const int channels : { 1, 2 }) {
        SCOPED_TRACE(channels);
        const auto track = makeTrack(std::vector<std::vector<float> >(channels, sine));
        const auto tracks = TrackList::Create(nullptr);
        tracks->Add(track);
        auto& list = RealtimeEffectList::Get(*track);
        const auto state = std::make_shared<RealtimeEffectState>(wxT("tracktion-au-lowpass"));
        ASSERT_TRUE(list.AddState(state));
        auto access = state->GetAccess();
        access->ModifySettings([](EffectSettings& settings) {
            auto& values = AudioUnitWrapper::GetSettings(settings).values;
            values.at(kLowPassParam_CutoffFrequency)->second = 1000.0f;
            values.at(kLowPassParam_Resonance)->second = 0.0f;
            return nullptr;
        });
        access->Flush();
        juce::AudioBuffer<float> filtered;
        ASSERT_NO_FATAL_FAILURE(renderTracks(*tracks, filtered, 24000));
        for (int channel = 0; channel < 2; ++channel) {
            EXPECT_LT(filtered.getRMSLevel(channel, 12000, 12000), 0.002f);
            EXPECT_GT(filtered.getRMSLevel(channel, 12000, 12000), 0.00001f);
        }
        EXPECT_FALSE(state->PeekInstance());
        state->SetActive(false);
        juce::AudioBuffer<float> bypassed;
        ASSERT_NO_FATAL_FAILURE(renderTracks(*tracks, bypassed, 24000));
        for (int channel = 0; channel < 2; ++channel) {
            for (int sample = 0; sample < 24000; ++sample) {
                ASSERT_NEAR(bypassed.getSample(channel, sample), sine[sample], 1e-5f);
            }
        }
    }
}

TEST_F(AudioSourceTests, TracktionAudioUnitPlaybackFollowsLiveSettingsAndListBypass)
{
    AudioComponentDescription description {};
    description.componentType = kAudioUnitType_Effect;
    description.componentSubType = kAudioUnitSubType_LowPassFilter;
    description.componentManufacturer = kAudioUnitManufacturer_Apple;
    const auto component = AudioComponentFindNext(nullptr, &description);
    ASSERT_TRUE(component);
    AudioUnitEffectBase effect("appl/aufx/lpas/Apple: AULowpass", "Apple: AULowpass", component);
    ASSERT_TRUE(effect.InitializePlugin());
    const RealtimeEffectState::EffectFactory::Scope factory([&](const PluginID&) -> const EffectInstanceFactory* {
        return &effect;
    });
    std::vector<float> sine(480000);
    for (size_t sample = 0; sample < sine.size(); ++sample) {
        sine[sample] = 0.2f * std::sin(2.0 * std::numbers::pi * 10000.0 * sample / 48000.0);
    }
    const auto track = makeTrack({ sine, sine });
    const auto tracks = TrackList::Create(nullptr);
    tracks->Add(track);
    auto& list = RealtimeEffectList::Get(*track);
    const auto state = std::make_shared<RealtimeEffectState>(wxT("tracktion-au-lowpass"));
    ASSERT_TRUE(list.AddState(state));
    const auto access = state->GetAccess();
    const auto setCutoff = [&](float frequency) {
        access->ModifySettings([frequency](EffectSettings& settings) {
            auto& values = AudioUnitWrapper::GetSettings(settings).values;
            values.at(kLowPassParam_CutoffFrequency)->second = frequency;
            values.at(kLowPassParam_Resonance)->second = 0.0f;
            return nullptr;
        });
        access->Flush();
    };
    setCutoff(1000.0f);
    const auto directory = m_projectFile.getSiblingFile("tracktion-au-playback-" + juce::Uuid().toString());
    DEFER { directory.deleteRecursively();
    };
    TracktionRuntime runtime;
    runtime.init(directory.getChildFile("cache").getFullPathName().toStdString(),
                 directory.getChildFile("settings").getFullPathName().toStdString());
    HostedPlayback playback(runtime.engine());
    TracktionSession session(runtime, *tracks, TracktionSession::Role::Playback);
    const auto nativeTracks = tracktion::getAudioTracks(session.edit());
    ASSERT_EQ(nativeTracks.size(), 1);
    int externalCount = 0;
    for (auto plugin : nativeTracks[0]->pluginList) {
        auto* external = dynamic_cast<tracktion::ExternalPlugin*>(plugin);
        if (external) {
            ASSERT_TRUE(external->getAudioPluginInstance());
            EXPECT_TRUE(external->getAudioPluginInstance()->getAudioUnitClient());
            ++externalCount;
        }
    }
    EXPECT_EQ(externalCount, 1);
    auto& transport = session.edit().getTransport();
    transport.play(false);
    const auto filtered = playback.process(64);
    EXPECT_LT(filtered.getRMSLevel(0, 8192, 8192), 0.002f);
    setCutoff(20000.0f);
    const auto opened = playback.process(64);
    EXPECT_GT(opened.getRMSLevel(0, 8192, 8192), 0.1f);
    setCutoff(1000.0f);
    const auto closed = playback.process(64);
    EXPECT_LT(closed.getRMSLevel(0, 8192, 8192), 0.002f);
    list.SetActive(false);
    const auto bypassed = playback.process(64);
    EXPECT_NEAR(bypassed.getRMSLevel(0, 8192, 8192), 0.2f / std::sqrt(2.0f), 1e-4f);
    transport.stop(false, true, false);
    EXPECT_FALSE(state->PeekInstance());
}

TEST_F(AudioSourceTests, TracktionOfflineAudioUnitKeepsAudacityParametersAndLatency)
{
    AudioComponentDescription description {};
    description.componentType = kAudioUnitType_Effect;
    description.componentSubType = kAudioUnitSubType_PeakLimiter;
    description.componentManufacturer = kAudioUnitManufacturer_Apple;
    const auto component = AudioComponentFindNext(nullptr, &description);
    ASSERT_TRUE(component);
    AudioUnitEffectBase effect("appl/aufx/lmtr/Apple: AUPeakLimiter", "Apple: AUPeakLimiter", component);
    ASSERT_TRUE(effect.InitializePlugin());
    auto settings = effect.MakeSettings();
    AudioUnitWrapper::GetSettings(settings).values.at(kLimiterParam_PreGain)->second = -6.0f;
    std::vector<std::vector<float> > input(2, std::vector<float>(4800));
    input[0][101] = 0.1f;
    input[0][2400] = 0.2f;
    input[1][201] = 0.2f;
    input[1][3000] = 0.1f;
    juce::AudioBuffer<float> expected;
    ASSERT_NO_FATAL_FAILURE(applyOfflineEffect(effect, settings, input, expected));
    const auto directory = m_projectFile.getSiblingFile("tracktion-offline-au-" + juce::Uuid().toString());
    DEFER { directory.deleteRecursively();
    };
    TracktionRuntime runtime;
    runtime.init(directory.getChildFile("cache").getFullPathName().toStdString(),
                 directory.getChildFile("settings").getFullPathName().toStdString());
    const EffectPlugin::ProcessingInstanceFactory::Scope factory([&](const EffectPlugin& plugin) {
        return createTracktionEffectInstance(runtime, plugin);
    });
    juce::AudioBuffer<float> actual;
    ASSERT_NO_FATAL_FAILURE(applyOfflineEffect(effect, settings, input, actual));
    for (int channel = 0; channel < actual.getNumChannels(); ++channel) {
        for (int frame = 0; frame < actual.getNumSamples(); ++frame) {
            ASSERT_NEAR(actual.getSample(channel, frame), expected.getSample(channel, frame), 1e-6f) << channel << ":" << frame;
        }
    }
}

TEST_F(AudioSourceTests, TracktionAudioUnitRenderCompensatesTrackAndMasterLatency)
{
    AudioComponentDescription description {};
    description.componentType = kAudioUnitType_Effect;
    description.componentSubType = kAudioUnitSubType_PeakLimiter;
    description.componentManufacturer = kAudioUnitManufacturer_Apple;
    const auto component = AudioComponentFindNext(nullptr, &description);
    ASSERT_TRUE(component);
    AudioUnitEffectBase effect("appl/aufx/lmtr/Apple: AUPeakLimiter", "Apple: AUPeakLimiter", component);
    ASSERT_TRUE(effect.InitializePlugin());
    const RealtimeEffectState::EffectFactory::Scope factory([&](const PluginID&) -> const EffectInstanceFactory* {
        return &effect;
    });
    auto& project = *reinterpret_cast<au3::Au3Project*>(m_project->au3ProjectPtr());
    auto& tracks = TrackList::Get(project);
    std::vector<float> samples(4800);
    samples[0] = 0.1f;
    samples[2400] = 0.1f;
    const auto track = makeTrack({ samples, samples });
    tracks.Add(track);
    const auto trackState = std::make_shared<RealtimeEffectState>(wxT("tracktion-au-limiter"));
    const auto masterState = std::make_shared<RealtimeEffectState>(wxT("tracktion-au-limiter"));
    ASSERT_TRUE(RealtimeEffectList::Get(*track).AddState(trackState));
    ASSERT_TRUE(RealtimeEffectList::Get(project).AddState(masterState));
    const auto directory = m_projectFile.getSiblingFile("tracktion-au-latency-" + juce::Uuid().toString());
    DEFER { directory.deleteRecursively();
    };
    TracktionRuntime runtime;
    runtime.init(directory.getChildFile("cache").getFullPathName().toStdString(),
                 directory.getChildFile("settings").getFullPathName().toStdString());
    juce::AudioBuffer<float> rendered;
    const auto render = [&] {
        TracktionSession session(runtime, tracks, TracktionSession::Role::Render);
        TracktionRenderSource::Options options;
        options.endTime = 0.1;
        options.sampleRate = 48000.0;
        TracktionRenderSource source(session, options);
        int externalCount = 0;
        const auto checkLatency = [&](tracktion::PluginList& plugins) {
            for (auto* plugin : plugins) {
                auto* external = dynamic_cast<tracktion::ExternalPlugin*>(plugin);
                if (external) {
                    EXPECT_GT(external->getLatencySeconds(), 0.0);
                    ++externalCount;
                }
            }
        };
        checkLatency(tracktion::getAudioTracks(session.edit())[0]->pluginList);
        checkLatency(session.edit().getMasterPluginList());
        ASSERT_EQ(externalCount, 2);
        rendered.setSize(2, 4800);
        int offset = 0;
        while (const int frames = source.process(257)) {
            ASSERT_LE(offset + frames, 4800);
            for (int channel = 0; channel < 2; ++channel) {
                rendered.copyFrom(channel, offset, source.buffer(), channel, 0, frames);
            }
            offset += frames;
        }
        ASSERT_EQ(offset, 4800);
    };
    ASSERT_NO_FATAL_FAILURE(render());
    for (int channel = 0; channel < 2; ++channel) {
        for (int sample = 0; sample < 4800; ++sample) {
            ASSERT_NEAR(rendered.getSample(channel, sample), samples[sample], 1e-5f);
        }
    }
    trackState->SetActive(false);
    masterState->SetActive(false);
    ASSERT_NO_FATAL_FAILURE(render());
    for (int channel = 0; channel < 2; ++channel) {
        for (int sample = 0; sample < 4800; ++sample) {
            ASSERT_NEAR(rendered.getSample(channel, sample), samples[sample], 1e-5f);
        }
    }
}
#endif

TEST_F(AudioSourceTests, TracktionRenderPreservesVoiceFormantsWhenPitchAndSpeedChange)
{
    const auto samples = makeVoiceSamples(48000);
    for (const double stretch : { 1.0, 1.5 }) {
        SCOPED_TRACE(stretch);
        const auto track = makeTrack({ samples });
        const auto clip = *track->Intervals().begin();
        clip->StretchBy(stretch);
        ASSERT_TRUE(clip->SetCentShift(700));
        const auto tracks = TrackList::Create(nullptr);
        tracks->Add(track);
        for (const bool voice : { false, true }) {
            SCOPED_TRACE(voice);
            clip->SetPitchAndSpeedPreset(voice ? PitchAndSpeedPreset::OptimizeForVoice : PitchAndSpeedPreset::Default);
            juce::AudioBuffer<float> rendered;
            ASSERT_NO_FATAL_FAILURE(renderTracks(*tracks, rendered, static_cast<int>(samples.size() * stretch)));
            EXPECT_NEAR(peakFrequency(rendered, 80.0, 220.0), 100.0 * std::pow(2.0, 700.0 / 1200.0), 4.0);
            EXPECT_NEAR(peakFrequency(rendered, 600.0, 1800.0), voice ? 1000.0 : 1000.0 * std::pow(2.0, 700.0 / 1200.0), 120.0);
        }
    }
}

TEST_F(AudioSourceTests, TracktionRenderCombinesIndependentPitchAndSpeedChanges)
{
    std::vector<float> samples(48000);
    for (size_t sample = 0; sample < samples.size(); ++sample) {
        samples[sample] = 0.2f * std::sin(2.0 * std::numbers::pi * 440.0 * sample / 48000.0);
    }
    const auto track = makeTrack({ samples });
    const auto clip = *track->Intervals().begin();
    clip->StretchBy(2.0);
    ASSERT_TRUE(clip->SetCentShift(1200));
    const auto tracks = TrackList::Create(nullptr);
    tracks->Add(track);

    juce::AudioBuffer<float> rendered;
    ASSERT_NO_FATAL_FAILURE(renderTracks(*tracks, rendered, 105600));
    int crossings = 0;
    for (int sample = 24001; sample < 72000; ++sample) {
        crossings += rendered.getSample(0, sample - 1) <= 0.0f && rendered.getSample(0, sample) > 0.0f;
    }
    EXPECT_NEAR(crossings, 880, 3);
    EXPECT_GT(rendered.getRMSLevel(0, 24000, 48000), 0.1f);
    EXPECT_FLOAT_EQ(rendered.getRMSLevel(0, 100800, 4800), 0.0f);
}

TEST_F(AudioSourceTests, TracktionPlaybackReadsProjectAudioAndSeeks)
{
    const auto directory = m_projectFile.getSiblingFile("tracktion-playback-" + juce::Uuid().toString());
    DEFER { directory.deleteRecursively();
    };
    TracktionRuntime runtime;
    runtime.init(directory.getChildFile("cache").getFullPathName().toStdString(),
                 directory.getChildFile("settings").getFullPathName().toStdString());
    HostedPlayback playback(runtime.engine());

    std::vector<float> samples(48000, 0.125f);
    std::fill(samples.begin() + 24000, samples.end(), 0.25f);
    const auto tracks = TrackList::Create(nullptr);
    tracks->Add(makeTrack({ samples }));
    TracktionSession session(runtime, *tracks, TracktionSession::Role::Playback);
    auto& transport = session.edit().getTransport();
    transport.ensureContextAllocated();
    transport.play(false);
    ASSERT_TRUE(transport.isPlaying());

    const auto first = playback.process(24);
    EXPECT_NEAR(first.getSample(0, first.getNumSamples() - 1), 0.125f, 1e-5f);
    EXPECT_NEAR(first.getSample(1, first.getNumSamples() - 1), 0.125f, 1e-5f);
    EXPECT_GT(transport.getCurrentPlaybackContext()->getAudibleTimelineTime().inSeconds(), 0.1);

    transport.setPosition(tracktion::TimePosition::fromSeconds(0.7));
    const auto afterSeek = playback.process(12);
    EXPECT_NEAR(afterSeek.getSample(0, afterSeek.getNumSamples() - 1), 0.25f, 1e-5f);
    EXPECT_GT(transport.getCurrentPlaybackContext()->getAudibleTimelineTime().inSeconds(), 0.7);

    transport.stop(false, true, false);
    EXPECT_FALSE(transport.isPlaying());
    const auto stopped = playback.process(12);
    EXPECT_FLOAT_EQ(stopped.getRMSLevel(0, 256, stopped.getNumSamples() - 256), 0.0f);
}

TEST_F(AudioSourceTests, TracktionPlaybackPausesWithoutLosingThePosition)
{
    const auto directory = m_projectFile.getSiblingFile("tracktion-pause-" + juce::Uuid().toString());
    DEFER { directory.deleteRecursively();
    };
    TracktionRuntime runtime;
    runtime.init(directory.getChildFile("cache").getFullPathName().toStdString(),
                 directory.getChildFile("settings").getFullPathName().toStdString());
    HostedPlayback playback(runtime.engine());
    const auto tracks = TrackList::Create(nullptr);
    const auto track = makeTrack({ std::vector<float>(48000, 0.25f) });
    tracks->Add(track);
    TracktionSession session(runtime, *tracks, TracktionSession::Role::Playback);
    auto& transport = session.edit().getTransport();
    transport.play(false);
    transport.setPaused(true);
    const auto beforeFirstBlock = playback.process(4);
    EXPECT_FLOAT_EQ(beforeFirstBlock.getMagnitude(0, beforeFirstBlock.getNumSamples()), 0.0f);
    EXPECT_EQ(transport.getCurrentPlaybackContext()->getNodePlayHead()->getPosition(), 0);
    EXPECT_TRUE(transport.isPaused());
    transport.setPaused(false);
    playback.process(12);
    auto* context = transport.getCurrentPlaybackContext();
    transport.setPaused(true);
    const auto pausedPosition = context->getNodePlayHead()->getPosition();
    const auto pausedAudio = playback.process(24);
    EXPECT_TRUE(transport.isPlaying());
    EXPECT_TRUE(transport.isPaused());
    EXPECT_EQ(context, transport.getCurrentPlaybackContext());
    EXPECT_EQ(pausedPosition, context->getNodePlayHead()->getPosition());
    EXPECT_FLOAT_EQ(pausedAudio.getRMSLevel(0, 256, pausedAudio.getNumSamples() - 256), 0.0f);

    const auto effect = addBuiltinEffect(RealtimeEffectList::Get(*track), wxT("tracktion-bass-treble"));
    ASSERT_TRUE(effect);
    session.edit().dispatchPendingUpdatesSynchronously();
    auto processing = std::async(std::launch::async, [&playback] { return playback.process(32, false); });
    const auto access = effect->GetAccess();
    access->ModifySettings([](EffectSettings& settings) {
        effects::BassTrebleEffect::GetSettings(settings).mGain = 6.020599913;
        return nullptr;
    });
    access->Flush();
    const auto afterChange = processing.get();
    EXPECT_FLOAT_EQ(afterChange.getMagnitude(0, afterChange.getNumSamples()), 0.0f);
    EXPECT_EQ(pausedPosition, context->getNodePlayHead()->getPosition());

    transport.setPaused(false);
    const auto resumedAudio = playback.process(12);
    EXPECT_FALSE(transport.isPaused());
    EXPECT_NEAR(resumedAudio.getSample(0, resumedAudio.getNumSamples() - 1), 0.5f, 1e-5f);
    EXPECT_EQ(context->getNodePlayHead()->getPosition(), pausedPosition + resumedAudio.getNumSamples());
    transport.stop(false, true, false);
}

TEST_F(AudioSourceTests, AudioDeviceStateDistinguishesPausedPlaybackFromAClosedDevice)
{
    const auto directory = m_projectFile.getSiblingFile("tracktion-device-state-" + juce::Uuid().toString());
    DEFER { directory.deleteRecursively();
    };
    TracktionRuntime runtime;
    runtime.init(directory.getChildFile("cache").getFullPathName().toStdString(),
                 directory.getChildFile("settings").getFullPathName().toStdString());
    EXPECT_FALSE(runtime.isAudioDeviceRunning());
    HostedPlayback playback(runtime.engine());
    ASSERT_TRUE(runtime.isAudioDeviceRunning());

    const auto tracks = TrackList::Create(nullptr);
    tracks->Add(makeTrack({ std::vector<float>(48000, 0.25f) }));
    TracktionSession session(runtime, *tracks, TracktionSession::Role::Playback);
    auto& transport = session.edit().getTransport();
    ASSERT_TRUE(session.startPlayback());
    playback.process(4);
    transport.setPaused(true);
    playback.process(4);
    ASSERT_TRUE(transport.isPlaying());
    ASSERT_TRUE(transport.isPaused());
    EXPECT_TRUE(runtime.isAudioDeviceRunning());

    transport.setPaused(false);
    runtime.closeAudioDevice();
    EXPECT_TRUE(transport.isPlaying());
    EXPECT_FALSE(runtime.isAudioDeviceRunning());
    transport.stop(false, true, false);
}

TEST(AudioDeviceTests, PhysicalOutputUsesPortAudioAndAdvancesTheDeviceClock)
{
    const auto host = qEnvironmentVariable("AU_TRACKTION_TEST_AUDIO_HOST").toStdString();
    if (host.empty()) {
        GTEST_SKIP() << "Set AU_TRACKTION_TEST_AUDIO_HOST to test a physical audio device.";
    }
    ASSERT_EQ(Pa_Initialize(), paNoError);
    DEFER { Pa_Terminate();
    };
    const auto directory = juce::File::getSpecialLocation(juce::File::tempDirectory)
                           .getChildFile("tracktion-physical-output-" + juce::Uuid().toString());
    DEFER { directory.deleteRecursively();
    };
    TracktionRuntime runtime;
    runtime.init(directory.getChildFile("cache").getFullPathName().toStdString(),
                 directory.getChildFile("settings").getFullPathName().toStdString());
    const auto callbacks = std::make_shared<TracktionAudioCallbacks>();
    const auto output = qEnvironmentVariable("AU_TRACKTION_TEST_OUTPUT_DEVICE").toStdString();
    const auto error = runtime.openAudioDevice(host, {}, output, 0, 2, 44100.0, callbacks);
    ASSERT_TRUE(error.empty()) << error;
    ASSERT_TRUE(runtime.isAudioDeviceRunning());
    auto* device = runtime.engine().getDeviceManager().deviceManager.getCurrentAudioDevice();
    ASSERT_NE(device, nullptr);
    EXPECT_EQ(device->getActiveInputChannels().countNumberOfSetBits(), 0);
    EXPECT_EQ(device->getActiveOutputChannels().countNumberOfSetBits(), 2);

    auto& manager = runtime.engine().getDeviceManager();
    ASSERT_FALSE(manager.getWaveOutputDevices().empty());
    const double start = manager.getCurrentStreamTime();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (manager.getCurrentStreamTime() - start < 0.1
           && std::chrono::steady_clock::now() < deadline) {
        QCoreApplication::processEvents();
        juce::Timer::callPendingTimersSynchronously();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    EXPECT_GT(manager.getCurrentStreamTime() - start, 0.1);
    EXPECT_TRUE(callbacks->consumeCallbackInfo());
    const double deviceClock = manager.getCurrentStreamTime();
    ASSERT_TRUE(runtime.openAudioDevice(host, {}, output, 0, 2, 44100.0, callbacks).empty());
    EXPECT_EQ(manager.deviceManager.getCurrentAudioDevice(), device);
    EXPECT_GE(manager.getCurrentStreamTime(), deviceClock);
    runtime.closeAudioDevice();
    EXPECT_FALSE(runtime.isAudioDeviceRunning());

    const auto input = qEnvironmentVariable("AU_TRACKTION_TEST_INPUT_DEVICE").toStdString();
    if (!input.empty()) {
        const auto inputError = runtime.openAudioDevice(host, input, output, 1, 2, 44100.0, callbacks);
        ASSERT_TRUE(inputError.empty()) << inputError;
        ASSERT_TRUE(runtime.isAudioDeviceRunning());
        const auto inputs = manager.getWaveInputDevices();
        ASSERT_FALSE(inputs.empty());
        for (const auto* waveInput : inputs) {
            EXPECT_LE(waveInput->deviceDescription.getDeviceChannelRange().second, 1u);
        }
        int inputChannels = 0;
        {
            auto* type = manager.deviceManager.getCurrentDeviceTypeObject();
            const std::unique_ptr<juce::AudioIODevice> probe(type->createDevice(juce::String(output), juce::String(input)));
            ASSERT_NE(probe, nullptr);
            inputChannels = probe->getInputChannelNames().size();
        }
        if (inputChannels >= 2) {
            ASSERT_TRUE(runtime.openAudioDevice(host, input, output, 2, 2, 44100.0, callbacks).empty());
            EXPECT_EQ(manager.deviceManager.getCurrentAudioDevice()->getActiveInputChannels().countNumberOfSetBits(), 2);
            ASSERT_TRUE(runtime.openAudioDevice(host, input, output, 1, 2, 44100.0, callbacks).empty());
            EXPECT_EQ(manager.deviceManager.getCurrentAudioDevice()->getActiveInputChannels().countNumberOfSetBits(), 1);
        }
        runtime.closeAudioDevice();
    }

    const auto reopenError = runtime.openAudioDevice(host, {}, output, 0, 2, 44100.0, callbacks);
    ASSERT_TRUE(reopenError.empty()) << reopenError;
    EXPECT_TRUE(runtime.isAudioDeviceRunning());
    EXPECT_TRUE(manager.getWaveInputDevices().empty());
    runtime.closeAudioDevice();
}

TEST_F(AudioSourceTests, TracktionPlaybackUsesOnlyTheRequestedSequences)
{
    const auto directory = m_projectFile.getSiblingFile("tracktion-sequences-" + juce::Uuid().toString());
    DEFER { directory.deleteRecursively();
    };
    TracktionRuntime runtime;
    runtime.init(directory.getChildFile("cache").getFullPathName().toStdString(),
                 directory.getChildFile("settings").getFullPathName().toStdString());
    HostedPlayback playback(runtime.engine());
    const auto tracks = TrackList::Create(nullptr);
    auto requested = makeTrack({ std::vector<float>(48000, 0.125f) });
    tracks->Add(requested);
    auto excluded = makeTrack({ std::vector<float>(48000, 0.5f) });
    excluded->SetSolo(true);
    tracks->Add(excluded);
    TransportSequences sequences;
    sequences.playbackSequences.push_back(requested);
    TracktionSession session(runtime, *tracks, sequences, TracktionSession::Role::Playback);
    auto& transport = session.edit().getTransport();
    transport.play(false);
    const auto audio = playback.process(12);
    EXPECT_NEAR(audio.getSample(0, audio.getNumSamples() - 1), 0.125f, 1e-5f);
    transport.stop(false, true, false);
}

TEST_F(AudioSourceTests, TracktionPlaybackSilencesOutputAtTheRequestedSample)
{
    const auto directory = m_projectFile.getSiblingFile("tracktion-playback-end-" + juce::Uuid().toString());
    DEFER { directory.deleteRecursively();
    };
    TracktionRuntime runtime;
    runtime.init(directory.getChildFile("cache").getFullPathName().toStdString(),
                 directory.getChildFile("settings").getFullPathName().toStdString());
    HostedPlayback playback(runtime.engine());
    const auto tracks = TrackList::Create(nullptr);
    tracks->Add(makeTrack({ std::vector<float>(48000, 0.25f) }));
    TracktionSession session(runtime, *tracks, TracktionSession::Role::Playback);
    session.setPlaybackEnd(1003.0 / 48000.0);
    session.setPlaybackVolume(0.5f);
    auto& transport = session.edit().getTransport();
    transport.play(false);
    const auto audio = playback.process(12);
    EXPECT_NEAR(audio.getSample(0, 1002), 0.125f, 1e-5f);
    EXPECT_FLOAT_EQ(audio.getMagnitude(1003, audio.getNumSamples() - 1003), 0.0f);
    ASSERT_TRUE(session.playbackEndSampleOffset().has_value());
    EXPECT_EQ(*session.playbackEndSampleOffset(), 235);
    transport.stop(false, true, false);
}

TEST_F(AudioSourceTests, AudioEngineChangesTheAudacityEffectListsWithoutOpeningADevice)
{
    static effects::BassTrebleEffect effect;
    const RealtimeEffectState::EffectFactory::Scope factory([](const PluginID& id) -> const EffectInstanceFactory* {
        return id == wxT("tracktion-bass-treble") ? &effect : nullptr;
    });
    auto& project = *reinterpret_cast<au3::Au3Project*>(m_project->au3ProjectPtr());
    const auto track = makeTrack({ std::vector<float>(48000, 0.25f) });
    auto& list = RealtimeEffectList::Get(*track);
    auto& master = RealtimeEffectList::Get(project);
    TracktionAudioEngine engine;
    const auto state = engine.addRealtimeEffectState(project, track.get(), "tracktion-bass-treble");
    ASSERT_TRUE(state);
    ASSERT_EQ(list.GetStatesCount(), 1u);
    EXPECT_EQ(list.GetStateAt(0), state);
    const auto replacement = engine.replaceRealtimeEffectState(project, track.get(), 0, "tracktion-bass-treble");
    ASSERT_TRUE(replacement);
    EXPECT_NE(replacement, state);
    EXPECT_EQ(list.GetStateAt(0), replacement);
    const auto masterState = engine.addRealtimeEffectState(project, nullptr, "tracktion-bass-treble");
    ASSERT_TRUE(masterState);
    ASSERT_EQ(master.GetStatesCount(), 1u);
    EXPECT_EQ(master.GetStateAt(0), masterState);
    engine.removeRealtimeEffectState(project, track.get(), replacement);
    engine.removeRealtimeEffectState(project, nullptr, masterState);
    EXPECT_EQ(list.GetStatesCount(), 0u);
    EXPECT_EQ(master.GetStatesCount(), 0u);
    EXPECT_FALSE(engine.currentStream());
}

TEST_F(AudioSourceTests, TracktionRenderStreamsTheRequestedRangeOnAnExportThread)
{
    const auto directory = m_projectFile.getSiblingFile("tracktion-render-stream-" + juce::Uuid().toString());
    DEFER { directory.deleteRecursively();
    };
    TracktionRuntime runtime;
    runtime.init(directory.getChildFile("cache").getFullPathName().toStdString(),
                 directory.getChildFile("settings").getFullPathName().toStdString());
    const auto tracks = TrackList::Create(nullptr);
    const auto track = makeTrack({ std::vector<float>(48000, 0.25f), std::vector<float>(48000, -0.125f) });
    tracks->Add(track);
    TracktionSession session(runtime, *tracks, TracktionSession::Role::Render);
    TracktionRenderSource::Options options;
    options.startTime = 1003.0 / 48000.0;
    options.endTime = 5803.0 / 48000.0;
    options.sampleRate = 44100.0;
    TracktionRenderSource source(session, options);
    std::atomic<bool> cancelled = false;
    source.setCancellationCheck([&cancelled] { return cancelled.load(); });
    auto result = std::async(std::launch::async, [&source] {
        std::vector<std::vector<float> > samples(2);
        while (const int frames = source.process(137)) {
            for (int channel = 0; channel < 2; ++channel) {
                const auto* data = source.buffer().getReadPointer(channel);
                samples[channel].insert(samples[channel].end(), data, data + frames);
            }
        }
        return samples;
    });
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (result.wait_for(std::chrono::milliseconds(1)) != std::future_status::ready) {
        QCoreApplication::processEvents();
#if JUCE_MAC
        CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.0, true);
#endif
        if (std::chrono::steady_clock::now() >= deadline) {
            cancelled.store(true);
        }
    }
    EXPECT_FALSE(cancelled.load());
    const auto samples = result.get();
    for (int channel = 0; channel < 2; ++channel) {
        ASSERT_EQ(samples[channel].size(), 4410u);
        const float expected = channel == 0 ? 0.25f : -0.125f;
        for (const auto sample : samples[channel]) {
            EXPECT_NEAR(sample, expected, 1e-5f);
        }
    }
    EXPECT_NEAR(source.currentTime(), options.endTime, 1.0 / options.sampleRate);
}

TEST_F(AudioSourceTests, TracktionRenderCanCancelBeforeTheNextOutputBlock)
{
    const auto directory = m_projectFile.getSiblingFile("tracktion-render-cancel-" + juce::Uuid().toString());
    DEFER { directory.deleteRecursively();
    };
    TracktionRuntime runtime;
    runtime.init(directory.getChildFile("cache").getFullPathName().toStdString(),
                 directory.getChildFile("settings").getFullPathName().toStdString());
    const auto tracks = TrackList::Create(nullptr);
    tracks->Add(makeTrack({ std::vector<float>(48000, 0.25f) }));
    TracktionSession session(runtime, *tracks, TracktionSession::Role::Render);
    TracktionRenderSource::Options options;
    options.endTime = 1.0;
    TracktionRenderSource source(session, options);
    EXPECT_EQ(source.process(64), 64);
    source.setCancellationCheck([] { return true; });
    EXPECT_THROW(source.process(512), std::runtime_error);
}

TEST_F(AudioSourceTests, TracktionRenderReturnsNoSamplesForAnEmptyRange)
{
    const auto directory = m_projectFile.getSiblingFile("tracktion-render-empty-" + juce::Uuid().toString());
    DEFER { directory.deleteRecursively();
    };
    TracktionRuntime runtime;
    runtime.init(directory.getChildFile("cache").getFullPathName().toStdString(),
                 directory.getChildFile("settings").getFullPathName().toStdString());
    const auto tracks = TrackList::Create(nullptr);
    TracktionSession session(runtime, *tracks, TracktionSession::Role::Render);
    TracktionRenderSource source(session, {});
    EXPECT_EQ(source.process(137), 0);
    EXPECT_DOUBLE_EQ(source.currentTime(), 0.0);
}

TEST_F(AudioSourceTests, TracktionRenderStreamsAudioWithCompensatedEffectLatency)
{
    const auto directory = m_projectFile.getSiblingFile("tracktion-render-stream-latency-" + juce::Uuid().toString());
    DEFER { directory.deleteRecursively();
    };
    TracktionRuntime runtime;
    runtime.init(directory.getChildFile("cache").getFullPathName().toStdString(),
                 directory.getChildFile("settings").getFullPathName().toStdString());
    const auto tracks = TrackList::Create(nullptr);
    const auto track = makeTrack({ std::vector<float>(48000, 0.25f) });
    track->SetVolume(0.5f);
    tracks->Add(track);
    const auto effect = addBuiltinEffect(RealtimeEffectList::Get(*track), wxT("tracktion-compressor"));
    ASSERT_TRUE(effect);
    const auto access = effect->GetAccess();
    access->ModifySettings([](EffectSettings& settings) {
        auto& compressor = effects::CompressorEffect::GetSettings(settings);
        compressor.compressionRatio = 1.0;
        compressor.makeupGainDb = 0.0;
        compressor.lookaheadMs = 10.0;
        return nullptr;
    });
    access->Flush();
    TracktionSession session(runtime, *tracks, TracktionSession::Role::Render);
    TracktionRenderSource::Options options;
    options.endTime = 0.1;
    options.sampleRate = 48000.0;
    TracktionRenderSource source(session, options);
    std::vector<std::vector<float> > samples(2);
    while (const int frames = source.process(137)) {
        for (int channel = 0; channel < 2; ++channel) {
            const auto* data = source.buffer().getReadPointer(channel);
            samples[channel].insert(samples[channel].end(), data, data + frames);
        }
    }
    for (int channel = 0; channel < 2; ++channel) {
        ASSERT_EQ(samples[channel].size(), 4800u);
        const auto firstAudio = std::find_if(samples[channel].begin(), samples[channel].end(), [](float sample) {
            return sample != 0.0f;
        });
        for (size_t frame = 0; frame < samples[channel].size(); ++frame) {
            ASSERT_NEAR(samples[channel][frame], 0.125f, 1e-5f) << frame << ", " << channel
                                                                << ", first audio: " << (firstAudio - samples[channel].begin());
        }
    }
}

TEST_F(AudioSourceTests, TracktionRenderStreamsSilenceWhenNoTracksAreAudible)
{
    const auto directory = m_projectFile.getSiblingFile("tracktion-render-stream-silent-" + juce::Uuid().toString());
    DEFER { directory.deleteRecursively();
    };
    TracktionRuntime runtime;
    runtime.init(directory.getChildFile("cache").getFullPathName().toStdString(),
                 directory.getChildFile("settings").getFullPathName().toStdString());
    const auto tracks = TrackList::Create(nullptr);
    TracktionSession session(runtime, *tracks, TracktionSession::Role::Render);
    TracktionRenderSource::Options options;
    options.endTime = 0.1;
    options.sampleRate = 48000.0;
    TracktionRenderSource source(session, options);
    int framesRead = 0;
    while (const int frames = source.process(137)) {
        framesRead += frames;
        EXPECT_FLOAT_EQ(source.buffer().getMagnitude(0, frames), 0.0f);
    }
    EXPECT_EQ(framesRead, 4800);
}

TEST_F(AudioSourceTests, TracktionPlaybackResamplesAcrossTheAudacitySpeedRange)
{
    ASSERT_NO_FATAL_FAILURE(checkPlaybackSpeeds(false));
}

TEST_F(AudioSourceTests, TracktionPlaybackSpeedChangesPitchOnStretchedClips)
{
    ASSERT_NO_FATAL_FAILURE(checkPlaybackSpeeds(true));
}

TEST_F(AudioSourceTests, TracktionPlaybackAppliesSpeedChangesDuringPlayback)
{
    const auto directory = m_projectFile.getSiblingFile("tracktion-live-speed-" + juce::Uuid().toString());
    DEFER { directory.deleteRecursively();
    };
    TracktionRuntime runtime;
    runtime.init(directory.getChildFile("cache").getFullPathName().toStdString(),
                 directory.getChildFile("settings").getFullPathName().toStdString());
    HostedPlayback playback(runtime.engine());
    const auto tracks = TrackList::Create(nullptr);
    tracks->Add(makeTrack({ std::vector<float>(48000, 0.25f) }));
    TracktionSession session(runtime, *tracks, TracktionSession::Role::Playback);
    auto& transport = session.edit().getTransport();
    transport.play(false);
    playback.process(12);
    const auto first = transport.getCurrentPlaybackContext()->getSyncPoint()->time;
    session.setPlaybackSpeed(2.0);
    playback.process(12);
    const auto afterChange = transport.getCurrentPlaybackContext()->getSyncPoint()->time;
    EXPECT_NEAR((afterChange - first).inSeconds(), 6144.0 / 48000.0, 1e-10);
    session.setPlaybackEnd(afterChange.inSeconds() + 2006.0 / 48000.0);
    const auto audio = playback.process(12);
    EXPECT_NEAR(audio.getSample(0, 1002), 0.25f, 1e-5f);
    EXPECT_FLOAT_EQ(audio.getMagnitude(1003, audio.getNumSamples() - 1003), 0.0f);
    ASSERT_TRUE(session.playbackEndSampleOffset());
    EXPECT_EQ(*session.playbackEndSampleOffset(), 235);
    transport.stop(false, true, false);
}

TEST_F(AudioSourceTests, TracktionPlaybackSpeedKeepsEffectLatencyAndThePlaybackEndAligned)
{
    const auto directory = m_projectFile.getSiblingFile("tracktion-speed-latency-" + juce::Uuid().toString());
    DEFER { directory.deleteRecursively();
    };
    TracktionRuntime runtime;
    runtime.init(directory.getChildFile("cache").getFullPathName().toStdString(),
                 directory.getChildFile("settings").getFullPathName().toStdString());
    HostedPlayback playback(runtime.engine());
    const auto tracks = TrackList::Create(nullptr);
    const auto track = makeTrack({ std::vector<float>(48000, 0.25f) });
    tracks->Add(track);
    const auto effect = addBuiltinEffect(RealtimeEffectList::Get(*track), wxT("tracktion-compressor"));
    ASSERT_TRUE(effect);
    const auto access = effect->GetAccess();
    access->ModifySettings([](EffectSettings& settings) {
        auto& compressor = effects::CompressorEffect::GetSettings(settings);
        compressor.compressionRatio = 1.0;
        compressor.makeupGainDb = 0.0;
        compressor.lookaheadMs = 10.0;
        return nullptr;
    });
    access->Flush();
    TracktionSession session(runtime, *tracks, TracktionSession::Role::Playback);
    session.setPlaybackSpeed(2.0);
    session.setPlaybackEnd(2006.0 / 48000.0);
    auto& transport = session.edit().getTransport();
    transport.play(false);
    const auto audio = playback.process(12);
    EXPECT_GT(audio.getSample(0, 1481), 0.01f);
    EXPECT_FLOAT_EQ(audio.getMagnitude(1482, audio.getNumSamples() - 1482), 0.0f);
    ASSERT_TRUE(session.playbackEndSampleOffset());
    EXPECT_EQ(*session.playbackEndSampleOffset(), 202);
    auto* context = transport.getCurrentPlaybackContext();
    EXPECT_NEAR(context->getAudibleTimelineTime().inSeconds(), (11.0 * 256.0 * 2.0 - 479.0 * 2.0) / 48000.0, 1.0 / 48000.0);
    transport.stop(false, true, false);
}

TEST_F(AudioSourceTests, TracktionPlaybackKeepsDelayedSamplesAlignedDuringASpeedChange)
{
    const auto directory = m_projectFile.getSiblingFile("tracktion-live-speed-latency-" + juce::Uuid().toString());
    DEFER { directory.deleteRecursively();
    };
    TracktionRuntime runtime;
    runtime.init(directory.getChildFile("cache").getFullPathName().toStdString(),
                 directory.getChildFile("settings").getFullPathName().toStdString());
    HostedPlayback playback(runtime.engine());
    const auto tracks = TrackList::Create(nullptr);
    const auto track = makeTrack({ std::vector<float>(48000, 0.25f) });
    tracks->Add(track);
    const auto effect = addBuiltinEffect(RealtimeEffectList::Get(*track), wxT("tracktion-compressor"));
    ASSERT_TRUE(effect);
    const auto access = effect->GetAccess();
    access->ModifySettings([](EffectSettings& settings) {
        auto& compressor = effects::CompressorEffect::GetSettings(settings);
        compressor.compressionRatio = 1.0;
        compressor.makeupGainDb = 0.0;
        compressor.lookaheadMs = 10.0;
        return nullptr;
    });
    access->Flush();
    TracktionSession session(runtime, *tracks, TracktionSession::Role::Playback);
    auto& transport = session.edit().getTransport();
    transport.play(false);
    playback.process(12);
    session.setPlaybackSpeed(2.0);
    session.setPlaybackEnd(3272.0 / 48000.0);
    const auto first = playback.process(2);
    EXPECT_NEAR(first.getSample(0, 511), 0.25f, 1e-5f);
    EXPECT_NEAR(transport.getCurrentPlaybackContext()->getAudibleTimelineTime().inSeconds(), 2849.0 / 48000.0, 1.0 / 48000.0);
    const auto last = playback.process(1);
    EXPECT_NEAR(last.getSample(0, 66), 0.25f, 1e-5f);
    EXPECT_FLOAT_EQ(last.getMagnitude(67, last.getNumSamples() - 67), 0.0f);
    ASSERT_TRUE(session.playbackEndSampleOffset());
    EXPECT_EQ(*session.playbackEndSampleOffset(), 67);
    transport.stop(false, true, false);
}

TEST(PlaybackRateTests, KeepsTheReferenceClockAndPriorRates)
{
    tracktion::graph::PlayHead playHead;
    playHead.setReferenceSampleRange({ 0, 256 });
    playHead.play();
    playHead.setPlaybackRate(2.0);
    EXPECT_EQ(playHead.referenceSamplePositionToTimelinePositionUnlooped(128), 128);
    EXPECT_EQ(playHead.referenceSamplePositionToTimelinePositionUnlooped(512), 768);
    playHead.setReferenceSampleRange({ 256, 512 });
    playHead.setPlaybackRate(0.5);
    EXPECT_EQ(playHead.getReferenceSampleRange(), (juce::Range<int64_t> { 256, 512 }));
    EXPECT_EQ(playHead.referenceSamplePositionToTimelinePositionUnlooped(128), 128);
    EXPECT_EQ(playHead.referenceSamplePositionToTimelinePositionUnlooped(384), 512);
    EXPECT_EQ(playHead.referenceSamplePositionToTimelinePositionUnlooped(768), 896);

    juce::AudioBuffer<float> buffer(2, 256);
    tracktion::PluginRenderContext context { &buffer, 0, 256, nullptr, 0.0, {}, true, false, false, false };
    context.playHead = &playHead;
    context.referenceSampleRange = { 512, 768 };
    context.referenceSampleRate = 48000.0;
    context.latencySamples = 479;
    EXPECT_NEAR(context.getEditTimeAtSample(0).inSeconds(), 33.0 / 48000.0, 1e-10);
    EXPECT_NEAR(context.getEditTimeAtSample(255).inSeconds(), 320.0 / 48000.0, 1e-10);
}

TEST_F(AudioSourceTests, LoopPlaybackStartsAtTheRequestedPositionBeforeAndInsideTheLoop)
{
    const auto directory = m_projectFile.getSiblingFile("tracktion-loop-start-" + juce::Uuid().toString());
    DEFER { directory.deleteRecursively();
    };
    TracktionRuntime runtime;
    runtime.init(directory.getChildFile("cache").getFullPathName().toStdString(),
                 directory.getChildFile("settings").getFullPathName().toStdString());
    HostedPlayback playback(runtime.engine());
    std::vector<float> samples(48000);
    for (size_t frame = 0; frame < samples.size(); ++frame) {
        samples[frame] = 0.1f + 0.5f * frame / 48000.0f;
    }
    const auto tracks = TrackList::Create(nullptr);
    tracks->Add(makeTrack({ samples }));
    for (const double position : { 0.05, 0.15, 0.275 }) {
        for (const double speed : { 0.5, 1.0, 2.0 }) {
            SCOPED_TRACE(position);
            SCOPED_TRACE(speed);
            TracktionSession session(runtime, *tracks, TracktionSession::Role::Playback);
            auto& transport = session.edit().getTransport();
            transport.setPosition(tracktion::TimePosition::fromSeconds(position));
            ASSERT_TRUE(session.updatePlaybackLoop(true, 0.1, 0.3, TracktionSession::PlaybackLoopUpdate::Start));
            session.setPlaybackSpeed(speed);
            ASSERT_TRUE(session.startPlayback());
            EXPECT_NEAR(transport.getPosition().inSeconds(), position, 1.0 / 48000.0);
            const auto audio = playback.process(2);
            EXPECT_NEAR(audio.getSample(0, 200), 0.1 + 0.5 * (position + 200.0 * speed / 48000.0), 1e-4);
            transport.stop(false, true, false);
        }
    }
}

TEST_F(AudioSourceTests, LoopPlaybackSupportsRangesShorterThanTenMilliseconds)
{
    const auto directory = m_projectFile.getSiblingFile("tracktion-short-loop-" + juce::Uuid().toString());
    DEFER { directory.deleteRecursively();
    };
    TracktionRuntime runtime;
    runtime.init(directory.getChildFile("cache").getFullPathName().toStdString(),
                 directory.getChildFile("settings").getFullPathName().toStdString());
    HostedPlayback playback(runtime.engine());
    const auto tracks = TrackList::Create(nullptr);
    for (const int frames : { 240, 24, 1 }) {
        SCOPED_TRACE(frames);
        std::vector<float> samples(48000, 0.75f);
        std::fill_n(samples.begin() + 4800, frames, 0.125f);
        const auto track = makeTrack({ samples });
        tracks->Add(track);
        {
            TracktionSession session(runtime, *tracks, TracktionSession::Role::Playback);
            auto& transport = session.edit().getTransport();
            transport.setPosition(tracktion::TimePosition::fromSamples(4800, 48000.0));
            ASSERT_TRUE(session.updatePlaybackLoop(true, 0.1, (4800.0 + frames) / 48000.0, TracktionSession::PlaybackLoopUpdate::Start));
            const bool started = session.startPlayback();
            EXPECT_TRUE(started);
            if (started) {
                const auto audio = playback.process(100);
                for (int frame = audio.getNumSamples() - 256; frame < audio.getNumSamples(); ++frame) {
                    ASSERT_NEAR(audio.getSample(0, frame), 0.125f, 1e-4f) << frame;
                }
                auto* context = transport.getCurrentPlaybackContext();
                ASSERT_TRUE(context);
                ASSERT_TRUE(context->getNodePlayHead());
                EXPECT_TRUE(context->getNodePlayHead()->isLooping());
                EXPECT_EQ(context->getNodePlayHead()->getLoopRange().getLength(), frames);
                EXPECT_FALSE(session.playbackEndSampleOffset());
            }
            transport.stop(false, true, false);
        }
        tracks->Remove(*track);
    }
}

TEST_F(AudioSourceTests, LoopPlaybackSilencesRangesShorterThanOneSample)
{
    const auto directory = m_projectFile.getSiblingFile("tracktion-empty-sample-loop-" + juce::Uuid().toString());
    DEFER { directory.deleteRecursively();
    };
    TracktionRuntime runtime;
    runtime.init(directory.getChildFile("cache").getFullPathName().toStdString(),
                 directory.getChildFile("settings").getFullPathName().toStdString());
    HostedPlayback playback(runtime.engine());
    const auto tracks = TrackList::Create(nullptr);
    tracks->Add(makeTrack({ std::vector<float>(48000, 0.75f) }));
    TracktionSession session(runtime, *tracks, TracktionSession::Role::Playback);
    auto& transport = session.edit().getTransport();
    transport.setPosition(tracktion::TimePosition::fromSeconds(0.05));
    ASSERT_TRUE(session.updatePlaybackLoop(true, 0.1, 0.100001, TracktionSession::PlaybackLoopUpdate::Start));
    ASSERT_TRUE(session.startPlayback());
    const auto before = playback.process(2);
    EXPECT_NEAR(before.getSample(0, 200), 0.75f, 1e-5f);
    const auto looped = playback.process(100);
    EXPECT_FLOAT_EQ(looped.getMagnitude(looped.getNumSamples() - 256, 256), 0.0f);
    EXPECT_TRUE(transport.isPlaying());
    EXPECT_FALSE(session.playbackEndSampleOffset());
    auto* context = transport.getCurrentPlaybackContext();
    ASSERT_TRUE(context);
    ASSERT_TRUE(context->getNodePlayHead());
    EXPECT_TRUE(context->getNodePlayHead()->isLooping());
    EXPECT_EQ(context->getNodePlayHead()->getLoopRange().getLength(), 1);
    EXPECT_TRUE(session.updatePlaybackLoop(true, 0.1, 0.100005, TracktionSession::PlaybackLoopUpdate::RegionChanged));
    const auto changed = playback.process(4);
    EXPECT_FLOAT_EQ(changed.getMagnitude(0, changed.getNumSamples()), 0.0f);
    EXPECT_FALSE(session.playbackEndSampleOffset());
    EXPECT_FALSE(session.updatePlaybackLoop(false, 0.1, 0.100005, TracktionSession::PlaybackLoopUpdate::RegionChanged));
    session.setPlaybackEnd(1.0);
    const auto continued = playback.process(4);
    EXPECT_NEAR(continued.getSample(0, continued.getNumSamples() - 1), 0.75f, 1e-5f);
    EXPECT_TRUE(transport.isPlaying());
    transport.stop(false, true, false);
}

TEST_F(AudioSourceTests, TracktionPlaybackLoopsAtVariableSpeeds)
{
    const auto directory = m_projectFile.getSiblingFile("tracktion-loop-speed-" + juce::Uuid().toString());
    DEFER { directory.deleteRecursively();
    };
    TracktionRuntime runtime;
    runtime.init(directory.getChildFile("cache").getFullPathName().toStdString(),
                 directory.getChildFile("settings").getFullPathName().toStdString());
    HostedPlayback playback(runtime.engine());
    std::vector<float> samples(9600, 0.875f);
    std::fill(samples.begin() + 3072, samples.begin() + 4608, 0.125f);
    std::fill(samples.begin() + 4608, samples.begin() + 6144, 0.25f);
    const auto tracks = TrackList::Create(nullptr);
    tracks->Add(makeTrack({ samples }));
    for (const double speed : { 0.5, 2.0, 32.0 }) {
        SCOPED_TRACE(speed);
        TracktionSession session(runtime, *tracks, TracktionSession::Role::Playback);
        auto& transport = session.edit().getTransport();
        transport.setLoopRange({ tracktion::TimePosition::fromSeconds(3072.0 / 48000.0),
                                 tracktion::TimePosition::fromSeconds(6144.0 / 48000.0) });
        transport.looping = true;
        transport.setPosition(tracktion::TimePosition::fromSeconds(3072.0 / 48000.0));
        session.setPlaybackSpeed(speed);
        transport.play(false);
        const auto audio = playback.process(24);
        for (int frame = 64; frame < audio.getNumSamples(); ++frame) {
            const double phase = std::fmod(frame * speed, 3072.0);
            if (phase < 64.0 || phase > 3008.0 || std::abs(phase - 1536.0) < 64.0) {
                continue;
            }
            const float expected = phase < 1536.0 ? 0.125f : 0.25f;
            ASSERT_NEAR(audio.getSample(0, frame), expected, 1e-5f) << frame;
        }
        transport.stop(false, true, false);
    }
}

TEST_F(AudioSourceTests, MovingTheLoopEndJustBehindThePlayheadKeepsLooping)
{
    const auto directory = m_projectFile.getSiblingFile("tracktion-loop-move-" + juce::Uuid().toString());
    DEFER { directory.deleteRecursively();
    };
    TracktionRuntime runtime;
    runtime.init(directory.getChildFile("cache").getFullPathName().toStdString(),
                 directory.getChildFile("settings").getFullPathName().toStdString());
    HostedPlayback playback(runtime.engine());
    std::vector<float> samples(48000, 0.75f);
    std::fill(samples.begin() + 4800, samples.begin() + 14400, 0.125f);
    const auto tracks = TrackList::Create(nullptr);
    tracks->Add(makeTrack({ samples }));
    for (const double speed : { 0.5, 1.0, 2.0 }) {
        SCOPED_TRACE(speed);
        TracktionSession session(runtime, *tracks, TracktionSession::Role::Playback);
        auto& transport = session.edit().getTransport();
        transport.setPosition(tracktion::TimePosition::fromSeconds(0.4));
        ASSERT_TRUE(session.updatePlaybackLoop(true, 0.1, 0.7, TracktionSession::PlaybackLoopUpdate::Start));
        session.setPlaybackSpeed(speed);
        transport.play(false);
        const auto before = playback.process(2);
        EXPECT_NEAR(before.getSample(0, before.getNumSamples() - 1), 0.75f, 1e-5f);
        EXPECT_TRUE(session.updatePlaybackLoop(true, 0.1, 0.3, TracktionSession::PlaybackLoopUpdate::RegionChanged));
        EXPECT_TRUE(transport.looping);
        EXPECT_NEAR(transport.getPosition().inSeconds(), 0.1, 1.0 / 48000.0);
        const auto after = playback.process(12);
        EXPECT_NEAR(after.getSample(0, after.getNumSamples() - 1), 0.125f, 1e-5f);
        transport.stop(false, true, false);
    }
}

TEST_F(AudioSourceTests, EnablingALoopNearItsEndKeepsTheAudacityCaptureAllowance)
{
    const auto directory = m_projectFile.getSiblingFile("tracktion-loop-enable-" + juce::Uuid().toString());
    DEFER { directory.deleteRecursively();
    };
    TracktionRuntime runtime;
    runtime.init(directory.getChildFile("cache").getFullPathName().toStdString(),
                 directory.getChildFile("settings").getFullPathName().toStdString());
    HostedPlayback playback(runtime.engine());
    const auto tracks = TrackList::Create(nullptr);
    tracks->Add(makeTrack({ std::vector<float>(96000, 0.25f) }));
    TracktionSession session(runtime, *tracks, TracktionSession::Role::Playback);
    auto& transport = session.edit().getTransport();
    for (const double position : { 0.4, 0.8, 0.9 }) {
        SCOPED_TRACE(position);
        transport.looping = false;
        transport.setPosition(tracktion::TimePosition::fromSeconds(position));
        EXPECT_FALSE(session.updatePlaybackLoop(true, 0.1, 0.3, TracktionSession::PlaybackLoopUpdate::Start));
        const bool expected = position <= 0.8;
        EXPECT_EQ(session.updatePlaybackLoop(true, 0.1, 0.3, TracktionSession::PlaybackLoopUpdate::RegionChanged), expected);
        EXPECT_EQ(transport.looping.get(), expected);
        EXPECT_NEAR(transport.getPosition().inSeconds(), expected ? 0.1 : position, 1.0 / 48000.0);
    }
    transport.setPosition(tracktion::TimePosition::fromSeconds(0.2));
    EXPECT_FALSE(session.updatePlaybackLoop(true, 0.3, 0.3, TracktionSession::PlaybackLoopUpdate::RegionChanged));
    EXPECT_FALSE(session.updatePlaybackLoop(false, 0.1, 0.3, TracktionSession::PlaybackLoopUpdate::RegionChanged));
    EXPECT_NEAR(transport.getPosition().inSeconds(), 0.2, 1.0 / 48000.0);
}

TEST_F(AudioSourceTests, MovingTheLoopStartAheadOfThePlayheadKeepsTheAudioBeforeTheLoop)
{
    const auto directory = m_projectFile.getSiblingFile("tracktion-loop-roll-in-" + juce::Uuid().toString());
    DEFER { directory.deleteRecursively();
    };
    TracktionRuntime runtime;
    runtime.init(directory.getChildFile("cache").getFullPathName().toStdString(),
                 directory.getChildFile("settings").getFullPathName().toStdString());
    HostedPlayback playback(runtime.engine());
    std::vector<float> samples(48000, 0.75f);
    std::fill(samples.begin(), samples.begin() + 24000, 0.125f);
    std::fill(samples.begin() + 24000, samples.begin() + 33600, 0.25f);
    const auto tracks = TrackList::Create(nullptr);
    tracks->Add(makeTrack({ samples }));
    TracktionSession session(runtime, *tracks, TracktionSession::Role::Playback);
    auto& transport = session.edit().getTransport();
    transport.setPosition(tracktion::TimePosition::fromSeconds(0.2));
    ASSERT_TRUE(session.updatePlaybackLoop(true, 0.1, 0.9, TracktionSession::PlaybackLoopUpdate::Start));
    transport.play(false);
    playback.process(2);
    ASSERT_TRUE(session.updatePlaybackLoop(true, 0.5, 0.7, TracktionSession::PlaybackLoopUpdate::RegionChanged));
    EXPECT_LT(transport.getPosition().inSeconds(), 0.5);
    auto* context = transport.getCurrentPlaybackContext();
    ASSERT_TRUE(context);
    ASSERT_TRUE(context->getNodePlayHead());
    EXPECT_TRUE(context->getNodePlayHead()->isRollingIntoLoop());
    const auto before = playback.process(12);
    EXPECT_NEAR(before.getSample(0, before.getNumSamples() - 1), 0.125f, 1e-5f);
    const auto after = playback.process(140);
    EXPECT_NEAR(after.getSample(0, after.getNumSamples() - 1), 0.25f, 1e-5f);
    EXPECT_GE(context->getAudibleTimelineTime().inSeconds(), 0.5);
    EXPECT_LT(context->getAudibleTimelineTime().inSeconds(), 0.7);
    EXPECT_TRUE(transport.isPlaying());
    transport.stop(false, true, false);
}

TEST_F(AudioSourceTests, DisablingTheLoopContinuesAfterTheOldLoopEnd)
{
    const auto directory = m_projectFile.getSiblingFile("tracktion-loop-disable-" + juce::Uuid().toString());
    DEFER { directory.deleteRecursively();
    };
    TracktionRuntime runtime;
    runtime.init(directory.getChildFile("cache").getFullPathName().toStdString(),
                 directory.getChildFile("settings").getFullPathName().toStdString());
    HostedPlayback playback(runtime.engine());
    std::vector<float> samples(48000, 0.75f);
    std::fill(samples.begin() + 4800, samples.begin() + 14400, 0.125f);
    const auto tracks = TrackList::Create(nullptr);
    tracks->Add(makeTrack({ samples }));
    TracktionSession session(runtime, *tracks, TracktionSession::Role::Playback);
    auto& transport = session.edit().getTransport();
    transport.setPosition(tracktion::TimePosition::fromSeconds(0.15));
    ASSERT_TRUE(session.updatePlaybackLoop(true, 0.1, 0.3, TracktionSession::PlaybackLoopUpdate::Start));
    transport.play(false);
    const auto before = playback.process(2);
    EXPECT_NEAR(before.getSample(0, before.getNumSamples() - 1), 0.125f, 1e-5f);
    EXPECT_FALSE(session.updatePlaybackLoop(false, 0.1, 0.3, TracktionSession::PlaybackLoopUpdate::RegionChanged));
    session.setPlaybackEnd(1.0);
    const auto after = playback.process(40);
    EXPECT_NEAR(after.getSample(0, after.getNumSamples() - 1), 0.75f, 1e-5f);
    EXPECT_GT(transport.getCurrentPlaybackContext()->getAudibleTimelineTime().inSeconds(), 0.3);
    EXPECT_TRUE(transport.isPlaying());
    transport.stop(false, true, false);
}

TEST_F(AudioSourceTests, TracktionPlaybackLoopsTheSelectedRange)
{
    const auto directory = m_projectFile.getSiblingFile("tracktion-loop-" + juce::Uuid().toString());
    DEFER { directory.deleteRecursively();
    };
    TracktionRuntime runtime;
    runtime.init(directory.getChildFile("cache").getFullPathName().toStdString(),
                 directory.getChildFile("settings").getFullPathName().toStdString());
    HostedPlayback playback(runtime.engine());

    std::vector<float> samples(48000, 0.0f);
    std::fill(samples.begin() + 4800, samples.begin() + 9600, 0.125f);
    const auto tracks = TrackList::Create(nullptr);
    tracks->Add(makeTrack({ samples }));
    TracktionSession session(runtime, *tracks, TracktionSession::Role::Playback);
    auto& transport = session.edit().getTransport();
    transport.setLoopRange({ tracktion::TimePosition::fromSeconds(0.1), tracktion::TimePosition::fromSeconds(0.2) });
    transport.looping = true;
    transport.setPosition(tracktion::TimePosition::fromSeconds(0.1));
    transport.ensureContextAllocated();
    transport.play(false);

    const auto output = playback.process(72);
    EXPECT_GT(output.getRMSLevel(0, 12000, 6000), 0.12f);
    const double position = transport.getCurrentPlaybackContext()->getAudibleTimelineTime().inSeconds();
    EXPECT_GE(position, 0.1);
    EXPECT_LT(position, 0.2);
    EXPECT_TRUE(transport.isPlaying());
    transport.stop(false, true, false);
}

TEST_F(AudioSourceTests, TracktionPlaybackFollowsAudacityClipPitch)
{
    for (const double stretch : { 1.0, 1.5 }) {
        SCOPED_TRACE(stretch);
        const auto directory = m_projectFile.getSiblingFile("tracktion-live-pitch-" + juce::Uuid().toString());
        DEFER { directory.deleteRecursively();
        };
        TracktionRuntime runtime;
        runtime.init(directory.getChildFile("cache").getFullPathName().toStdString(),
                     directory.getChildFile("settings").getFullPathName().toStdString());
        HostedPlayback playback(runtime.engine());
        std::vector<float> samples(8 * 48000);
        for (size_t frame = 0; frame < samples.size(); ++frame) {
            samples[frame] = 0.25f * std::sin(2.0 * std::numbers::pi * 440.0 * frame / 48000.0);
        }
        const auto track = makeTrack({ samples });
        const auto clip = *track->Intervals().begin();
        clip->StretchBy(stretch);
        const auto tracks = TrackList::Create(nullptr);
        tracks->Add(track);
        TracktionSession session(runtime, *tracks, TracktionSession::Role::Playback);
        auto& transport = session.edit().getTransport();
        transport.ensureContextAllocated();
        transport.play(false);
        for (const int cents : { 0, 1200, -1200, 0 }) {
            SCOPED_TRACE(cents);
            ASSERT_TRUE(clip->SetCentShift(cents));
            session.edit().dispatchPendingUpdatesSynchronously();
            const auto output = playback.process(192);
            const int first = output.getNumSamples() - 24000;
            int crossings = 0;
            for (int frame = first + 1; frame < output.getNumSamples(); ++frame) {
                crossings += output.getSample(0, frame - 1) <= 0.0f && output.getSample(0, frame) > 0.0f;
            }
            EXPECT_NEAR(crossings, 220.0 * std::pow(2.0, cents / 1200.0), 2.0);
            EXPECT_GT(output.getRMSLevel(0, first, 24000), 0.1f);
            EXPECT_TRUE(transport.isPlaying());
        }
        transport.stop(false, true, false);
    }
}

TEST_F(AudioSourceTests, TracktionPlaybackUpdatesVoiceFormantsWithoutResettingThePosition)
{
    const auto directory = m_projectFile.getSiblingFile("tracktion-live-voice-" + juce::Uuid().toString());
    DEFER { directory.deleteRecursively();
    };
    TracktionRuntime runtime;
    runtime.init(directory.getChildFile("cache").getFullPathName().toStdString(),
                 directory.getChildFile("settings").getFullPathName().toStdString());
    HostedPlayback playback(runtime.engine());
    const auto track = makeTrack({ makeVoiceSamples(8 * 48000) });
    const auto clip = *track->Intervals().begin();
    clip->StretchBy(1.5);
    const auto tracks = TrackList::Create(nullptr);
    tracks->Add(track);
    TracktionSession session(runtime, *tracks, TracktionSession::Role::Playback);
    auto& transport = session.edit().getTransport();
    transport.ensureContextAllocated();
    transport.play(false);
    DEFER { transport.stop(false, true, false);
    };
    struct Change {
        int cents;
        bool voice;
    };
    for (const auto change : { Change { 700, false }, Change { 700, true }, Change { -700, true },
                               Change { -700, false }, Change { 0, true } }) {
        SCOPED_TRACE(change.cents);
        SCOPED_TRACE(change.voice);
        const auto before = transport.getPosition();
        ASSERT_TRUE(clip->SetCentShift(change.cents));
        clip->SetPitchAndSpeedPreset(change.voice ? PitchAndSpeedPreset::OptimizeForVoice : PitchAndSpeedPreset::Default);
        session.edit().dispatchPendingUpdatesSynchronously();
        EXPECT_GE(transport.getPosition().inSeconds(), before.inSeconds() - 256.0 / 48000.0);
        const auto output = playback.process(192);
        const double pitch = std::pow(2.0, change.cents / 1200.0);
        EXPECT_NEAR(peakFrequency(output, 50.0, 220.0, 24000), 100.0 * pitch, 4.0);
        EXPECT_NEAR(peakFrequency(output, 600.0, 1800.0, 24000), change.voice ? 1000.0 : 1000.0 * pitch, 120.0);
        EXPECT_TRUE(transport.isPlaying());
        EXPECT_GT(transport.getPosition().inSeconds(), before.inSeconds() + 0.5);
    }
}

TEST_F(AudioSourceTests, TracktionPlaybackFollowsAudacityClipEnvelope)
{
    const auto directory = m_projectFile.getSiblingFile("tracktion-live-envelope-" + juce::Uuid().toString());
    DEFER { directory.deleteRecursively();
    };
    TracktionRuntime runtime;
    runtime.init(directory.getChildFile("cache").getFullPathName().toStdString(),
                 directory.getChildFile("settings").getFullPathName().toStdString());
    HostedPlayback playback(runtime.engine());
    const auto track = makeTrack({ std::vector<float>(96000, 0.25f) });
    const auto clip = *track->Intervals().begin();
    const auto tracks = TrackList::Create(nullptr);
    tracks->Add(track);
    TracktionSession session(runtime, *tracks, TracktionSession::Role::Playback);
    auto& transport = session.edit().getTransport();
    transport.ensureContextAllocated();
    transport.play(false);
    for (const double gain : { 1.0, 0.5, 0.0, 1.0 }) {
        SCOPED_TRACE(gain);
        clip->GetEnvelope().Flatten(gain);
        const auto output = playback.process(24);
        for (int channel = 0; channel < 2; ++channel) {
            for (int frame = output.getNumSamples() - 256; frame < output.getNumSamples(); ++frame) {
                ASSERT_NEAR(output.getSample(channel, frame), 0.25f * gain, 1e-5f);
            }
        }
        EXPECT_TRUE(transport.isPlaying());
    }
    transport.stop(false, true, false);
}

TEST_F(AudioSourceTests, TracktionRenderKeepsClipPitchAndEnvelopeSnapshot)
{
    const auto directory = m_projectFile.getSiblingFile("tracktion-render-envelope-" + juce::Uuid().toString());
    DEFER { directory.deleteRecursively();
    };
    TracktionRuntime runtime;
    runtime.init(directory.getChildFile("cache").getFullPathName().toStdString(),
                 directory.getChildFile("settings").getFullPathName().toStdString());
    std::vector<float> samples(48000);
    for (size_t frame = 0; frame < samples.size(); ++frame) {
        samples[frame] = 0.25f * std::sin(2.0 * std::numbers::pi * 440.0 * frame / 48000.0);
    }
    const auto track = makeTrack({ samples });
    const auto clip = *track->Intervals().begin();
    clip->GetEnvelope().Flatten(0.5);
    const auto tracks = TrackList::Create(nullptr);
    tracks->Add(track);
    TracktionSession session(runtime, *tracks, TracktionSession::Role::Render);
    ASSERT_TRUE(clip->SetCentShift(1200));
    clip->GetEnvelope().Flatten(0.0);
    juce::Timer::callPendingTimersSynchronously();
    TracktionRenderSource source(session, { 0.0, 1.0, 48000.0, 2, 512 });
    size_t total = 0;
    while (const auto frames = source.process(257)) {
        const auto& output = source.buffer();
        ASSERT_LE(total + frames, samples.size());
        for (int channel = 0; channel < 2; ++channel) {
            for (int frame = 0; frame < frames; ++frame) {
                ASSERT_NEAR(output.getSample(channel, frame), samples[total + frame] * 0.5f, 1e-5f);
            }
        }
        total += frames;
    }
    EXPECT_EQ(total, samples.size());
}

TEST_F(AudioSourceTests, TracktionTrackMetersKeepMonoPanAndStereoChannels)
{
    const auto directory = m_projectFile.getSiblingFile("tracktion-track-meters-" + juce::Uuid().toString());
    DEFER { directory.deleteRecursively();
    };
    TracktionRuntime runtime;
    runtime.init(directory.getChildFile("cache").getFullPathName().toStdString(),
                 directory.getChildFile("settings").getFullPathName().toStdString());
    const auto callback = std::make_shared<TracktionAudioCallbacks>();
    const auto meter = std::make_shared<MeterCapture>();
    callback->setMeters({}, meter);
    HostedPlayback playback(runtime.engine(), 0, 0, 2, callback);
    const auto mono = makeTrack({ std::vector<float>(48000, 0.125f) });
    mono->SetPan(1.0f);
    mono->SetVolume(0.5f);
    const auto stereo = makeTrack({ std::vector<float>(48000, 0.25f), std::vector<float>(48000, -0.125f) });
    stereo->SetPan(0.5f);
    const auto tracks = TrackList::Create(nullptr);
    tracks->Add(mono);
    tracks->Add(stereo);
    TracktionSession session(runtime, *tracks, TracktionSession::Role::Playback);
    callback->setTrackMeters(session.trackMeters());
    auto& transport = session.edit().getTransport();
    transport.ensureContextAllocated();
    transport.play(false);
    playback.process(24);
    const auto* monoLevel = meter->latest(mono->GetId(), 0);
    ASSERT_TRUE(monoLevel);
    EXPECT_NEAR(monoLevel->levels.peak, 0.0625f, 1e-5f);
    EXPECT_NEAR(monoLevel->levels.rms, 0.0625f, 1e-5f);
    EXPECT_FALSE(meter->latest(mono->GetId(), 1));
    for (const auto channel : { 0, 1 }) {
        const auto* level = meter->latest(stereo->GetId(), channel);
        ASSERT_TRUE(level);
        EXPECT_NEAR(level->levels.peak, 0.125f, 1e-5f);
        EXPECT_NEAR(level->levels.rms, 0.125f, 1e-5f);
    }
    transport.stop(false, true, false);
    callback->stop();
}

TEST_F(AudioSourceTests, TracktionTrackMetersSendBothPeakAndRmsAtTheDeviceTime)
{
    const auto directory = m_projectFile.getSiblingFile("tracktion-meter-clock-" + juce::Uuid().toString());
    DEFER { directory.deleteRecursively();
    };
    TracktionRuntime runtime;
    runtime.init(directory.getChildFile("cache").getFullPathName().toStdString(),
                 directory.getChildFile("settings").getFullPathName().toStdString());
    const auto callback = std::make_shared<TracktionAudioCallbacks>();
    const auto meter = std::make_shared<MeterCapture>();
    callback->setMeters({}, meter);
    HostedPlayback playback(runtime.engine(), 0, 0, 2, callback);
    std::vector<float> samples(48000);
    for (size_t frame = 0; frame < samples.size(); ++frame) {
        samples[frame] = 0.25f * std::sin(2.0 * std::numbers::pi * 440.0 * frame / 48000.0);
    }
    const auto track = makeTrack({ samples, samples });
    const auto tracks = TrackList::Create(nullptr);
    tracks->Add(track);
    TracktionSession session(runtime, *tracks, TracktionSession::Role::Playback);
    callback->setTrackMeters(session.trackMeters());
    auto& transport = session.edit().getTransport();
    transport.ensureContextAllocated();
    transport.play(false);
    playback.process(24);
    ASSERT_EQ(meter->entries.size(), 24u * 4u);
    for (size_t block = 0; block < 24; ++block) {
        for (size_t channel = 0; channel < 2; ++channel) {
            const auto& master = meter->entries[block * 4 + channel];
            const auto& level = meter->entries[block * 4 + 2 + channel];
            ASSERT_TRUE(level.track);
            EXPECT_EQ(level.track->value, track->GetId());
            EXPECT_EQ(level.channel, channel);
            EXPECT_EQ(level.levels.frames, 256u);
            EXPECT_EQ(level.levels.dacTime, master.levels.dacTime);
            EXPECT_NEAR(level.levels.peak, master.levels.peak, 1e-5f) << block;
            EXPECT_NEAR(level.levels.rms, master.levels.rms, 1e-5f) << block;
        }
    }
    const auto& level = meter->entries.back().levels;
    EXPECT_GT(level.peak - level.rms, 0.04f);
    transport.stop(false, true, false);
    callback->stop();
}

TEST_F(AudioSourceTests, TracktionTrackMetersFollowMuteSoloAndPause)
{
    const auto directory = m_projectFile.getSiblingFile("tracktion-meter-mute-" + juce::Uuid().toString());
    DEFER { directory.deleteRecursively();
    };
    TracktionRuntime runtime;
    runtime.init(directory.getChildFile("cache").getFullPathName().toStdString(),
                 directory.getChildFile("settings").getFullPathName().toStdString());
    const auto callback = std::make_shared<TracktionAudioCallbacks>();
    const auto meter = std::make_shared<MeterCapture>();
    callback->setMeters({}, meter);
    HostedPlayback playback(runtime.engine(), 0, 0, 2, callback);
    const auto first = makeTrack({ std::vector<float>(96000, 0.125f) });
    const auto second = makeTrack({ std::vector<float>(96000, 0.25f) });
    const auto tracks = TrackList::Create(nullptr);
    tracks->Add(first);
    tracks->Add(second);
    TracktionSession session(runtime, *tracks, TracktionSession::Role::Playback);
    callback->setTrackMeters(session.trackMeters());
    auto& transport = session.edit().getTransport();
    transport.ensureContextAllocated();
    transport.play(false);
    const auto check = [&](float firstValue, float secondValue) {
        playback.process(24);
        for (const auto& [track, expected] : { std::pair { first, firstValue }, std::pair { second, secondValue } }) {
            SCOPED_TRACE(track->GetId());
            SCOPED_TRACE(expected);
            const auto* level = meter->latest(track->GetId(), 0);
            ASSERT_TRUE(level);
            EXPECT_NEAR(level->levels.peak, expected, 1e-5f);
            EXPECT_NEAR(level->levels.rms, expected, 1e-5f);
        }
    };
    ASSERT_NO_FATAL_FAILURE(check(0.125f, 0.25f));
    second->SetSolo(true);
    session.edit().dispatchPendingUpdatesSynchronously();
    ASSERT_NO_FATAL_FAILURE(check(0.0f, 0.25f));
    second->SetMute(true);
    session.edit().dispatchPendingUpdatesSynchronously();
    ASSERT_NO_FATAL_FAILURE(check(0.0f, 0.0f));
    second->SetMute(false);
    transport.setPaused(true);
    ASSERT_NO_FATAL_FAILURE(check(0.0f, 0.0f));
    transport.setPaused(false);
    session.edit().dispatchPendingUpdatesSynchronously();
    ASSERT_NO_FATAL_FAILURE(check(0.0f, 0.25f));
    transport.stop(false, true, false);
    callback->stop();
}

TEST_F(AudioSourceTests, TracktionTrackMetersAccountForTrackAndMasterLatency)
{
    const auto directory = m_projectFile.getSiblingFile("tracktion-meter-delay-" + juce::Uuid().toString());
    DEFER { directory.deleteRecursively();
    };
    TracktionRuntime runtime;
    runtime.init(directory.getChildFile("cache").getFullPathName().toStdString(),
                 directory.getChildFile("settings").getFullPathName().toStdString());
    const auto callback = std::make_shared<TracktionAudioCallbacks>();
    const auto meter = std::make_shared<MeterCapture>();
    callback->setMeters({}, meter);
    HostedPlayback playback(runtime.engine(), 0, 0, 2, callback);
    std::vector<float> samples(48000, 0.0f);
    samples[2400] = 0.5f;
    const auto track = makeTrack({ samples });
    auto& project = *reinterpret_cast<au3::Au3Project*>(m_project->au3ProjectPtr());
    auto& tracks = TrackList::Get(project);
    tracks.Add(track);
    for (auto* list : { &RealtimeEffectList::Get(*track), &RealtimeEffectList::Get(project) }) {
        const auto state = addBuiltinEffect(*list, wxT("tracktion-compressor"));
        ASSERT_TRUE(state);
        const auto access = state->GetAccess();
        access->ModifySettings([](EffectSettings& settings) {
            auto& compressor = effects::CompressorEffect::GetSettings(settings);
            compressor.compressionRatio = 1.0;
            compressor.makeupGainDb = 0.0;
            compressor.lookaheadMs = 10.0;
            return nullptr;
        });
        access->Flush();
    }
    TracktionSession session(runtime, tracks, TracktionSession::Role::Playback);
    callback->setTrackMeters(session.trackMeters());
    auto& transport = session.edit().getTransport();
    transport.ensureContextAllocated();
    transport.play(false);
    playback.process(24);
    ASSERT_EQ(meter->entries.size(), 24u * 3u);
    float largestPeak = 0.0f;
    for (size_t block = 0; block < 24; ++block) {
        const auto& master = meter->entries[block * 3];
        const auto& level = meter->entries[block * 3 + 2];
        EXPECT_NEAR(level.levels.peak, master.levels.peak, 1e-5f) << block;
        EXPECT_NEAR(level.levels.rms, master.levels.rms, 1e-5f) << block;
        EXPECT_EQ(level.levels.dacTime, master.levels.dacTime);
        largestPeak = std::max(largestPeak, level.levels.peak);
    }
    EXPECT_NEAR(largestPeak, 0.5f, 1e-5f);
    transport.stop(false, true, false);
    callback->stop();
}

TEST_F(AudioSourceTests, TracktionTrackMetersIncludeTrackEffectsAndExcludeMasterGain)
{
    const auto directory = m_projectFile.getSiblingFile("tracktion-meter-effects-" + juce::Uuid().toString());
    DEFER { directory.deleteRecursively();
    };
    TracktionRuntime runtime;
    runtime.init(directory.getChildFile("cache").getFullPathName().toStdString(),
                 directory.getChildFile("settings").getFullPathName().toStdString());
    const auto callback = std::make_shared<TracktionAudioCallbacks>();
    const auto meter = std::make_shared<MeterCapture>();
    callback->setMeters({}, meter);
    HostedPlayback playback(runtime.engine(), 0, 0, 2, callback);
    const auto track = makeTrack({ std::vector<float>(48000, 0.25f) });
    track->SetVolume(0.5f);
    auto& project = *reinterpret_cast<au3::Au3Project*>(m_project->au3ProjectPtr());
    auto& tracks = TrackList::Get(project);
    tracks.Add(track);
    const auto trackEffect = addBuiltinEffect(RealtimeEffectList::Get(*track), wxT("tracktion-bass-treble"));
    const auto masterEffect = addBuiltinEffect(RealtimeEffectList::Get(project), wxT("tracktion-bass-treble"));
    ASSERT_TRUE(trackEffect);
    ASSERT_TRUE(masterEffect);
    for (const auto& [state, gain] : { std::pair { trackEffect, 6.020599913 }, std::pair { masterEffect, 12.041199826 } }) {
        const auto access = state->GetAccess();
        access->ModifySettings([gain](EffectSettings& settings) {
            effects::BassTrebleEffect::GetSettings(settings).mGain = gain;
            return nullptr;
        });
        access->Flush();
    }
    TracktionSession session(runtime, tracks, TracktionSession::Role::Playback);
    session.setPlaybackVolume(0.5f);
    callback->setTrackMeters(session.trackMeters());
    auto& transport = session.edit().getTransport();
    transport.ensureContextAllocated();
    transport.play(false);
    const auto output = playback.process(24);
    EXPECT_NEAR(output.getSample(0, output.getNumSamples() - 1), 0.5f, 1e-5f);
    const auto* level = meter->latest(track->GetId(), 0);
    ASSERT_TRUE(level);
    EXPECT_NEAR(level->levels.peak, 0.25f, 1e-5f);
    EXPECT_NEAR(level->levels.rms, 0.25f, 1e-5f);
    transport.stop(false, true, false);
    callback->stop();
}

TEST_F(AudioSourceTests, TracktionTrackMetersKeepRmsWhenTheLoopSplitsABlock)
{
    const auto directory = m_projectFile.getSiblingFile("tracktion-meter-loop-" + juce::Uuid().toString());
    DEFER { directory.deleteRecursively();
    };
    TracktionRuntime runtime;
    runtime.init(directory.getChildFile("cache").getFullPathName().toStdString(),
                 directory.getChildFile("settings").getFullPathName().toStdString());
    const auto callback = std::make_shared<TracktionAudioCallbacks>();
    const auto meter = std::make_shared<MeterCapture>();
    callback->setMeters({}, meter);
    HostedPlayback playback(runtime.engine(), 0, 0, 2, callback);
    std::vector<float> samples(48000, 0.0f);
    std::fill(samples.begin() + 4800, samples.begin() + 4973, 0.25f);
    std::fill(samples.begin() + 4973, samples.begin() + 5147, 0.5f);
    const auto track = makeTrack({ samples });
    const auto tracks = TrackList::Create(nullptr);
    tracks->Add(track);
    TracktionSession session(runtime, *tracks, TracktionSession::Role::Playback);
    callback->setTrackMeters(session.trackMeters());
    auto& transport = session.edit().getTransport();
    transport.setLoopRange({ tracktion::TimePosition::fromSeconds(0.1), tracktion::TimePosition::fromSeconds(5147.0 / 48000.0) });
    transport.looping = true;
    transport.setPosition(tracktion::TimePosition::fromSeconds(0.1));
    transport.ensureContextAllocated();
    ASSERT_TRUE(session.startPlayback());
    const auto output = playback.process(24);
    EXPECT_GT(output.getMagnitude(0, output.getNumSamples()), 0.0f);
    ASSERT_EQ(meter->entries.size(), 24u * 3u);
    for (size_t block = 0; block < 24; ++block) {
        const auto& master = meter->entries[block * 3];
        const auto& level = meter->entries[block * 3 + 2];
        EXPECT_NEAR(level.levels.peak, master.levels.peak, 1e-5f) << block;
        EXPECT_NEAR(level.levels.rms, master.levels.rms, 1e-5f) << block;
    }
    transport.stop(false, true, false);
    callback->stop();
}

TEST_F(AudioSourceTests, TracktionTrackMetersDiscardLevelsWhenNoMeterIsAttached)
{
    const auto directory = m_projectFile.getSiblingFile("tracktion-meter-attach-" + juce::Uuid().toString());
    DEFER { directory.deleteRecursively();
    };
    TracktionRuntime runtime;
    runtime.init(directory.getChildFile("cache").getFullPathName().toStdString(),
                 directory.getChildFile("settings").getFullPathName().toStdString());
    const auto callback = std::make_shared<TracktionAudioCallbacks>();
    HostedPlayback playback(runtime.engine(), 0, 0, 2, callback);
    const auto track = makeTrack({ std::vector<float>(48000, 0.25f) });
    const auto tracks = TrackList::Create(nullptr);
    tracks->Add(track);
    TracktionSession session(runtime, *tracks, TracktionSession::Role::Playback);
    callback->setTrackMeters(session.trackMeters());
    auto& transport = session.edit().getTransport();
    transport.ensureContextAllocated();
    transport.play(false);
    playback.process(24);
    track->SetVolume(0.1f);
    playback.process(24);
    const auto meter = std::make_shared<MeterCapture>();
    callback->setMeters({}, meter);
    playback.process(1);
    const auto* level = meter->latest(track->GetId(), 0);
    ASSERT_TRUE(level);
    EXPECT_NEAR(level->levels.peak, 0.025f, 1e-5f);
    EXPECT_NEAR(level->levels.rms, 0.025f, 1e-5f);
    transport.stop(false, true, false);
    callback->stop();
}

TEST_F(AudioSourceTests, TracktionTrackMetersDisconnectBeforeTheEditIsDeleted)
{
    const auto directory = m_projectFile.getSiblingFile("tracktion-meter-close-" + juce::Uuid().toString());
    DEFER { directory.deleteRecursively();
    };
    TracktionRuntime runtime;
    runtime.init(directory.getChildFile("cache").getFullPathName().toStdString(),
                 directory.getChildFile("settings").getFullPathName().toStdString());
    const auto callback = std::make_shared<TracktionAudioCallbacks>();
    const auto meter = std::make_shared<MeterCapture>();
    callback->setMeters({}, meter);
    HostedPlayback playback(runtime.engine(), 0, 0, 2, callback);
    const auto tracks = TrackList::Create(nullptr);
    tracks->Add(makeTrack({ std::vector<float>(48000, 0.125f) }));
    {
        TracktionSession session(runtime, *tracks, TracktionSession::Role::Playback);
        callback->setTrackMeters(session.trackMeters());
        auto& transport = session.edit().getTransport();
        transport.ensureContextAllocated();
        transport.play(false);
        playback.process(1);
        transport.stop(false, true, false);
    }
    meter->entries.clear();
    playback.process(1);
    ASSERT_EQ(meter->entries.size(), 2u);
    for (const auto& entry : meter->entries) {
        EXPECT_FALSE(entry.track);
    }
    callback->stop();
}

TEST_F(AudioSourceTests, TracktionPlaybackFollowsAudacityTrackControls)
{
    const auto directory = m_projectFile.getSiblingFile("tracktion-controls-" + juce::Uuid().toString());
    DEFER { directory.deleteRecursively();
    };
    TracktionRuntime runtime;
    runtime.init(directory.getChildFile("cache").getFullPathName().toStdString(),
                 directory.getChildFile("settings").getFullPathName().toStdString());
    HostedPlayback playback(runtime.engine());
    const auto first = makeTrack({ std::vector<float>(96000, 0.05f) });
    const auto second = makeTrack({ std::vector<float>(96000, 0.025f) });
    const auto tracks = TrackList::Create(nullptr);
    tracks->Add(first);
    tracks->Add(second);
    TracktionSession session(runtime, *tracks, TracktionSession::Role::Playback);
    auto& transport = session.edit().getTransport();
    transport.ensureContextAllocated();
    transport.play(false);

    auto output = playback.process(24);
    EXPECT_NEAR(output.getSample(0, output.getNumSamples() - 1), 0.075f, 1e-5f);

    first->SetVolume(3.0f);
    first->SetPan(0.5f);
    output = playback.process(24);
    EXPECT_NEAR(output.getSample(0, output.getNumSamples() - 1), 0.1f, 1e-5f);
    EXPECT_NEAR(output.getSample(1, output.getNumSamples() - 1), 0.175f, 1e-5f);

    second->SetSolo(true);
    session.edit().dispatchPendingUpdatesSynchronously();
    output = playback.process(24);
    EXPECT_NEAR(output.getSample(0, output.getNumSamples() - 1), 0.025f, 1e-5f);
    EXPECT_NEAR(output.getSample(1, output.getNumSamples() - 1), 0.025f, 1e-5f);

    second->SetMute(true);
    session.edit().dispatchPendingUpdatesSynchronously();
    output = playback.process(24);
    EXPECT_FLOAT_EQ(output.getRMSLevel(0, 3000, 3000), 0.0f);
    EXPECT_FLOAT_EQ(output.getRMSLevel(1, 3000, 3000), 0.0f);
    transport.stop(false, true, false);
}

TEST_F(AudioSourceTests, TracktionRenderProcessesAudacityTrackAndMasterEffects)
{
    auto& project = *reinterpret_cast<au3::Au3Project*>(m_project->au3ProjectPtr());
    auto& tracks = TrackList::Get(project);
    const auto track = makeTrack({ std::vector<float>(4800, 0.125f) });
    track->SetVolume(0.5f);
    track->SetPan(0.5f);
    tracks.Add(track);
    const auto trackEffect = addBuiltinEffect(RealtimeEffectList::Get(*track), wxT("tracktion-bass-treble"));
    const auto masterEffect = addBuiltinEffect(RealtimeEffectList::Get(project), wxT("tracktion-bass-treble"));
    ASSERT_TRUE(trackEffect);
    ASSERT_TRUE(masterEffect);
    for (const auto& state : { trackEffect, masterEffect }) {
        auto access = state->GetAccess();
        access->ModifySettings([](EffectSettings& settings) {
            effects::BassTrebleEffect::GetSettings(settings).mGain = 6.020599913;
            return nullptr;
        });
        access->Flush();
    }

    juce::AudioBuffer<float> rendered;
    ASSERT_NO_FATAL_FAILURE(renderTracks(tracks, rendered, 4800));
    EXPECT_NEAR(rendered.getSample(0, 2400), 0.125f, 1e-5f);
    EXPECT_NEAR(rendered.getSample(1, 2400), 0.25f, 1e-5f);

    RealtimeEffectList::Get(project).SetActive(false);
    ASSERT_NO_FATAL_FAILURE(renderTracks(tracks, rendered, 4800));
    EXPECT_NEAR(rendered.getSample(0, 2400), 0.0625f, 1e-5f);
    EXPECT_NEAR(rendered.getSample(1, 2400), 0.125f, 1e-5f);
}

TEST_F(AudioSourceTests, TracktionRenderBalancesAudacityEffectLatency)
{
    std::vector<float> samples(4800, 0.0f);
    samples[0] = 0.125f;
    samples[2400] = 0.125f;
    const auto first = makeTrack({ samples });
    const auto second = makeTrack({ samples });
    const auto tracks = TrackList::Create(nullptr);
    tracks->Add(first);
    tracks->Add(second);
    const auto state = addBuiltinEffect(RealtimeEffectList::Get(*first), wxT("tracktion-compressor"));
    ASSERT_TRUE(state);
    auto access = state->GetAccess();
    access->ModifySettings([](EffectSettings& settings) {
        auto& compressor = effects::CompressorEffect::GetSettings(settings);
        compressor.compressionRatio = 1.0;
        compressor.makeupGainDb = 0.0;
        compressor.lookaheadMs = 10.0;
        return nullptr;
    });
    access->Flush();

    juce::AudioBuffer<float> rendered;
    ASSERT_NO_FATAL_FAILURE(renderTracks(*tracks, rendered, 4800));
    EXPECT_NEAR(rendered.getSample(0, 0), 0.25f, 1e-5f);
    EXPECT_NEAR(rendered.getSample(1, 0), 0.25f, 1e-5f);
    EXPECT_NEAR(rendered.getSample(0, 2400), 0.25f, 1e-5f);
    EXPECT_NEAR(rendered.getSample(1, 2400), 0.25f, 1e-5f);
    std::vector<int> impulseFrames;
    for (int frame = 0; frame < rendered.getNumSamples(); ++frame) {
        if (std::abs(rendered.getSample(0, frame)) > 1e-5f) {
            impulseFrames.push_back(frame);
        }
    }
    EXPECT_EQ(impulseFrames, (std::vector<int> { 0, 2400 }));

    state->SetActive(false);
    ASSERT_NO_FATAL_FAILURE(renderTracks(*tracks, rendered, 4800));
    EXPECT_NEAR(rendered.getSample(0, 0), 0.25f, 1e-5f);
    EXPECT_NEAR(rendered.getSample(1, 0), 0.25f, 1e-5f);
    EXPECT_NEAR(rendered.getSample(0, 2400), 0.25f, 1e-5f);
    EXPECT_NEAR(rendered.getSample(1, 2400), 0.25f, 1e-5f);
}

TEST_F(AudioSourceTests, TracktionPlaybackUpdatesAudacityEffectsOnMutedTracks)
{
    const auto directory = m_projectFile.getSiblingFile("tracktion-effect-settings-" + juce::Uuid().toString());
    DEFER { directory.deleteRecursively();
    };
    TracktionRuntime runtime;
    runtime.init(directory.getChildFile("cache").getFullPathName().toStdString(),
                 directory.getChildFile("settings").getFullPathName().toStdString());
    HostedPlayback playback(runtime.engine());
    const auto track = makeTrack({ std::vector<float>(96000, 0.125f) });
    const auto tracks = TrackList::Create(nullptr);
    tracks->Add(track);
    const auto state = addBuiltinEffect(RealtimeEffectList::Get(*track), wxT("tracktion-bass-treble"));
    ASSERT_TRUE(state);
    TracktionSession session(runtime, *tracks, TracktionSession::Role::Playback);
    auto& transport = session.edit().getTransport();
    transport.ensureContextAllocated();
    transport.play(false);
    playback.process(24);
    track->SetMute(true);
    session.edit().dispatchPendingUpdatesSynchronously();

    auto output = std::async(std::launch::async, [&playback] { return playback.process(128, false); });
    state->SetActive(false);
    auto access = state->GetAccess();
    access->ModifySettings([](EffectSettings& settings) {
        effects::BassTrebleEffect::GetSettings(settings).mGain = 12.041199826;
        return nullptr;
    });
    access->Flush();
    state->SetActive(true);
    track->SetMute(false);
    session.edit().dispatchPendingUpdatesSynchronously();
    const auto audio = output.get();
    EXPECT_NEAR(audio.getSample(0, audio.getNumSamples() - 1), 0.5f, 1e-5f);
    EXPECT_NEAR(audio.getSample(1, audio.getNumSamples() - 1), 0.5f, 1e-5f);
    transport.stop(false, true, false);
}

TEST_F(AudioSourceTests, TracktionPlaybackFollowsAudacityEffectChainChanges)
{
    const auto directory = m_projectFile.getSiblingFile("tracktion-effect-chain-" + juce::Uuid().toString());
    DEFER { directory.deleteRecursively();
    };
    TracktionRuntime runtime;
    runtime.init(directory.getChildFile("cache").getFullPathName().toStdString(),
                 directory.getChildFile("settings").getFullPathName().toStdString());
    HostedPlayback playback(runtime.engine());
    const auto track = makeTrack({ std::vector<float>(144000, 0.125f) });
    const auto tracks = TrackList::Create(nullptr);
    tracks->Add(track);
    auto& list = RealtimeEffectList::Get(*track);
    TracktionSession session(runtime, *tracks, TracktionSession::Role::Playback);
    auto& transport = session.edit().getTransport();
    transport.ensureContextAllocated();
    transport.play(false);
    playback.process(24);

    const auto gain = addBuiltinEffect(list, wxT("tracktion-bass-treble"));
    ASSERT_TRUE(gain);
    auto access = gain->GetAccess();
    access->ModifySettings([](EffectSettings& settings) {
        effects::BassTrebleEffect::GetSettings(settings).mGain = 6.020599913;
        return nullptr;
    });
    access->Flush();
    session.edit().dispatchPendingUpdatesSynchronously();
    auto output = playback.process(24);
    EXPECT_NEAR(output.getSample(0, output.getNumSamples() - 1), 0.25f, 1e-5f);
    const auto gainInstance = gain->PeekInstance();

    const auto compressor = addBuiltinEffect(list, wxT("tracktion-compressor"));
    ASSERT_TRUE(compressor);
    access = compressor->GetAccess();
    access->ModifySettings([](EffectSettings& settings) {
        auto& value = effects::CompressorEffect::GetSettings(settings);
        value.thresholdDb = -20.0;
        value.compressionRatio = 2.0;
        value.makeupGainDb = 0.0;
        value.kneeWidthDb = 0.0;
        value.lookaheadMs = 0.0;
        value.attackMs = 0.0;
        value.releaseMs = 0.0;
        return nullptr;
    });
    access->Flush();
    session.edit().dispatchPendingUpdatesSynchronously();
    output = playback.process(24);
    EXPECT_NEAR(output.getSample(0, output.getNumSamples() - 1), 0.15811388f, 1e-3f);

    list.MoveEffect(1, 0);
    session.edit().dispatchPendingUpdatesSynchronously();
    output = playback.process(24);
    EXPECT_NEAR(output.getSample(0, output.getNumSamples() - 1), 0.22360680f, 1e-3f);
    EXPECT_EQ(gain->PeekInstance(), gainInstance);

    list.RemoveState(compressor);
    session.edit().dispatchPendingUpdatesSynchronously();
    output = playback.process(24);
    EXPECT_NEAR(output.getSample(0, output.getNumSamples() - 1), 0.25f, 1e-5f);

    RealtimeEffectList replacementList;
    const auto replacement = addBuiltinEffect(replacementList, wxT("tracktion-bass-treble"));
    ASSERT_TRUE(replacement);
    access = replacement->GetAccess();
    access->ModifySettings([](EffectSettings& settings) {
        effects::BassTrebleEffect::GetSettings(settings).mGain = 12.041199826;
        return nullptr;
    });
    access->Flush();
    ASSERT_TRUE(list.ReplaceState(0, replacement));
    session.edit().dispatchPendingUpdatesSynchronously();
    output = playback.process(24);
    EXPECT_NEAR(output.getSample(0, output.getNumSamples() - 1), 0.5f, 1e-5f);

    list.Clear();
    session.edit().dispatchPendingUpdatesSynchronously();
    output = playback.process(24);
    EXPECT_NEAR(output.getSample(0, output.getNumSamples() - 1), 0.125f, 1e-5f);
    transport.stop(false, true, false);
}

TEST_F(AudioSourceTests, TracktionRenderKeepsTheLiveAudacityEffectInstance)
{
    const auto directory = m_projectFile.getSiblingFile("tracktion-effect-render-" + juce::Uuid().toString());
    DEFER { directory.deleteRecursively();
    };
    TracktionRuntime runtime;
    runtime.init(directory.getChildFile("cache").getFullPathName().toStdString(),
                 directory.getChildFile("settings").getFullPathName().toStdString());
    HostedPlayback playback(runtime.engine());
    std::vector<float> samples(48000, 0.125f);
    std::fill(samples.begin() + 24000, samples.end(), 0.5f);
    const auto track = makeTrack({ samples });
    const auto tracks = TrackList::Create(nullptr);
    tracks->Add(track);
    const auto state = addBuiltinEffect(RealtimeEffectList::Get(*track), wxT("tracktion-bass-treble"));
    ASSERT_TRUE(state);
    auto access = state->GetAccess();
    access->ModifySettings([](EffectSettings& settings) {
        effects::BassTrebleEffect::GetSettings(settings).mGain = 6.020599913;
        return nullptr;
    });
    access->Flush();
    TracktionSession session(runtime, *tracks, TracktionSession::Role::Playback);
    auto& transport = session.edit().getTransport();
    transport.ensureContextAllocated();
    transport.play(false);
    playback.process(24);
    const auto liveInstance = state->PeekInstance();
    ASSERT_TRUE(liveInstance);

    juce::AudioBuffer<float> rendered;
    ASSERT_NO_FATAL_FAILURE(renderTracks(*tracks, rendered, 48000));
    EXPECT_NEAR(rendered.getSample(0, 2400), 0.25f, 1e-5f);
    EXPECT_NEAR(rendered.getSample(0, 36000), 1.0f, 1e-5f);
    EXPECT_EQ(state->PeekInstance(), liveInstance);

    transport.setPosition(tracktion::TimePosition::fromSeconds(0.7));
    const auto output = playback.process(24);
    EXPECT_NEAR(output.getSample(0, output.getNumSamples() - 1), 1.0f, 1e-5f);
    EXPECT_NEAR(output.getSample(1, output.getNumSamples() - 1), 1.0f, 1e-5f);
    transport.stop(false, true, false);
}

TEST_F(AudioSourceTests, TracktionRenderKeepsTheAudacityReverbTail)
{
    std::vector<float> samples(48000, 0.0f);
    samples[2400] = 0.5f;
    const auto track = makeTrack({ samples, samples });
    const auto tracks = TrackList::Create(nullptr);
    tracks->Add(track);
    const auto state = addBuiltinEffect(RealtimeEffectList::Get(*track), wxT("tracktion-reverb"));
    ASSERT_TRUE(state);
    auto access = state->GetAccess();
    access->ModifySettings([](EffectSettings& settings) {
        auto& value = effects::ReverbEffect::GetSettings(settings);
        value.mWetOnly = true;
        value.mPreDelay = 0.0;
        return nullptr;
    });
    access->Flush();

    juce::AudioBuffer<float> rendered;
    ASSERT_NO_FATAL_FAILURE(renderTracks(*tracks, rendered, 48000));
    EXPECT_FLOAT_EQ(rendered.getSample(0, 2400), 0.0f);
    EXPECT_GT(rendered.getRMSLevel(0, 4800, 24000), 1e-5f);
    EXPECT_GT(rendered.getRMSLevel(1, 4800, 24000), 1e-5f);
}

TEST_F(AudioSourceTests, DestructiveAudioUnitKeepsClipsOutsideTheSelection)
{
#if AU_TRACKTION_TEST_AUDIO_UNIT
    const auto identifier = juce::String(qEnvironmentVariable("AU_TRACKTION_TEST_AUDIO_UNIT_IDENTIFIER").toStdString());
    auto path = wxString("appl/aufx/lpas/Apple: AULowpass");
    if (identifier.isNotEmpty()) {
        const auto fields = juce::StringArray::fromTokens(identifier.fromFirstOccurrenceOf(":", false, false), ",", "");
        ASSERT_EQ(fields.size(), 3);
        for (const auto& field : fields) {
            ASSERT_EQ(field.length(), 4);
        }
        path = wxString::FromUTF8((fields[2] + "/" + fields[0] + "/" + fields[1] + "/Test").toStdString());
    }
    const auto directory = m_projectFile.getSiblingFile("tracktion-selected-effect-" + juce::Uuid().toString());
    DEFER { directory.deleteRecursively();
    };
    const auto metadata = TracktionPluginMetaReader(PluginFormat::AudioUnit).readMeta(path.ToStdString());
    ASSERT_TRUE(metadata.ret);
    ASSERT_EQ(metadata.val.size(), 1u);
    const muse::audioplugins::AudioPluginInfoList plugins {
        { metadata.val.front(), muse::io::path_t(path.ToStdString()), muse::audioplugins::AudioPluginState::Validated }
    };
    auto known = std::make_shared<testing::NiceMock<muse::audioplugins::KnownAudioPluginsRegisterMock> >();
    ON_CALL(*known, pluginInfoList(testing::_)).WillByDefault(testing::Return(plugins));
    const auto runtime = std::make_shared<TracktionRuntime>();
    runtime->init(directory.getChildFile("cache").getFullPathName().toStdString(),
                  directory.getChildFile("settings").getFullPathName().toStdString());
    effects::AudioUnitEffectLoader loader(runtime);
    loader.knownPlugins.set(known);
    const auto id = muse::String::fromStdString(metadata.val.front().id);
    ASSERT_TRUE(loader.ensurePluginIsLoaded(id));
    auto& effect = *loader.effect(id);
    ASSERT_EQ(effect.GetType(), EffectTypeProcess);
    auto& project = *reinterpret_cast<au3::Au3Project*>(m_project->au3ProjectPtr());
    auto& tracks = TrackList::Get(project);
    const auto track = makeCaptureTrack(1);
    std::vector<std::shared_ptr<WaveClip> > clips;
    for (int index = 0; index < 3; ++index) {
        auto clip = makeClip({ std::vector<float>(4800, 0.1f * (index + 1)) });
        clip->SetSequenceStartTime(index * 0.1);
        track->InsertInterval(clip, true);
        clips.push_back(clip);
    }
    tracks.Add(track);
    track->SetSelected(true);
    effect.SetTracks(&tracks);
    DEFER { effect.SetTracks(nullptr);
    };
    effect.mFactory = &WaveTrackFactory::Get(project);
    effect.mProjectRate = 48000.0;
    effect.mT0 = 0.1;
    effect.mT1 = 0.2;
    effect.CountWaveTracks();
    auto settings = effect.MakeSettings();
    const auto instance = std::dynamic_pointer_cast<EffectInstanceEx>(effect.MakeProcessingInstance());
    ASSERT_TRUE(instance);
    ASSERT_TRUE(instance->Init());
    ASSERT_TRUE(instance->Process(settings));
    const auto result = *tracks.Any<WaveTrack>().begin();
    ASSERT_EQ(result->NIntervals(), 3u);
    for (const int index : { 0, 2 }) {
        const auto intervals = result->Intervals();
        const auto found = std::find_if(intervals.begin(), intervals.end(), [&](const auto& clip) {
            return clip->GetId() == clips[index]->GetId();
        });
        ASSERT_NE(found, intervals.end());
        EXPECT_DOUBLE_EQ((*found)->GetPlayStartTime(), index * 0.1);
        const AudacityAudioSource source(**found);
        std::vector<float> samples(4800);
        ASSERT_TRUE(source.read(0, samples.data(), 0, samples.size()));
        EXPECT_EQ(samples, std::vector<float>(4800, 0.1f * (index + 1)));
    }
#endif
}

TEST_F(AudioSourceTests, PhysicalRecordingStoresAudioThroughoutCapture)
{
    const auto host = qEnvironmentVariable("AU_TRACKTION_TEST_AUDIO_HOST").toStdString();
    const auto input = qEnvironmentVariable("AU_TRACKTION_TEST_INPUT_DEVICE").toStdString();
    if (host.empty() || input.empty()) {
        GTEST_SKIP() << "Set the physical audio host and input device to test recording.";
    }
    const auto previousBuffer = AudioIOLatencyDuration.Read();
    AudioIOLatencyDuration.Write(10.0);
    DEFER { AudioIOLatencyDuration.Write(previousBuffer);
    };
    RecordProperty("buffer_length_ms", AudioIOLatencyDuration.Read());
    const auto directory = m_projectFile.getSiblingFile("tracktion-physical-recording-" + juce::Uuid().toString());
    DEFER { directory.deleteRecursively();
    };
    auto configuration = std::make_shared<testing::NiceMock<muse::GlobalConfigurationMock> >();
    const muse::io::path_t path(directory.getFullPathName().toStdString());
    ON_CALL(*configuration, userAppDataPath()).WillByDefault(testing::Return(path));
    ON_CALL(*configuration, appConfigPath()).WillByDefault(testing::Return(path));
    auto driver = std::make_shared<testing::NiceMock<audio::AudioDriverControllerMock> >();
    audio::AudioConfiguration devices;
    devices.api = host;
    devices.inputDevice = input;
    devices.outputDevice = qEnvironmentVariable("AU_TRACKTION_TEST_OUTPUT_DEVICE").toStdString();
    ON_CALL(*driver, configuration()).WillByDefault(testing::Return(devices));
    auto plugins = std::make_shared<testing::NiceMock<muse::audioplugins::KnownAudioPluginsRegisterMock> >();
    auto recordConfiguration = std::make_shared<testing::NiceMock<record::RecordConfigurationMock> >();
    TracktionAudioEngine engine;
    engine.globalConfiguration.set(configuration);
    engine.knownPlugins.set(plugins);
    engine.audioDriverController.set(driver);
    engine.recordConfiguration.set(recordConfiguration);
    engine.init();
    auto& project = *reinterpret_cast<au3::Au3Project*>(m_project->au3ProjectPtr());
    auto& tracks = TrackList::Get(project);
    const auto backing = makeTrack({ std::vector<float>(96000, 0.125f) });
    (*backing->Intervals().begin())->SetSequenceStartTime(15.0);
    tracks.Add(backing);
    const auto captured = makeCaptureTrack(1, 44100.0);
    TransportSequences sequences;
    sequences.playbackSequences.push_back(StretchingSequence::Create(*backing, backing->GetClipInterfaces()));
    sequences.captureSequences.push_back(captured);
    audio::IAudioEngine::StartStreamOptions options;
    options.sampleRate = 44100.0;
    const auto recordStart = std::chrono::steady_clock::now();
    ASSERT_GT(engine.startStream(sequences, 2.0, std::numeric_limits<double>::max(),
                                 std::numeric_limits<double>::max(), project, options), 0) << engine.lastErrorString().toStdString();
    RecordProperty("record_start_ms", std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - recordStart).count());
    const auto start = std::chrono::steady_clock::now();
    while (std::chrono::steady_clock::now() - start < std::chrono::seconds(3)) {
        QCoreApplication::processEvents();
#if JUCE_MAC
        CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.001, true);
#endif
        juce::Timer::callPendingTimersSynchronously();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    juce::String files;
    for (const auto& file : directory.findChildFiles(juce::File::findFiles, true, "*.wav")) {
        files += file.getFileName() + ": " + juce::String(file.getSize()) + " bytes; ";
    }
    SCOPED_TRACE(files.toStdString());
    const double elapsed = engine.streamTime() - 2.0;
    EXPECT_GT(elapsed, 1.5);
    EXPECT_GT(captured->GetEndTime(), elapsed - 0.2);
    const auto recordStop = std::chrono::steady_clock::now();
    engine.stopStream();
    RecordProperty("record_stop_ms", std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - recordStop).count());
    EXPECT_TRUE(engine.lastErrorString().empty());
    EXPECT_NEAR(captured->GetEndTime(), elapsed, 0.1);
    EXPECT_FALSE(engine.isBusy());
    EXPECT_FALSE(engine.currentStream());
    EXPECT_FALSE(engine.consumeNextCallbackInfo());
    const double stoppedEnd = captured->GetEndTime();
    const auto idleDeadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(100);
    while (std::chrono::steady_clock::now() < idleDeadline) {
        QCoreApplication::processEvents();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    EXPECT_DOUBLE_EQ(captured->GetEndTime(), stoppedEnd);
    EXPECT_FALSE(engine.consumeNextCallbackInfo());
    const auto repeatedCapture = makeCaptureTrack(1, 44100.0);
    sequences.captureSequences = { repeatedCapture };
    const auto repeatedStart = std::chrono::steady_clock::now();
    ASSERT_GT(engine.startStream(sequences, 0.0, std::numeric_limits<double>::max(),
                                 std::numeric_limits<double>::max(), project, options), 0);
    RecordProperty("repeat_record_start_ms", std::chrono::duration<double, std::milli>(
                       std::chrono::steady_clock::now() - repeatedStart).count());
    const auto repeatDeadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(300);
    while (std::chrono::steady_clock::now() < repeatDeadline) {
        QCoreApplication::processEvents();
        juce::Timer::callPendingTimersSynchronously();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    const auto repeatedStop = std::chrono::steady_clock::now();
    engine.stopStream();
    RecordProperty("repeat_record_stop_ms", std::chrono::duration<double, std::milli>(
                       std::chrono::steady_clock::now() - repeatedStop).count());
    EXPECT_GT(repeatedCapture->GetEndTime(), 0.05);
    EXPECT_TRUE(engine.lastErrorString().empty());
    sequences.captureSequences.clear();
    const auto playStart = std::chrono::steady_clock::now();
    ASSERT_GT(engine.startStream(sequences, 2.0, 5.0, 5.0, project, options), 0);
    RecordProperty("playback_start_ms", std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - playStart).count());
    const auto playbackStart = std::chrono::steady_clock::now();
    while (std::chrono::steady_clock::now() - playbackStart < std::chrono::seconds(1)) {
        QCoreApplication::processEvents();
#if JUCE_MAC
        CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.001, true);
#endif
        juce::Timer::callPendingTimersSynchronously();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    EXPECT_TRUE(engine.isStreamActive(project));
    EXPECT_GT(engine.streamTime(), 2.5);
    const auto callback = engine.consumeNextCallbackInfo();
    ASSERT_TRUE(callback);
    const auto delay = std::chrono::duration<double>(std::chrono::steady_clock::now() - callback->dacTime).count();
    EXPECT_LT(std::abs(delay), 2.0);
    const auto playStop = std::chrono::steady_clock::now();
    engine.stopStream();
    RecordProperty("playback_stop_ms", std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - playStop).count());
}

TEST_F(AudioSourceTests, TracktionRecordingWritesAudacityProjectStorage)
{
    const auto directory = m_projectFile.getSiblingFile("tracktion-recording-" + juce::Uuid().toString());
    DEFER { directory.deleteRecursively();
    };
    TracktionRuntime runtime;
    runtime.init(directory.getChildFile("cache").getFullPathName().toStdString(),
                 directory.getChildFile("settings").getFullPathName().toStdString());
    HostedPlayback playback(runtime.engine(), 2);
    const auto tracks = TrackList::Create(nullptr);
    TracktionSession session(runtime, *tracks, TracktionSession::Role::Playback);
    const auto captured = makeCaptureTrack(2);
    TracktionRecording recording(session.edit(), { captured }, 2, directory.getFullPathName().toStdString(), {});
    const auto original = makeCaptureTrack(2);
    const auto visibleClip = original->CreateClip(0.0, "recording");
    const auto startError = recording.start(0.0);
    ASSERT_TRUE(startError.empty()) << startError;
    juce::AudioBuffer<float> input(2, 20 * 5120);
    for (int frame = 0; frame < input.getNumSamples(); ++frame) {
        input.setSample(0, frame, (frame % 100) / 200.0f);
        input.setSample(1, frame, -(frame % 100) / 200.0f);
    }
    for (int offset = 0; offset < input.getNumSamples(); offset += 5120) {
        const juce::AudioBuffer<float> chunk(input.getArrayOfWritePointers(), 2, offset, 5120);
        playback.processInput(chunk);
#if JUCE_MAC
        CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.001, true);
#endif
        ASSERT_TRUE(recording.update().empty());
        if (offset == 0) {
            visibleClip->LinkToOtherSource(**captured->Intervals().begin());
        }
        EXPECT_DOUBLE_EQ(visibleClip->GetPlayEndTime(), captured->GetEndTime());
    }
    ASSERT_EQ(captured->NIntervals(), 1u);
    EXPECT_GT(captured->GetEndTime(), 0.0);
    const auto error = recording.stop();
    ASSERT_TRUE(error.empty()) << error;
    EXPECT_DOUBLE_EQ(visibleClip->GetPlayEndTime(), captured->GetEndTime());
    ASSERT_EQ(captured->NIntervals(), 1u);
    const AudacityAudioSource source(**captured->Intervals().begin());
    ASSERT_EQ(source.sampleCount(), input.getNumSamples());
    std::vector<float> samples(input.getNumSamples());
    for (int channel = 0; channel < 2; ++channel) {
        ASSERT_TRUE(source.read(channel, samples.data(), 0, samples.size()));
        EXPECT_EQ(samples, (std::vector<float>(input.getReadPointer(channel), input.getReadPointer(channel) + input.getNumSamples())));
    }
}

TEST_F(AudioSourceTests, RecordingKeepsStoredSamplesWhenTheCaptureFileIsRemoved)
{
    const auto directory = m_projectFile.getSiblingFile("tracktion-removed-capture-" + juce::Uuid().toString());
    DEFER { directory.deleteRecursively();
    };
    TracktionRuntime runtime;
    runtime.init(directory.getChildFile("cache").getFullPathName().toStdString(),
                 directory.getChildFile("settings").getFullPathName().toStdString());
    HostedPlayback playback(runtime.engine(), 1);
    const auto tracks = TrackList::Create(nullptr);
    TracktionSession session(runtime, *tracks, TracktionSession::Role::Playback);
    const auto captured = makeCaptureTrack(1);
    TracktionRecording recording(session.edit(), { captured }, 1, directory.getFullPathName().toStdString(), {});
    ASSERT_TRUE(recording.start(0.0).empty());
    juce::AudioBuffer<float> input(1, 5120);
    juce::FloatVectorOperations::fill(input.getWritePointer(0), 0.25f, input.getNumSamples());
    playback.processInput(input);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    while (captured->GetEndTime() < 5120.0 / 48000.0 && std::chrono::steady_clock::now() < deadline) {
        ASSERT_TRUE(recording.update().empty());
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    ASSERT_DOUBLE_EQ(captured->GetEndTime(), 5120.0 / 48000.0);
    const auto files = directory.findChildFiles(juce::File::findFiles, true, "*.wav");
    ASSERT_EQ(files.size(), 1);
    ASSERT_TRUE(files[0].deleteFile());
    ASSERT_FALSE(files[0].existsAsFile());
    ASSERT_TRUE(recording.stop().empty());
    const auto copy = captured->Duplicate(Track::DuplicateOptions {}.Backup());
    const auto* stored = dynamic_cast<WaveTrack*>(copy.get());
    ASSERT_TRUE(stored);
    ASSERT_EQ(stored->NIntervals(), 1u);
    const AudacityAudioSource source(**stored->Intervals().begin());
    ASSERT_EQ(source.sampleCount(), input.getNumSamples());
    std::vector<float> samples(input.getNumSamples());
    ASSERT_TRUE(source.read(0, samples.data(), 0, samples.size()));
    EXPECT_EQ(samples, std::vector<float>(input.getNumSamples(), 0.25f));
}

TEST_F(AudioSourceTests, RecordingAppendFailuresKeepTheCaptureFileAndRepairPartialChannels)
{
    for (const bool updateBeforeStop : { true, false }) {
        SCOPED_TRACE(updateBeforeStop);
        const auto directory = m_projectFile.getSiblingFile("tracktion-recording-append-failure-" + juce::Uuid().toString());
        DEFER { directory.deleteRecursively();
        };
        TracktionRuntime runtime;
        runtime.init(directory.getChildFile("cache").getFullPathName().toStdString(),
                     directory.getChildFile("settings").getFullPathName().toStdString());
        HostedPlayback playback(runtime.engine(), 2);
        const auto tracks = TrackList::Create(nullptr);
        TracktionSession session(runtime, *tracks, TracktionSession::Role::Playback);
        const auto captured = makeCaptureTrack(2);
        const auto destination = std::make_shared<FailingRecordingDestination>(captured, FailingRecordingDestination::Failure::Append);
        {
            TracktionRecording recording(session.edit(), { destination }, 2, directory.getFullPathName().toStdString(), {});
            ASSERT_TRUE(recording.start(0.0).empty());
            juce::AudioBuffer<float> input(2, 5120);
            juce::FloatVectorOperations::fill(input.getWritePointer(0), 0.25f, input.getNumSamples());
            juce::FloatVectorOperations::fill(input.getWritePointer(1), 0.75f, input.getNumSamples());
            playback.processInput(input);
            if (updateBeforeStop) {
                std::string error;
                ASSERT_NO_THROW(error = recording.update());
                ASSERT_FALSE(error.empty());
                const int attempts = destination->appendCalls;
                EXPECT_EQ(recording.update(), error);
                EXPECT_EQ(destination->appendCalls, attempts);
            }
            std::string error;
            ASSERT_NO_THROW(error = recording.stop());
            EXPECT_FALSE(error.empty());
            EXPECT_FALSE(session.edit().getTransport().isRecording());
            EXPECT_EQ(destination->appendCalls, 2);
            EXPECT_EQ(destination->flushCalls, 1);
            EXPECT_EQ(destination->repairCalls, 1);
            const auto files = directory.findChildFiles(juce::File::findFiles, true, "*.wav");
            ASSERT_EQ(files.size(), 1);
            EXPECT_NE(error.find(files[0].getFullPathName().toStdString()), std::string::npos);
        }
        destination->waitForErrorHandler();
        EXPECT_EQ(*destination->handled, 1);
        ASSERT_EQ(captured->NIntervals(), 1u);
        const auto clip = *captured->Intervals().begin();
        const auto frames = clip->GetSequence(0)->GetNumSamples();
        EXPECT_GT(frames.as_long_long(), 0);
        EXPECT_EQ(clip->GetSequence(1)->GetNumSamples(), frames);
        const AudacityAudioSource source(*clip);
        std::vector<float> samples(source.sampleCount());
        ASSERT_TRUE(source.read(0, samples.data(), 0, samples.size()));
        EXPECT_EQ(samples, std::vector<float>(samples.size(), 0.25f));
        ASSERT_TRUE(source.read(1, samples.data(), 0, samples.size()));
        EXPECT_EQ(samples, std::vector<float>(samples.size(), 0.0f));
        const auto files = directory.findChildFiles(juce::File::findFiles, true, "*.wav");
        ASSERT_EQ(files.size(), 1);
        juce::WavAudioFormat format;
        const std::unique_ptr<juce::AudioFormatReader> reader(format.createReaderFor(files[0].createInputStream().release(), true));
        ASSERT_TRUE(reader);
        ASSERT_EQ(reader->lengthInSamples, 5120);
        juce::AudioBuffer<float> recovered(2, 5120);
        ASSERT_TRUE(reader->read(&recovered, 0, recovered.getNumSamples(), 0, true, true));
        EXPECT_FLOAT_EQ(recovered.getSample(0, 5119), 0.25f);
        EXPECT_FLOAT_EQ(recovered.getSample(1, 5119), 0.75f);
    }
}

TEST_F(AudioSourceTests, RecordingFlushFailuresKeepTheCaptureFileAndFinishChannelRepair)
{
    const auto directory = m_projectFile.getSiblingFile("tracktion-recording-flush-failure-" + juce::Uuid().toString());
    DEFER { directory.deleteRecursively();
    };
    TracktionRuntime runtime;
    runtime.init(directory.getChildFile("cache").getFullPathName().toStdString(),
                 directory.getChildFile("settings").getFullPathName().toStdString());
    HostedPlayback playback(runtime.engine(), 2);
    const auto tracks = TrackList::Create(nullptr);
    TracktionSession session(runtime, *tracks, TracktionSession::Role::Playback);
    const auto captured = makeCaptureTrack(2);
    const auto destination = std::make_shared<FailingRecordingDestination>(captured, FailingRecordingDestination::Failure::Flush);
    {
        TracktionRecording recording(session.edit(), { destination }, 2, directory.getFullPathName().toStdString(), {});
        ASSERT_TRUE(recording.start(0.0).empty());
        juce::AudioBuffer<float> input(2, 5120);
        juce::FloatVectorOperations::fill(input.getWritePointer(0), 0.25f, input.getNumSamples());
        juce::FloatVectorOperations::fill(input.getWritePointer(1), 0.75f, input.getNumSamples());
        playback.processInput(input);
        std::string error;
        ASSERT_NO_THROW(error = recording.stop());
        EXPECT_FALSE(error.empty());
        EXPECT_FALSE(session.edit().getTransport().isRecording());
        EXPECT_EQ(destination->flushCalls, 1);
        EXPECT_EQ(destination->repairCalls, 1);
        const auto files = directory.findChildFiles(juce::File::findFiles, true, "*.wav");
        ASSERT_EQ(files.size(), 1);
        EXPECT_NE(error.find(files[0].getFullPathName().toStdString()), std::string::npos);
    }
    destination->waitForErrorHandler();
    EXPECT_EQ(*destination->handled, 1);
    EXPECT_GT(captured->GetEndTime(), 0.0);
    EXPECT_EQ(directory.findChildFiles(juce::File::findFiles, true, "*.wav").size(), 1);
}

TEST_F(AudioSourceTests, TracktionRecordingExcludesPausedInput)
{
    const auto directory = m_projectFile.getSiblingFile("tracktion-recording-pause-" + juce::Uuid().toString());
    DEFER { directory.deleteRecursively();
    };
    TracktionRuntime runtime;
    runtime.init(directory.getChildFile("cache").getFullPathName().toStdString(),
                 directory.getChildFile("settings").getFullPathName().toStdString());
    HostedPlayback playback(runtime.engine(), 1);
    const auto tracks = TrackList::Create(nullptr);
    TracktionSession session(runtime, *tracks, TracktionSession::Role::Playback);
    const auto captured = makeCaptureTrack(1);
    TracktionRecording recording(session.edit(), { captured }, 1, directory.getFullPathName().toStdString(), {});
    ASSERT_TRUE(recording.start(0.0).empty());
    juce::AudioBuffer<float> input(1, 2560);
    auto& transport = session.edit().getTransport();
    transport.setPaused(true);
    juce::FloatVectorOperations::fill(input.getWritePointer(0), 0.75f, input.getNumSamples());
    playback.processInput(input);
    EXPECT_TRUE(transport.isRecording());
    transport.setPaused(false);
    juce::FloatVectorOperations::fill(input.getWritePointer(0), 0.125f, input.getNumSamples());
    playback.processInput(input);
    auto* context = transport.getCurrentPlaybackContext();
    transport.setPaused(true);
    juce::FloatVectorOperations::fill(input.getWritePointer(0), 0.75f, input.getNumSamples());
    playback.processInput(input);
    EXPECT_TRUE(transport.isRecording());
    EXPECT_EQ(context, transport.getCurrentPlaybackContext());
    transport.setPaused(false);
    juce::FloatVectorOperations::fill(input.getWritePointer(0), 0.25f, input.getNumSamples());
    playback.processInput(input);
    ASSERT_TRUE(recording.stop().empty());

    ASSERT_EQ(captured->NIntervals(), 1u);
    const AudacityAudioSource source(**captured->Intervals().begin());
    ASSERT_EQ(source.sampleCount(), 5120);
    std::vector<float> samples(5120);
    ASSERT_TRUE(source.read(0, samples.data(), 0, samples.size()));
    EXPECT_EQ(std::vector<float>(samples.begin(), samples.begin() + 2560), std::vector<float>(2560, 0.125f));
    EXPECT_EQ(std::vector<float>(samples.begin() + 2560, samples.end()), std::vector<float>(2560, 0.25f));
}

TEST_F(AudioSourceTests, TracktionRecordingMixesStereoInputIntoAMonoTrack)
{
    const auto directory = m_projectFile.getSiblingFile("tracktion-recording-mono-" + juce::Uuid().toString());
    DEFER { directory.deleteRecursively();
    };
    TracktionRuntime runtime;
    runtime.init(directory.getChildFile("cache").getFullPathName().toStdString(),
                 directory.getChildFile("settings").getFullPathName().toStdString());
    HostedPlayback playback(runtime.engine(), 2);
    const auto tracks = TrackList::Create(nullptr);
    TracktionSession session(runtime, *tracks, TracktionSession::Role::Playback);
    const auto captured = makeCaptureTrack(1);
    TracktionRecording recording(session.edit(), { captured }, 2, directory.getFullPathName().toStdString(), {});
    const auto startError = recording.start(0.0);
    ASSERT_TRUE(startError.empty()) << startError;
    juce::AudioBuffer<float> input(2, 5120);
    juce::FloatVectorOperations::fill(input.getWritePointer(0), 0.125f, input.getNumSamples());
    juce::FloatVectorOperations::fill(input.getWritePointer(1), 0.375f, input.getNumSamples());
    playback.processInput(input);
    const auto error = recording.stop();
    ASSERT_TRUE(error.empty()) << error;
    ASSERT_EQ(captured->NIntervals(), 1u);
    const AudacityAudioSource source(**captured->Intervals().begin());
    EXPECT_EQ(source.sampleCount(), input.getNumSamples());
    std::vector<float> samples(5120);
    ASSERT_TRUE(source.read(0, samples.data(), 0, samples.size()));
    EXPECT_EQ(samples, std::vector<float>(5120, 0.25f));
}

TEST_F(AudioSourceTests, TracktionRecordingResamplesIntoTheDocumentRate)
{
    const auto directory = m_projectFile.getSiblingFile("tracktion-recording-rate-" + juce::Uuid().toString());
    DEFER { directory.deleteRecursively();
    };
    TracktionRuntime runtime;
    runtime.init(directory.getChildFile("cache").getFullPathName().toStdString(),
                 directory.getChildFile("settings").getFullPathName().toStdString());
    HostedPlayback playback(runtime.engine(), 1);
    const auto tracks = TrackList::Create(nullptr);
    TracktionSession session(runtime, *tracks, TracktionSession::Role::Playback);
    const auto captured = makeCaptureTrack(1, 44100.0);
    TracktionRecording recording(session.edit(), { captured }, 1, directory.getFullPathName().toStdString(), {});
    const auto startError = recording.start(0.0);
    ASSERT_TRUE(startError.empty()) << startError;
    juce::AudioBuffer<float> input(1, 48000);
    for (int frame = 0; frame < input.getNumSamples(); ++frame) {
        input.setSample(0, frame, 0.25f * std::sin(2.0 * std::numbers::pi * 440.0 * frame / 48000.0));
    }
    for (int offset = 0; offset < input.getNumSamples(); offset += 4800) {
        juce::AudioBuffer<float> block(1, 4800);
        block.copyFrom(0, 0, input, 0, offset, block.getNumSamples());
        playback.processInput(block);
        ASSERT_TRUE(recording.update().empty());
    }
    EXPECT_GT(captured->GetEndTime(), 0.8);
    const auto error = recording.stop();
    ASSERT_TRUE(error.empty()) << error;
    ASSERT_EQ(captured->NIntervals(), 1u);
    const AudacityAudioSource source(**captured->Intervals().begin());
    ASSERT_EQ(source.sampleCount(), 44100);
    std::vector<float> samples(44100);
    ASSERT_TRUE(source.read(0, samples.data(), 0, samples.size()));
    int crossings = 0;
    for (int frame = 11026; frame < 33075; ++frame) {
        crossings += samples[frame - 1] <= 0.0f && samples[frame] > 0.0f;
    }
    EXPECT_NEAR(crossings, 220, 1);
    juce::MemoryAudioSource referenceSource(input, false, false);
    juce::ResamplingAudioSource referenceResampler(&referenceSource, false, 1);
    referenceResampler.setResamplingRatio(48000.0 / 44100.0);
    referenceResampler.prepareToPlay(4096, 44100.0);
    juce::AudioBuffer<float> reference(1, samples.size());
    for (int offset = 0; offset < reference.getNumSamples(); offset += 4096) {
        referenceResampler.getNextAudioBlock({ &reference, offset, std::min(4096, reference.getNumSamples() - offset) });
    }
    float maximumError = 0.0f;
    for (int frame = 0; frame < reference.getNumSamples(); ++frame) {
        maximumError = std::max(maximumError, std::abs(samples[frame] - reference.getSample(0, frame)));
    }
    EXPECT_LT(maximumError, 1e-5f);
}

TEST_F(AudioSourceTests, TracktionRecordingDuplicatesMonoInputIntoStereo)
{
    const auto captured = makeCaptureTrack(2);
    juce::AudioBuffer<float> input(1, 5120);
    juce::FloatVectorOperations::fill(input.getWritePointer(0), 0.25f, input.getNumSamples());
    ASSERT_NO_FATAL_FAILURE(recordInput(input, { captured }));
    ASSERT_EQ(captured->NIntervals(), 1u);
    const AudacityAudioSource source(**captured->Intervals().begin());
    ASSERT_EQ(source.sampleCount(), input.getNumSamples());
    std::vector<float> samples(5120);
    for (int channel = 0; channel < 2; ++channel) {
        ASSERT_TRUE(source.read(channel, samples.data(), 0, samples.size()));
        EXPECT_EQ(samples, std::vector<float>(5120, 0.25f));
    }
}

TEST_F(AudioSourceTests, TracktionRecordingAppliesInputGainToTheFirstSamples)
{
    juce::AudioBuffer<float> input(2, 5120);
    juce::FloatVectorOperations::fill(input.getWritePointer(0), 0.5f, input.getNumSamples());
    juce::FloatVectorOperations::fill(input.getWritePointer(1), -0.25f, input.getNumSamples());
    for (const float gain : { 0.0f, 0.5f, 1.0f }) {
        SCOPED_TRACE(gain);
        const auto captured = makeCaptureTrack(2);
        TracktionRecording::Options options;
        options.inputGain = gain;
        ASSERT_NO_FATAL_FAILURE(recordInput(input, { captured }, options));
        ASSERT_EQ(captured->NIntervals(), 1u);
        const AudacityAudioSource source(**captured->Intervals().begin());
        ASSERT_EQ(source.sampleCount(), input.getNumSamples());
        std::vector<float> samples(source.sampleCount());
        for (int channel = 0; channel < 2; ++channel) {
            ASSERT_TRUE(source.read(channel, samples.data(), 0, samples.size()));
            for (const auto sample : samples) {
                ASSERT_FLOAT_EQ(sample, input.getSample(channel, 0) * gain);
            }
        }
    }
}

TEST_F(AudioSourceTests, TracktionRecordingKeepsMultichannelInputsSeparate)
{
    const auto first = makeCaptureTrack(1);
    const auto second = makeCaptureTrack(1);
    const auto third = makeCaptureTrack(1);
    juce::AudioBuffer<float> input(3, 5120);
    for (int channel = 0; channel < 3; ++channel) {
        juce::FloatVectorOperations::fill(input.getWritePointer(channel), (channel + 1) * 0.125f, input.getNumSamples());
    }
    ASSERT_NO_FATAL_FAILURE(recordInput(input, { first, second, third }));
    int channel = 0;
    for (const auto& captured : { first, second, third }) {
        ASSERT_EQ(captured->NIntervals(), 1u);
        const AudacityAudioSource source(**captured->Intervals().begin());
        ASSERT_EQ(source.sampleCount(), input.getNumSamples());
        std::vector<float> samples(5120);
        ASSERT_TRUE(source.read(0, samples.data(), 0, samples.size()));
        EXPECT_EQ(samples, std::vector<float>(5120, (++channel) * 0.125f));
    }
}

TEST_F(AudioSourceTests, TracktionRecordingAppliesManualLatencyCorrection)
{
    juce::AudioBuffer<float> input(1, 5120);
    for (int frame = 0; frame < input.getNumSamples(); ++frame) {
        input.setSample(0, frame, (frame % 100) / 200.0f);
    }
    const auto captured = makeCaptureTrack(1);
    TracktionRecording::Options options;
    options.automaticLatencyCompensation = false;
    options.latencyCompensationMs = -10.0;
    ASSERT_NO_FATAL_FAILURE(recordInput(input, { captured }, options));
    ASSERT_EQ(captured->NIntervals(), 1u);
    const AudacityAudioSource source(**captured->Intervals().begin());
    ASSERT_EQ(source.sampleCount(), input.getNumSamples() - 480);
    std::vector<float> samples(source.sampleCount());
    ASSERT_TRUE(source.read(0, samples.data(), 0, samples.size()));
    EXPECT_EQ(samples, (std::vector<float>(input.getReadPointer(0) + 480, input.getReadPointer(0) + input.getNumSamples())));
}

TEST_F(AudioSourceTests, TracktionRecordingKeepsManualCorrectionIndependentOfDeviceLatency)
{
    juce::AudioBuffer<float> input(2, 48000);
    for (int frame = 0; frame < input.getNumSamples(); ++frame) {
        input.setSample(0, frame, (frame % 101) / 202.0f);
        input.setSample(1, frame, input.getSample(0, frame));
    }
    for (const auto setting : { std::pair { -10.0, 480 }, { 0.0, 480 }, { 10.0, 480 },
                                { -10.0, 28800 }, { 0.0, 28800 }, { 10.0, 28800 } }) {
        const auto [correction, deviceLatency] = setting;
        SCOPED_TRACE(correction);
        SCOPED_TRACE(deviceLatency);
        const auto captured = makeCaptureTrack(1);
        TracktionRecording::Options options;
        options.automaticLatencyCompensation = false;
        options.latencyCompensationMs = correction;
        ASSERT_NO_FATAL_FAILURE(recordInput(input, { captured }, options, deviceLatency));
        ASSERT_EQ(captured->NIntervals(), 1u);
        const AudacityAudioSource source(**captured->Intervals().begin());
        const int adjustment = static_cast<int>(correction * 48.0);
        ASSERT_EQ(source.sampleCount(), input.getNumSamples() + adjustment);
        std::vector<float> actual(source.sampleCount());
        ASSERT_TRUE(source.read(0, actual.data(), 0, actual.size()));
        for (int frame = 0; frame < source.sampleCount(); ++frame) {
            const int inputFrame = frame - adjustment - deviceLatency;
            const float expected = inputFrame < 0 ? 0.0f : input.getSample(0, inputFrame);
            ASSERT_FLOAT_EQ(actual[frame], expected) << frame;
        }
    }
}

TEST_F(AudioSourceTests, TracktionRecordingAppliesLargeNegativeLatencyCorrection)
{
    juce::AudioBuffer<float> input(1, 48000);
    for (int frame = 0; frame < input.getNumSamples(); ++frame) {
        input.setSample(0, frame, (frame % 101) / 202.0f);
    }
    const auto captured = makeCaptureTrack(1);
    TracktionRecording::Options options;
    options.automaticLatencyCompensation = false;
    options.latencyCompensationMs = -750.02;
    ASSERT_NO_FATAL_FAILURE(recordInput(input, { captured }, options));
    ASSERT_EQ(captured->NIntervals(), 1u);
    const AudacityAudioSource source(**captured->Intervals().begin());
    ASSERT_EQ(source.sampleCount(), 12000);
    std::vector<float> actual(source.sampleCount());
    ASSERT_TRUE(source.read(0, actual.data(), 0, actual.size()));
    EXPECT_EQ(actual, (std::vector<float>(input.getReadPointer(0) + 36000, input.getReadPointer(0) + input.getNumSamples())));
}

TEST_F(AudioSourceTests, TracktionRecordingPadsLargePositiveLatencyCorrection)
{
    for (const double correction : { 750.0, 750.02, 3500.0 }) {
        SCOPED_TRACE(correction);
        const auto captured = makeCaptureTrack(1);
        juce::AudioBuffer<float> input(1, 5120);
        juce::FloatVectorOperations::fill(input.getWritePointer(0), 0.25f, input.getNumSamples());
        TracktionRecording::Options options;
        options.automaticLatencyCompensation = false;
        options.latencyCompensationMs = correction;
        ASSERT_NO_FATAL_FAILURE(recordInput(input, { captured }, options));
        ASSERT_EQ(captured->NIntervals(), 1u);
        const AudacityAudioSource source(**captured->Intervals().begin());
        const int padding = static_cast<int>(correction * 48.0);
        ASSERT_EQ(source.sampleCount(), input.getNumSamples() + padding);
        std::vector<float> actual(source.sampleCount());
        ASSERT_TRUE(source.read(0, actual.data(), 0, actual.size()));
        for (int frame = 0; frame < padding; ++frame) {
            ASSERT_FLOAT_EQ(actual[frame], 0.0f) << frame;
        }
        for (size_t frame = padding; frame < actual.size(); ++frame) {
            ASSERT_FLOAT_EQ(actual[frame], 0.25f) << frame;
        }
    }
}

TEST_F(AudioSourceTests, TracktionRecordingAppliesDeviceLatencyCorrection)
{
    const auto captured = makeCaptureTrack(1);
    juce::AudioBuffer<float> input(2, 5600);
    input.clear();
    for (int frame = 0; frame < 5120; ++frame) {
        for (int channel = 0; channel < input.getNumChannels(); ++channel) {
            input.setSample(channel, frame, (frame % 100) / 200.0f);
        }
    }
    ASSERT_NO_FATAL_FAILURE(recordInput(input, { captured }, {}, 480));
    ASSERT_EQ(captured->NIntervals(), 1u);
    const AudacityAudioSource source(**captured->Intervals().begin());
    ASSERT_EQ(source.sampleCount(), 5120);
    std::vector<float> samples(5120);
    ASSERT_TRUE(source.read(0, samples.data(), 0, samples.size()));
    EXPECT_EQ(samples, (std::vector<float>(input.getReadPointer(0), input.getReadPointer(0) + 5120)));
}

TEST_F(AudioSourceTests, TracktionRecordingKeepsExistingClipsAndPendingClipPosition)
{
    const auto captured = makeTrack({ std::vector<float>(4800, 0.5f) });
    const auto original = *captured->Intervals().begin();
    const auto pending = captured->CreateClip(2.0, "Recording");
    pending->SetIsPlaceholder(true);
    captured->InsertInterval(pending, true);
    const auto pendingId = pending->GetId();
    juce::AudioBuffer<float> input(1, 5120);
    juce::FloatVectorOperations::fill(input.getWritePointer(0), 0.25f, input.getNumSamples());
    ASSERT_NO_FATAL_FAILURE(recordInput(input, { captured }, {}, 0, 2.0));
    ASSERT_EQ(captured->NIntervals(), 2u);
    EXPECT_EQ(pending->GetId(), pendingId);
    EXPECT_DOUBLE_EQ(pending->GetPlayStartTime(), 2.0);
    EXPECT_NEAR(pending->GetPlayEndTime(), 2.0 + 5120.0 / 48000.0, 1e-10);
    const AudacityAudioSource previous(*original);
    const AudacityAudioSource recorded(*pending);
    std::vector<float> samples(4800);
    ASSERT_TRUE(previous.read(0, samples.data(), 0, samples.size()));
    EXPECT_EQ(samples, std::vector<float>(4800, 0.5f));
    ASSERT_EQ(recorded.sampleCount(), 5120);
    samples.resize(5120);
    ASSERT_TRUE(recorded.read(0, samples.data(), 0, samples.size()));
    EXPECT_EQ(samples, std::vector<float>(5120, 0.25f));
}

TEST_F(AudioSourceTests, TracktionRecordingPadsPositiveLatencyCorrection)
{
    const auto captured = makeCaptureTrack(1);
    juce::AudioBuffer<float> input(1, 5120);
    juce::FloatVectorOperations::fill(input.getWritePointer(0), 0.25f, input.getNumSamples());
    TracktionRecording::Options options;
    options.automaticLatencyCompensation = false;
    options.latencyCompensationMs = 10.0;
    ASSERT_NO_FATAL_FAILURE(recordInput(input, { captured }, options));
    ASSERT_EQ(captured->NIntervals(), 1u);
    const AudacityAudioSource source(**captured->Intervals().begin());
    ASSERT_EQ(source.sampleCount(), 5600);
    std::vector<float> samples(5600);
    ASSERT_TRUE(source.read(0, samples.data(), 0, samples.size()));
    EXPECT_EQ((std::vector<float>(samples.begin(), samples.begin() + 480)), std::vector<float>(480, 0.0f));
    EXPECT_EQ((std::vector<float>(samples.begin() + 480, samples.end())), std::vector<float>(5120, 0.25f));
}

TEST_F(AudioSourceTests, TracktionRecordingDiscardsLeadInAndKeepsThePendingClip)
{
    const auto captured = makeCaptureTrack(1);
    const auto pending = captured->CreateClip(2.0, "Recording");
    pending->SetIsPlaceholder(true);
    captured->InsertInterval(pending, true);
    const auto id = pending->GetId();
    juce::AudioBuffer<float> input(1, 6200);
    for (int frame = 0; frame < input.getNumSamples(); ++frame) {
        input.setSample(0, frame, frame < 1200 ? 0.75f : (frame % 100) / 200.0f);
    }
    TracktionRecording::Options options;
    options.leadInTime = 0.025;
    ASSERT_NO_FATAL_FAILURE(recordInput(input, { captured }, options, 0, 2.0));
    ASSERT_EQ(captured->NIntervals(), 1u);
    EXPECT_EQ(pending->GetId(), id);
    EXPECT_DOUBLE_EQ(pending->GetPlayStartTime(), 2.0);
    const AudacityAudioSource source(*pending);
    ASSERT_EQ(source.sampleCount(), 5000);
    std::vector<float> samples(5000);
    ASSERT_TRUE(source.read(0, samples.data(), 0, samples.size()));
    EXPECT_EQ(samples, (std::vector<float>(input.getReadPointer(0) + 1200, input.getReadPointer(0) + 6200)));
}

TEST_F(AudioSourceTests, TracktionRecordingStoresOnlyTheRequestedDuration)
{
    const auto captured = makeCaptureTrack(1);
    juce::AudioBuffer<float> input(1, 5120);
    for (int frame = 0; frame < input.getNumSamples(); ++frame) {
        input.setSample(0, frame, (frame % 100) / 200.0f);
    }
    TracktionRecording::Options options;
    options.duration = 1003.0 / 48000.0;
    ASSERT_NO_FATAL_FAILURE(recordInput(input, { captured }, options));
    ASSERT_EQ(captured->NIntervals(), 1u);
    const AudacityAudioSource source(**captured->Intervals().begin());
    ASSERT_EQ(source.sampleCount(), 1003);
    std::vector<float> samples(1003);
    ASSERT_TRUE(source.read(0, samples.data(), 0, samples.size()));
    EXPECT_EQ(samples, (std::vector<float>(input.getReadPointer(0), input.getReadPointer(0) + 1003)));
}

TEST_F(AudioSourceTests, TracktionRecordingReportsWhenTheRequestedSamplesAreStored)
{
    const auto directory = m_projectFile.getSiblingFile("tracktion-recording-end-" + juce::Uuid().toString());
    DEFER { directory.deleteRecursively();
    };
    TracktionRuntime runtime;
    runtime.init(directory.getChildFile("cache").getFullPathName().toStdString(),
                 directory.getChildFile("settings").getFullPathName().toStdString());
    HostedPlayback playback(runtime.engine(), 1);
    const auto tracks = TrackList::Create(nullptr);
    TracktionSession session(runtime, *tracks, TracktionSession::Role::Playback);
    const auto captured = makeCaptureTrack(1);
    TracktionRecording::Options options;
    options.duration = 1003.0 / 48000.0;
    TracktionRecording recording(session.edit(), { captured }, 1, directory.getFullPathName().toStdString(), options);
    ASSERT_TRUE(recording.start(0.0).empty());
    ASSERT_TRUE(recording.update().empty());
    EXPECT_FALSE(recording.hasReachedEnd());
    juce::AudioBuffer<float> input(1, 5120);
    juce::FloatVectorOperations::fill(input.getWritePointer(0), 0.25f, input.getNumSamples());
    playback.processInput(input);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    while (!recording.hasReachedEnd() && std::chrono::steady_clock::now() < deadline) {
        ASSERT_TRUE(recording.update().empty());
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    EXPECT_TRUE(recording.hasReachedEnd());
    EXPECT_NEAR(captured->GetEndTime(), 1003.0 / 48000.0, 1e-10);
    ASSERT_TRUE(recording.stop().empty());
    const AudacityAudioSource source(**captured->Intervals().begin());
    EXPECT_EQ(source.sampleCount(), 1003);
}

TEST_F(AudioSourceTests, TracktionPlaybackStopsLeadInTracksAtTheRecordingStart)
{
    const auto directory = m_projectFile.getSiblingFile("tracktion-recording-lead-in-" + juce::Uuid().toString());
    DEFER { directory.deleteRecursively();
    };
    TracktionRuntime runtime;
    runtime.init(directory.getChildFile("cache").getFullPathName().toStdString(),
                 directory.getChildFile("settings").getFullPathName().toStdString());
    HostedPlayback playback(runtime.engine());
    const auto tracks = TrackList::Create(nullptr);
    const auto leadIn = makeTrack({ std::vector<float>(48000, 0.25f) });
    const auto backing = makeTrack({ std::vector<float>(48000, 0.125f) });
    tracks->Add(leadIn);
    tracks->Add(backing);
    TransportSequences sequences;
    sequences.playbackSequences.push_back(StretchingSequence::Create(*leadIn, leadIn->GetClipInterfaces()));
    sequences.playbackSequences.push_back(StretchingSequence::Create(*backing, backing->GetClipInterfaces()));
    sequences.leadInTimeSequences.push_back(leadIn);
    TracktionSession session(runtime, *tracks, sequences, TracktionSession::Role::Playback);
    session.setLeadInEnd(sequences, 0.05);
    auto& transport = session.edit().getTransport();
    transport.play(false);
    const auto audio = playback.process(24);
    EXPECT_NEAR(audio.getSample(0, 2000), 0.375f, 1e-5f);
    EXPECT_NEAR(audio.getSample(0, 2600), 0.125f, 1e-5f);
    transport.stop(false, true, false);
    EXPECT_DOUBLE_EQ(leadIn->GetEndTime(), 1.0);
}

TEST_F(AudioSourceTests, TracktionRecordingCanStopDuringLeadInWithoutAddingSamples)
{
    const auto captured = makeCaptureTrack(1);
    juce::AudioBuffer<float> input(1, 1024);
    juce::FloatVectorOperations::fill(input.getWritePointer(0), 0.75f, input.getNumSamples());
    TracktionRecording::Options options;
    options.leadInTime = 0.025;
    ASSERT_NO_FATAL_FAILURE(recordInput(input, { captured }, options, 0, 2.0));
    EXPECT_EQ(captured->NIntervals(), 0u);
}

TEST_F(AudioSourceTests, TracktionRecordingResamplesAfterLeadIn)
{
    const auto captured = makeCaptureTrack(1, 44100.0);
    juce::AudioBuffer<float> input(1, 7200);
    for (int frame = 0; frame < input.getNumSamples(); ++frame) {
        input.setSample(0, frame, frame < 2400 ? 0.75f : 0.25f * std::sin(2.0 * std::numbers::pi * 440.0 * frame / 48000.0));
    }
    TracktionRecording::Options options;
    options.leadInTime = 0.05;
    options.duration = 0.075;
    ASSERT_NO_FATAL_FAILURE(recordInput(input, { captured }, options, 0, 2.0));
    ASSERT_EQ(captured->NIntervals(), 1u);
    const AudacityAudioSource source(**captured->Intervals().begin());
    ASSERT_EQ(source.sampleCount(), std::llround(0.075 * 44100.0));
    std::vector<float> samples(source.sampleCount());
    ASSERT_TRUE(source.read(0, samples.data(), 0, samples.size()));
    juce::AudioBuffer<float> referenceInput(1, 4800);
    referenceInput.copyFrom(0, 0, input, 0, 2400, 4800);
    juce::MemoryAudioSource referenceSource(referenceInput, false, false);
    juce::ResamplingAudioSource referenceResampler(&referenceSource, false, 1);
    referenceResampler.setResamplingRatio(48000.0 / 44100.0);
    referenceResampler.prepareToPlay(4096, 44100.0);
    juce::AudioBuffer<float> reference(1, samples.size());
    referenceResampler.getNextAudioBlock({ &reference, 0, reference.getNumSamples() });
    for (size_t frame = 0; frame < samples.size(); ++frame) {
        ASSERT_NEAR(samples[frame], reference.getSample(0, frame), 1e-5f) << frame;
    }
}

TEST_F(AudioSourceTests, TracktionRecordingCrossfadesEachDestinationChannel)
{
    const auto left = makeCaptureTrack(1);
    const auto right = makeCaptureTrack(1);
    juce::AudioBuffer<float> input(2, 7200);
    juce::FloatVectorOperations::fill(input.getWritePointer(0), 0.25f, input.getNumSamples());
    juce::FloatVectorOperations::fill(input.getWritePointer(1), 0.75f, input.getNumSamples());
    TracktionRecording::Options options;
    options.crossfadeData = { std::vector<float>(5000, 1.0f), std::vector<float>(6000, -1.0f) };
    ASSERT_NO_FATAL_FAILURE(recordInput(input, { left, right }, options));
    const std::shared_ptr<WaveTrack> tracks[] { left, right };
    for (int channel = 0; channel < 2; ++channel) {
        ASSERT_EQ(tracks[channel]->NIntervals(), 1u);
        const AudacityAudioSource source(**tracks[channel]->Intervals().begin());
        ASSERT_EQ(source.sampleCount(), input.getNumSamples());
        std::vector<float> samples(source.sampleCount());
        ASSERT_TRUE(source.read(0, samples.data(), 0, samples.size()));
        const auto& previous = options.crossfadeData[channel];
        for (size_t frame = 0; frame < samples.size(); ++frame) {
            const double ratio = std::min(1.0, static_cast<double>(frame) / previous.size());
            const float expected = ratio * input.getSample(channel, frame) + (1.0 - ratio) * previous.front();
            ASSERT_NEAR(samples[frame], expected, 1e-6f) << channel << ": " << frame;
        }
    }
}

TEST_F(AudioSourceTests, TracktionRecordingCrossfadesDuplicatedMonoInputWithoutChangingTheOtherChannel)
{
    const auto captured = makeCaptureTrack(2);
    juce::AudioBuffer<float> input(1, 5120);
    juce::FloatVectorOperations::fill(input.getWritePointer(0), 0.25f, input.getNumSamples());
    TracktionRecording::Options options;
    options.crossfadeData = { std::vector<float>(1000, 0.5f), std::vector<float>(1000, -0.5f) };
    ASSERT_NO_FATAL_FAILURE(recordInput(input, { captured }, options));
    const AudacityAudioSource source(**captured->Intervals().begin());
    for (int channel = 0; channel < 2; ++channel) {
        std::vector<float> samples(source.sampleCount());
        ASSERT_TRUE(source.read(channel, samples.data(), 0, samples.size()));
        const auto& previous = options.crossfadeData[channel];
        for (size_t frame = 0; frame < samples.size(); ++frame) {
            const double ratio = std::min(1.0, static_cast<double>(frame) / previous.size());
            const float expected = ratio * 0.25f + (1.0 - ratio) * previous.front();
            ASSERT_NEAR(samples[frame], expected, 1e-6f) << channel << ": " << frame;
        }
    }
}

TEST_F(AudioSourceTests, TracktionRecordingKeepsLatencyPaddingOutsideTheCrossfade)
{
    const auto captured = makeCaptureTrack(1);
    juce::AudioBuffer<float> input(1, 5120);
    juce::FloatVectorOperations::fill(input.getWritePointer(0), 0.25f, input.getNumSamples());
    TracktionRecording::Options options;
    options.automaticLatencyCompensation = false;
    options.latencyCompensationMs = 10.0;
    options.crossfadeData = { std::vector<float>(1000, 1.0f) };
    ASSERT_NO_FATAL_FAILURE(recordInput(input, { captured }, options));
    const AudacityAudioSource source(**captured->Intervals().begin());
    ASSERT_EQ(source.sampleCount(), 5600);
    std::vector<float> samples(source.sampleCount());
    ASSERT_TRUE(source.read(0, samples.data(), 0, samples.size()));
    for (size_t frame = 0; frame < samples.size(); ++frame) {
        const double ratio = std::min(1.0, frame / 1000.0);
        const float expected = frame < 480 ? 0.0f : ratio * 0.25f + (1.0 - ratio);
        ASSERT_NEAR(samples[frame], expected, 1e-6f) << frame;
    }
}

TEST_F(AudioSourceTests, TracktionRecordingDoesNotRequireAnOutputDevice)
{
    const auto captured = makeCaptureTrack(1);
    juce::AudioBuffer<float> input(1, 5120);
    for (int frame = 0; frame < input.getNumSamples(); ++frame) {
        input.setSample(0, frame, (frame % 100) / 200.0f);
    }
    ASSERT_NO_FATAL_FAILURE(recordInput(input, { captured }, {}, 0, 0.0, 0));
    ASSERT_EQ(captured->NIntervals(), 1u);
    const AudacityAudioSource source(**captured->Intervals().begin());
    ASSERT_EQ(source.sampleCount(), input.getNumSamples());
    std::vector<float> samples(source.sampleCount());
    ASSERT_TRUE(source.read(0, samples.data(), 0, samples.size()));
    EXPECT_EQ(samples, (std::vector<float>(input.getReadPointer(0), input.getReadPointer(0) + input.getNumSamples())));
}
}
