/*
* Audacity: A Digital Audio Editor
*/
#include <tracktion_engine/tracktion_engine.h>

#include "tracktionaudioengine.h"

#include <QTimer>

#include "au3-audio-io/ProjectAudioIO.h"
#include "au3-audio-io/AudioIO.h"
#include "au3-mixer/AudioRenderSource.h"
#include "au3-project/Project.h"
#include "au3-project-rate/ProjectRate.h"
#include "au3-realtime-effects/RealtimeEffectList.h"
#include "au3-realtime-effects/RealtimeEffectState.h"
#include "au3-time-frequency-selection/ViewInfo.h"
#include "au3-wave-track/WaveTrack.h"
#include "au3wrap/internal/wxtypes_convert.h"

#include "portaudiodevice.h"
#include "tracktionaudiocallbacks.h"
#include "tracktionaudioio.h"
#include "tracktionmixsource.h"
#include "tracktioneffectinstance.h"
#include "tracktionplugindescriptions.h"
#include "global/async/asyncable.h"
#include "global/log.h"
#include "au3-effects/EffectPlugin.h"
#include "tracktionrecording.h"
#include "tracktionruntime.h"
#include "tracktionsession.h"
#include "tracktioninstrument.h"
#include "au3-midi-track/MidiTrack.h"

using namespace au::tracktionaudio;

namespace {
const ProjectAudioIO::DefaultOptions::Scope defaultOptionsScope {
    [](AudacityProject& project, bool newDefault) {
        auto options = ProjectAudioIO::DefaultOptionsFactory(project, newDefault);
        const auto& region = ViewInfo::Get(project).playRegion;
        options.loopEnabled = region.Active();
        if (newDefault) {
            options.pStartTime = std::max(0.0, region.GetStart());
        }
        return options;
    }
};
}

struct TracktionAudioEngine::Data : muse::async::Asyncable
{
    enum class State {
        Idle, Starting, Running, Stopping
    };

    TracktionRuntime runtime;
    std::optional<AudioRenderSource::Factory::Scope> renderFactory;
    std::optional<EffectPlugin::ProcessingInstanceFactory::Scope> effectFactory;
    std::optional<EffectInstanceFactory::EditorInstanceFactory::Scope> editorFactory;
    std::shared_ptr<TracktionAudioCallbacks> callbacks = std::make_shared<TracktionAudioCallbacks>();
    std::unique_ptr<TracktionSession> session;
    std::unique_ptr<TracktionRecording> recording;
    std::weak_ptr<audio::IInstrumentEditor> instrumentEditor;
    std::weak_ptr<AudacityProject> owner;
    std::optional<audio::AudioStreamDescriptor> stream;
    std::unique_ptr<juce::XmlElement> previousInputLayout;
    std::optional<tracktion::InputDevice::MonitorMode> previousMonitorMode;
    juce::String monitoringInputID;
    Observer::Subscription regionSubscription;
    Observer::Subscription speedSubscription;
    QTimer timer;
    std::string error;
    bool initialised = false;
    State state = State::Idle;
    bool defaultPolicy = false;
    int nextToken = 0;
    double rate = 44100.0;
    double endTime = 0.0;
    double trackEndTime = 0.0;
    double recordingStartTime = 0.0;
    std::chrono::steady_clock::time_point lastRecordingUpdate;
    muse::async::Notification recordingUpdateRequested;
    muse::async::Notification recordingCommitRequested;
    muse::async::Notification recordingFinished;
    muse::async::Notification streamStopped;
    muse::async::Channel<au3::Au3TrackId, au3::Au3ClipId> recordingClipChanged;

    bool hasReachedEnd() const
    {
        if (recording && !recording->hasReachedEnd()) {
            return false;
        }
        const auto end = callbacks->playbackEndTime();
        if (end) {
            return std::chrono::steady_clock::now() >= *end;
        }
        const auto* device = runtime.engine().getDeviceManager().deviceManager.getCurrentAudioDevice();
        return recording && device && device->getActiveOutputChannels().isZero();
    }

    void updateLoop(AudacityProject& project,
                    TracktionSession::PlaybackLoopUpdate update = TracktionSession::PlaybackLoopUpdate::RegionChanged)
    {
        const juce::ScopedLock lock(runtime.engine().getDeviceManager().deviceManager.getAudioCallbackLock());
        auto& transport = session->edit().getTransport();
        auto& region = ViewInfo::Get(project).playRegion;
        const bool wasLooping = transport.looping;
        const bool looping = session->updatePlaybackLoop(defaultPolicy && region.Active(), region.GetStart(), region.GetEnd(), update);
        if (wasLooping && !looping) {
            endTime = trackEndTime;
        }
        session->setPlaybackEnd(looping ? std::nullopt : std::make_optional(endTime));
        callbacks->setPlaybackControls(session->playbackControls());
        callbacks->setTrackMeters(session->trackMeters());
    }

    std::string connectMonitoringInput(int channels, bool audible)
    {
        auto& manager = runtime.engine().getDeviceManager();
        previousInputLayout = std::make_unique<juce::XmlElement>(manager.getCurrentWaveDeviceLayout());
        manager.setAllWaveInputsToNumChannels(channels);
        manager.dispatchPendingUpdates();
        for (auto* input : manager.getWaveInputDevices()) {
            if (input->getChannels().getNumChannels() == channels
                && input->deviceDescription.getDeviceChannelRange().first == 0) {
                monitoringInputID = input->getDeviceID();
                manager.setDeviceEnabled(*input, true);
                break;
            }
        }
        manager.dispatchPendingUpdates();
        for (auto* input : manager.getWaveInputDevices()) {
            if (input->getDeviceID() != monitoringInputID) {
                continue;
            }
            previousMonitorMode = input->getMonitorMode();
            input->setMonitorMode(audible ? tracktion::InputDevice::MonitorMode::on : tracktion::InputDevice::MonitorMode::off);
            TracktionRuntime::setInputGain(*input, TracktionAudioIO::instance().softwareInputGain());
            auto& edit = session->edit();
            edit.playInStopEnabled = true;
            auto target = edit.insertNewAudioTrack(tracktion::TrackInsertPoint::getEndOfTracks(edit), nullptr);
            if (!target) {
                return "Cannot create an input monitoring track.";
            }
            target->getVolumePlugin()->setVolumeDb(0.0f);
            auto& transport = edit.getTransport();
            transport.ensureContextAllocated();
            auto* context = transport.getCurrentPlaybackContext();
            auto* instance = context ? context->getInputFor(input) : nullptr;
            if (!instance) {
                return "Cannot connect the monitoring input.";
            }
            const auto assignment = instance->setTarget(target->itemID, true, nullptr);
            if (!assignment) {
                return assignment.error().toStdString();
            }
            (*assignment)->recordEnabled = false;
            instance->dispatchPendingRecordingUpdates();
            edit.dispatchPendingUpdatesSynchronously();
            return {};
        }
        return "The audio input is not available.";
    }

    void restoreMonitoringInput()
    {
        auto& manager = runtime.engine().getDeviceManager();
        if (previousMonitorMode) {
            for (auto* input : manager.getWaveInputDevices()) {
                if (input->getDeviceID() == monitoringInputID) {
                    input->setMonitorMode(*previousMonitorMode);
                    break;
                }
            }
        }
        if (previousInputLayout) {
            manager.applyWaveDeviceLayout(*previousInputLayout);
            manager.dispatchPendingUpdates();
        }
        previousInputLayout.reset();
        previousMonitorMode.reset();
        monitoringInputID.clear();
    }
};

TracktionAudioEngine::TracktionAudioEngine()
    : m_data(std::make_unique<Data>())
{
    m_data->timer.setInterval(20);
    QObject::connect(&m_data->timer, &QTimer::timeout, &m_data->timer, [this] { updateStream(); });
}

TracktionAudioEngine::~TracktionAudioEngine()
{
    deinit();
}

void TracktionAudioEngine::init()
{
    m_data->error = TracktionAudioIO::init(*this);
    const auto cache = globalConfiguration()->userAppDataPath() + "/tracktion/cache";
    const auto settings = globalConfiguration()->appConfigPath() + "/tracktion";
    m_data->runtime.init(cache.toStdString(), settings.toStdString());
    restoreTracktionPluginDescriptions(m_data->runtime.engine(), knownPlugins()->pluginInfoList());
    knownPlugins()->pluginInfoListChanged().onNotify(m_data.get(), [this] {
        restoreTracktionPluginDescriptions(m_data->runtime.engine(), knownPlugins()->pluginInfoList());
    });
    m_data->renderFactory.emplace([this](const AudacityProject& project, const AudioRenderSource::Tracks& tracks,
                                         const AudioRenderSource::Options& options) {
        return std::make_unique<TracktionMixSource>(m_data->runtime, project, tracks, options);
    });
    m_data->effectFactory.emplace([this](const EffectPlugin& effect) {
        return createTracktionEffectInstance(m_data->runtime, effect);
    });
    m_data->editorFactory.emplace([this](const EffectInstanceFactory& effect) {
        return createTracktionEffectInstance(m_data->runtime, effect);
    });
    m_data->initialised = true;
}

void TracktionAudioEngine::deinit()
{
    if (!m_data->initialised) {
        return;
    }
    stopStream();
    m_data->timer.stop();
    m_data->runtime.closeAudioDevice();
    m_data->owner.reset();
    m_data->renderFactory.reset();
    m_data->effectFactory.reset();
    m_data->editorFactory.reset();
    knownPlugins()->pluginInfoListChanged().disconnect(m_data.get());
    m_data->runtime.deinit();
    TracktionAudioIO::deinit();
    m_data->initialised = false;
}

bool TracktionAudioEngine::isBusy() const
{
    return m_data->state == Data::State::Starting || m_data->state == Data::State::Stopping
           || (m_data->stream && m_data->stream->kind != audio::AudioStreamKind::Monitoring);
}

bool TracktionAudioEngine::isCapturing() const
{
    return m_data->recording && m_data->runtime.isAudioDeviceRunning() && m_data->session->edit().getTransport().isRecording()
           && !m_data->session->edit().getTransport().isPaused() && streamTime() >= m_data->recordingStartTime;
}

bool TracktionAudioEngine::isStreamActive(AudacityProject& project) const
{
    if (m_data->state != Data::State::Running || !isBusy() || m_data->owner.lock().get() != &project) {
        return false;
    }
    return m_data->runtime.isAudioDeviceRunning() && m_data->session->edit().getTransport().isPlaying() && !m_data->hasReachedEnd();
}

double TracktionAudioEngine::streamTime() const
{
    if (!m_data->session) {
        return 0.0;
    }
    auto& transport = m_data->session->edit().getTransport();
    auto* context = transport.getCurrentPlaybackContext();
    if (!context || transport.isPaused()) {
        return transport.getPosition().inSeconds();
    }
    const double time = context->getAudibleTimelineTime().inSeconds();
    return m_data->recording ? std::min(time, m_data->endTime) : time;
}

bool TracktionAudioEngine::isMonitoring() const
{
    return m_data->stream && m_data->stream->kind == audio::AudioStreamKind::Monitoring;
}

std::optional<au::audio::AudioStreamDescriptor> TracktionAudioEngine::currentStream() const
{
    auto stream = m_data->stream;
    if (stream) {
        stream->ownerProject = m_data->owner.lock().get();
    }
    return stream;
}

int TracktionAudioEngine::startStream(const TransportSequences& sequences, double startTime, double endTime, double,
                                      AudacityProject& project, const StartStreamOptions& options)
{
    if (isBusy()) {
        m_data->error = "The audio device is in use.";
        return 0;
    }
    stopMonitoring();
    auto& data = *m_data;
    data.timer.stop();
    const bool recording = !sequences.captureSequences.empty();
    const bool variableSpeed = !recording && options.isDefaultPolicy
                               && ProjectAudioIO::GetDefaultOptions(project, true).variableSpeed;
    const auto configuration = audioDriverController()->configuration();
    const int inputChannels = recording ? configuration.inputChannels : 0;
    const bool monitorInput = recordConfiguration()->isInputMonitoringOn();
    const int outputChannels = recording && sequences.playbackSequences.empty() && sequences.otherPlayableSequences.empty()
                               && !monitorInput ? 0 : 2;
    const double sampleRate = options.sampleRate > 0.0 ? options.sampleRate : ProjectRate::Get(project).GetRate();
    data.owner = project.shared_from_this();
    data.stream = audio::AudioStreamDescriptor {
        recording ? audio::AudioStreamKind::Recording : audio::AudioStreamKind::Playback, &project, sampleRate
    };
    data.state = Data::State::Starting;
    std::vector<TracktionAudioCallbacks::CaptureTrack> captureTracks;
    for (const auto& sequence : sequences.captureSequences) {
        captureTracks.push_back({ sequence->GetRecordableSequenceId(), static_cast<unsigned>(sequence->NChannels()) });
    }
    {
        const juce::ScopedLock lock(data.runtime.engine().getDeviceManager().deviceManager.getAudioCallbackLock());
        data.callbacks->setMeters(recording ? ProjectAudioIO::Get(project).GetCaptureMeter() : nullptr,
                                  ProjectAudioIO::Get(project).GetPlaybackMeter());
        data.callbacks->setInputGain(TracktionAudioIO::instance().softwareInputGain());
        data.callbacks->setCaptureTracks(std::move(captureTracks));
    }
    data.error = data.runtime.openAudioDevice(configuration.api, configuration.inputDevice.value_or(""),
                                              configuration.outputDevice.value_or(""), inputChannels, outputChannels, sampleRate,
                                              data.callbacks);
    if (!data.error.empty()) {
        LOGE() << "Cannot open the audio device: " << data.error;
        stopStream();
        return 0;
    }
    auto* device = data.runtime.engine().getDeviceManager().deviceManager.getCurrentAudioDevice();
    data.rate = device->getCurrentSampleRate();
    try {
        data.session
            = std::make_unique<TracktionSession>(data.runtime, TrackList::Get(project), sequences, TracktionSession::Role::Playback);
        data.session->setPlaybackVolume(TracktionAudioIO::instance().playbackVolume());
        data.defaultPolicy = options.isDefaultPolicy && !recording;
        data.endTime = endTime;
        data.trackEndTime = TrackList::Get(project).GetEndTime();
        auto& transport = data.session->edit().getTransport();
        transport.setPosition(tracktion::TimePosition::fromSeconds(options.streamStartTime.value_or(startTime)));
        data.updateLoop(project, TracktionSession::PlaybackLoopUpdate::Start);
        if (variableSpeed) {
            data.session->setPlaybackSpeed(ProjectAudioIO::Get(project).GetPlaySpeed());
        }
        if (recording) {
            data.recordingStartTime = startTime;
            data.session->setLeadInEnd(sequences, startTime);
            TracktionRecording::Options recordOptions;
            recordOptions.monitorInput = monitorInput;
            recordOptions.automaticLatencyCompensation = configuration.automaticLatencyCompensation;
            recordOptions.inputGain = TracktionAudioIO::instance().softwareInputGain();
            recordOptions.latencyCompensationMs = configuration.latencyCompensation;
            recordOptions.leadInTime = options.leadInTime;
            if (options.crossfadeData) {
                recordOptions.crossfadeData = std::move(*options.crossfadeData);
            }
            if (endTime != std::numeric_limits<double>::max()) {
                recordOptions.duration = endTime - startTime;
            }
            const auto directory = juce::File(globalConfiguration()->userAppDataPath().toStdString()).getChildFile("tracktion/recording")
                                   .getChildFile(juce::Uuid().toString());
            data.recording = std::make_unique<TracktionRecording>(data.session->edit(), sequences.captureSequences, inputChannels,
                                                                  directory.getFullPathName().toStdString(), std::move(recordOptions));
            data.error = data.recording->start(startTime);
        } else {
            if (!data.session->startPlayback()) {
                data.error = "Cannot start audio playback.";
            }
        }
    } catch (const std::exception& exception) {
        data.error = exception.what();
    }
    if (!data.error.empty()) {
        LOGE() << "Cannot start the audio stream: " << data.error;
        stopStream();
        return 0;
    }
    data.nextToken = data.nextToken == std::numeric_limits<int>::max() ? 1 : data.nextToken + 1;
    const int token = data.nextToken;
    ProjectAudioIO::Get(project).SetAudioIOToken(token);
    TracktionAudioIO::instance().setStream(project.shared_from_this(), portAudioStream(device), token, data.rate);
    data.stream = audio::AudioStreamDescriptor {
        recording ? audio::AudioStreamKind::Recording : audio::AudioStreamKind::Playback, &project, data.rate
    };
    data.state = Data::State::Running;
    if (data.defaultPolicy) {
        data.regionSubscription = ViewInfo::Get(project).playRegion.Subscribe([this](const PlayRegionMessage&) {
            const auto owner = m_data->owner.lock();
            if (owner) {
                m_data->updateLoop(*owner);
            }
        });
    }
    if (variableSpeed) {
        data.speedSubscription = ProjectAudioIO::Get(project).Subscribe([this](const SpeedChangeMessage&) {
            const auto owner = m_data->owner.lock();
            if (owner) {
                m_data->session->setPlaybackSpeed(ProjectAudioIO::Get(*owner).GetPlaySpeed());
            }
        });
    }
    data.lastRecordingUpdate = std::chrono::steady_clock::now();
    data.timer.setInterval(20);
    data.timer.start();
    return token;
}

void TracktionAudioEngine::stopStream()
{
    auto& data = *m_data;
    if (!data.initialised || data.state == Data::State::Stopping || data.state == Data::State::Idle) {
        return;
    }
    data.timer.stop();
    data.regionSubscription.Reset();
    data.speedSubscription.Reset();
    const bool hadStream = data.state == Data::State::Running;
    const bool wasRecording = hadStream && data.stream->kind == audio::AudioStreamKind::Recording;
    data.state = Data::State::Stopping;
    const auto owner = data.owner.lock();
    if (data.recording) {
        const auto error = data.recording->stop();
        if (!error.empty()) {
            data.error = error;
        }
        data.recording.reset();
    }
    if (auto editor = data.instrumentEditor.lock()) {
        editor->close();
    }
    data.instrumentEditor.reset();
    if (data.session) {
        data.session->edit().getTransport().stop(false, true, false);
        data.session.reset();
    }
    data.restoreMonitoringInput();
    TracktionAudioIO::instance().setStream(owner, nullptr, 0, data.rate);
    {
        const juce::ScopedLock lock(data.runtime.engine().getDeviceManager().deviceManager.getAudioCallbackLock());
        data.callbacks->stop();
    }
    TracktionAudioIO::instance().clearStream();
    data.stream.reset();
    data.state = Data::State::Idle;
    if (hadStream && owner && data.error.empty()) {
        data.timer.setInterval(250);
        data.timer.start();
    } else {
        data.runtime.closeAudioDevice();
        data.owner.reset();
    }
    if (owner && hadStream) {
        ProjectAudioIO::Get(*owner).SetAudioIOToken(0);
    }
    if (wasRecording) {
        data.recordingCommitRequested.notify();
        data.recordingFinished.notify();
    }
    if (hadStream) {
        data.streamStopped.notify();
    }
}

void TracktionAudioEngine::pauseStream(bool pause)
{
    if (m_data->session && isBusy()) {
        m_data->session->edit().getTransport().setPaused(pause);
        TracktionAudioIO::instance().setPaused(pause);
    }
}

void TracktionAudioEngine::seekStream(double time)
{
    if (m_data->session && !m_data->recording) {
        const juce::ScopedLock lock(m_data->runtime.engine().getDeviceManager().deviceManager.getAudioCallbackLock());
        m_data->session->edit().getTransport().setPosition(tracktion::TimePosition::fromSeconds(time));
        m_data->session->setPlaybackEnd(m_data->session->edit().getTransport().looping ? std::nullopt : std::make_optional(m_data->endTime));
        m_data->callbacks->setPlaybackControls(m_data->session->playbackControls());
    }
}

std::shared_ptr<RealtimeEffectState> TracktionAudioEngine::addRealtimeEffectState(AudacityProject& project, ChannelGroup* group,
                                                                                  const std::string& effectId)
{
    auto& list = group ? RealtimeEffectList::Get(*group) : RealtimeEffectList::Get(project);
    const auto state = std::make_shared<RealtimeEffectState>(au3::wxFromStdString(effectId));
    return list.AddState(state) ? state : nullptr;
}

void TracktionAudioEngine::removeRealtimeEffectState(AudacityProject& project, ChannelGroup* group,
                                                     const std::shared_ptr<RealtimeEffectState>& state)
{
    auto& list = group ? RealtimeEffectList::Get(*group) : RealtimeEffectList::Get(project);
    list.RemoveState(state);
}

std::shared_ptr<RealtimeEffectState> TracktionAudioEngine::replaceRealtimeEffectState(AudacityProject& project, ChannelGroup* group,
                                                                                      size_t effectListIndex, const std::string& effectId)
{
    auto& list = group ? RealtimeEffectList::Get(*group) : RealtimeEffectList::Get(project);
    const auto state = std::make_shared<RealtimeEffectState>(au3::wxFromStdString(effectId));
    return list.ReplaceState(effectListIndex, state) ? state : nullptr;
}

void TracktionAudioEngine::startMonitoring(AudacityProject& project)
{
    if (isBusy()) {
        return;
    }
    stopMonitoring();
    auto& data = *m_data;
    data.timer.stop();
    const auto configuration = audioDriverController()->configuration();
    const bool monitorInput = recordConfiguration()->isInputMonitoringOn();
    data.owner = project.shared_from_this();
    data.stream = audio::AudioStreamDescriptor { audio::AudioStreamKind::Monitoring, &project, ProjectRate::Get(project).GetRate() };
    data.state = Data::State::Starting;
    {
        const juce::ScopedLock lock(data.runtime.engine().getDeviceManager().deviceManager.getAudioCallbackLock());
        data.callbacks->setMeters(ProjectAudioIO::Get(project).GetCaptureMeter(), ProjectAudioIO::Get(project).GetPlaybackMeter());
        data.callbacks->setInputGain(TracktionAudioIO::instance().softwareInputGain());
    }
    data.error = data.runtime.openAudioDevice(configuration.api, configuration.inputDevice.value_or(""),
                                              configuration.outputDevice.value_or(""), configuration.inputChannels, monitorInput ? 2 : 0,
                                              ProjectRate::Get(project).GetRate(), data.callbacks);
    if (data.error.empty()) {
        try {
            const TransportSequences sequences;
            data.session = std::make_unique<TracktionSession>(data.runtime, TrackList::Get(
                                                                  project), sequences, TracktionSession::Role::Playback);
            data.session->setPlaybackVolume(TracktionAudioIO::instance().playbackVolume());
            data.error = data.connectMonitoringInput(configuration.inputChannels, monitorInput);
        } catch (const std::exception& exception) {
            data.error = exception.what();
        }
    }
    if (!data.error.empty()) {
        stopStream();
        return;
    }
    auto* device = data.runtime.engine().getDeviceManager().deviceManager.getCurrentAudioDevice();
    data.rate = device->getCurrentSampleRate();
    data.stream = audio::AudioStreamDescriptor { audio::AudioStreamKind::Monitoring, &project, data.rate };
    data.state = Data::State::Running;
    TracktionAudioIO::instance().setStream(project.shared_from_this(), portAudioStream(device), 0, data.rate);
    data.timer.setInterval(20);
    data.timer.start();
}

void TracktionAudioEngine::stopMonitoring()
{
    if (isMonitoring()) {
        stopStream();
    }
}

void TracktionAudioEngine::setInputVolume(float volume)
{
    TracktionAudioIO::instance().setInputVolume(volume);
    const juce::ScopedLock lock(m_data->runtime.engine().getDeviceManager().deviceManager.getAudioCallbackLock());
    m_data->callbacks->setInputGain(TracktionAudioIO::instance().softwareInputGain());
    for (auto* input : m_data->runtime.engine().getDeviceManager().getWaveInputDevices()) {
        TracktionRuntime::setInputGain(*input, TracktionAudioIO::instance().softwareInputGain());
    }
}

float TracktionAudioEngine::getInputVolume() const
{
    return TracktionAudioIO::instance().inputVolume();
}

void TracktionAudioEngine::setPlaybackVolume(float volume)
{
    TracktionAudioIO::instance().setPlaybackVolume(volume);
    if (m_data->session) {
        m_data->session->setPlaybackVolume(volume);
    }
}

float TracktionAudioEngine::getPlaybackVolume() const
{
    return TracktionAudioIO::instance().playbackVolume();
}

bool TracktionAudioEngine::canStopAudioStream(AudacityProject& project) const
{
    return !isBusy() || m_data->owner.expired() || m_data->owner.lock().get() == &project;
}

void TracktionAudioEngine::handleDeviceChange()
{
    if (m_data->state == Data::State::Idle) {
        m_data->timer.stop();
        m_data->runtime.closeAudioDevice();
        m_data->owner.reset();
    }
    TracktionAudioIO::instance().refreshDevices();
}

muse::String TracktionAudioEngine::lastErrorString() const
{
    return muse::String::fromStdString(m_data->error);
}

double TracktionAudioEngine::getPlaybackSampleRate() const
{
    return m_data->rate;
}

void TracktionAudioEngine::updateTimePosition(unsigned long)
{
    // Tracktion supplies the position from its audio clock.
}

std::optional<au::audio::AudioCallbackInfo> TracktionAudioEngine::consumeNextCallbackInfo()
{
    return m_data->callbacks->consumeCallbackInfo();
}

void TracktionAudioEngine::updateStream()
{
    auto& data = *m_data;
    if (data.state == Data::State::Idle) {
        if (data.owner.expired() || !data.runtime.isAudioDeviceRunning()) {
            data.timer.stop();
            data.runtime.closeAudioDevice();
            data.owner.reset();
        }
        return;
    }
    const auto owner = data.owner.lock();
    if (!owner || !data.runtime.isAudioDeviceRunning()) {
        stopStream();
        return;
    }
    if (isBusy() && !data.session->edit().getTransport().isPlaying()) {
        stopStream();
        return;
    }
    {
        const juce::ScopedLock lock(data.runtime.engine().getDeviceManager().deviceManager.getAudioCallbackLock());
        const bool inputMeterEnabled = data.recording || !data.monitoringInputID.isEmpty();
        data.callbacks->setMeters(inputMeterEnabled ? ProjectAudioIO::Get(*owner).GetCaptureMeter() : nullptr,
                                  ProjectAudioIO::Get(*owner).GetPlaybackMeter());
    }
    const auto now = std::chrono::steady_clock::now();
    if (data.recording && now - data.lastRecordingUpdate >= std::chrono::milliseconds(100)) {
        data.error = data.recording->update();
        data.lastRecordingUpdate = now;
        data.recordingUpdateRequested.notify();
        if (!data.error.empty()) {
            stopStream();
            return;
        }
    }
    if (data.hasReachedEnd()) {
        stopStream();
    }
}

muse::async::Notification TracktionAudioEngine::recordingUpdateRequested() const { return m_data->recordingUpdateRequested; }
muse::async::Notification TracktionAudioEngine::recordingCommitRequested() const { return m_data->recordingCommitRequested; }
muse::async::Notification TracktionAudioEngine::recordingFinished() const { return m_data->recordingFinished; }
muse::async::Notification TracktionAudioEngine::streamStopped() const { return m_data->streamStopped; }
muse::async::Channel<au::au3::Au3TrackId, au::au3::Au3ClipId> TracktionAudioEngine::recordingClipChanged() const
{
    return m_data->recordingClipChanged;
}

muse::RetVal<std::shared_ptr<au::audio::IInstrumentEditor> > TracktionAudioEngine::openInstrument(AudacityProject& project,
                                                                                                  MidiTrack& track)
{
    auto& data = *m_data;
    const auto stream = currentStream();
    if (stream && stream->ownerProject != &project) {
        return muse::make_ret(muse::Ret::Code::InternalError, "Stop audio in the other project first.");
    }
    if (isCapturing()) {
        return muse::make_ret(muse::Ret::Code::InternalError, "Stop audio recording first.");
    }
    auto* target = data.session ? data.session->instrumentTrack(track) : nullptr;
    if (!target) {
        if (isBusy()) {
            return muse::make_ret(muse::Ret::Code::InternalError, "Stop playback before changing the instrument track.");
        }
        stopMonitoring();
        data.timer.stop();
        const auto configuration = audioDriverController()->configuration();
        data.owner = project.shared_from_this();
        data.stream = audio::AudioStreamDescriptor { audio::AudioStreamKind::Monitoring, &project, ProjectRate::Get(project).GetRate() };
        data.state = Data::State::Starting;
        data.error = data.runtime.openAudioDevice(configuration.api, "", configuration.outputDevice.value_or(""), 0, 2,
                                                  ProjectRate::Get(project).GetRate(), data.callbacks);
        if (data.error.empty()) {
            try {
                TransportSequences sequences;
                sequences.otherPlayableSequences.push_back(std::static_pointer_cast<MidiTrack>(track.shared_from_this()));
                data.session = std::make_unique<TracktionSession>(data.runtime, TrackList::Get(project), sequences,
                                                                  TracktionSession::Role::Playback);
                data.session->setPlaybackVolume(TracktionAudioIO::instance().playbackVolume());
                auto& edit = data.session->edit();
                edit.playInStopEnabled = true;
                edit.getTransport().ensureContextAllocated();
                edit.dispatchPendingUpdatesSynchronously();
                target = data.session->instrumentTrack(track);
            } catch (const std::exception& error) {
                data.error = error.what();
            }
        }
        if (!data.error.empty() || !target) {
            const auto error = data.error.empty() ? "Cannot create the instrument input." : data.error;
            stopStream();
            return muse::make_ret(muse::Ret::Code::InternalError, error);
        }
        auto* device = data.runtime.engine().getDeviceManager().deviceManager.getCurrentAudioDevice();
        data.rate = device->getCurrentSampleRate();
        data.stream->sampleRate = data.rate;
        data.state = Data::State::Running;
        TracktionAudioIO::instance().setStream(project.shared_from_this(), portAudioStream(device), 0, data.rate);
        data.timer.setInterval(20);
        data.timer.start();
    }
    if (auto editor = data.instrumentEditor.lock()) {
        editor->close();
    }
    const auto editor = createLiveInstrumentEditor(data.session->edit(), *target);
    data.instrumentEditor = editor;
    return muse::RetVal<std::shared_ptr<audio::IInstrumentEditor> >::make_ok(editor);
}

void TracktionAudioEngine::closeInstrument(const std::shared_ptr<audio::IInstrumentEditor>& editor)
{
    const bool ownsEditor = m_data->instrumentEditor.lock() == editor;
    editor->close();
    if (ownsEditor && isMonitoring()) {
        stopMonitoring();
    }
}
