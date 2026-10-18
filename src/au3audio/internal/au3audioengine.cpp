#include "au3audioengine.h"
#include "au3-numeric-formats/ProjectTimeSignature.h"
#include "nativetimesignature.h"

#include "framework/global/log.h"
#include "framework/global/realfn.h"

#include "au3-audio-io/AudioIO.h"
#include "au3-audio-io/ProjectAudioIO.h"
#include "au3-track/Track.h"
#include "au3-time-frequency-selection/ViewInfo.h"

#include "au3wrap/au3types.h"
#include "au3wrap/internal/wxtypes_convert.h"

#include "defaultplaybackpolicy.h"
#include "au3audioiolistener.h"
#include "midiplayablesequence.h"
#include "liveinstrumentsession.h"
#include "nativeinstrumentsessions.h"
#include "nativemonitorsource.h"
#include "au3-stretching-sequence/StretchingSequence.h"
#include "au3-playable-track/TrackRouting.h"
#include "au3-wave-track/WaveTrack.h"
#include <algorithm>
#include <set>
#include <stdexcept>
#include "au3-project-rate/ProjectRate.h"
#include "au3-project/Project.h"

using namespace au::au3audio;

std::shared_ptr<Au3AudioIOListener> s_audioIOListener;

static ProjectAudioIO::DefaultOptions::Scope s_defaultOptionsScope {
    [](au::au3::Au3Project& project, const bool newDefault) -> AudioIOStartStreamOptions {
        auto options = ProjectAudioIO::DefaultOptionsFactory(project, newDefault);
        options.listener = s_audioIOListener;

        const auto& playRegion = ViewInfo::Get(project).playRegion;
        const bool loopEnabled = playRegion.Active();
        options.loopEnabled = loopEnabled;

        if (newDefault) {
            const double trackEndTime = TrackList::Get(project).GetEndTime();
            const double loopEndTime = playRegion.GetEnd();
            options.policyFactory = [&project, trackEndTime, loopEndTime](
                const AudioIOStartStreamOptions& options) -> std::unique_ptr<PlaybackPolicy>
            {
                return std::make_unique<DefaultPlaybackPolicy>(project,
                                                               trackEndTime, loopEndTime, options.pStartTime,
                                                               options.loopEnabled, options.variableSpeed);
            };

            double startTime = playRegion.GetStart();
            options.pStartTime.emplace(muse::RealIsEqualOrMore(startTime, 0.0) ? startTime : 0.0);
        }

        return options;
    } };

void Au3AudioEngine::init()
{
    m_monitorResume.setSingleShot(true);
    m_monitorResume.callOnTimeout([this] { resumeInstrumentMonitoring(); });
    m_monitorRefresh.setInterval(20);
    m_monitorRefresh.callOnTimeout([this] {
        const auto project = m_liveProject.lock();
        if (m_liveMonitor && project && AudioIO::Get()->IsMonitoring()
            && (!m_liveMonitor->ready() || m_liveMonitor->NeedsRefresh(ViewInfo::Get(*project).selectedRegion.t0()))) {
            auto& engine = *AudioIO::Get();
            engine.SetLiveOutput({});
            if (!m_liveMonitor->Refresh(ViewInfo::Get(*project).selectedRegion.t0())) {
                m_startError = muse::String(u"Cannot refresh native instrument monitoring.");
                LOGE() << m_startError;
                stopStream();
                m_monitorResume.stop();
                if (m_liveInstrument) {
                    m_liveInstrument->reportProcessingFailure(m_startError.toStdString());
                }
                return;
            }
            engine.SetLiveOutput(m_liveMonitor);
        }
    });
    s_audioIOListener = std::make_shared<Au3AudioIOListener>();
    AudioIO::Init();
    m_effectRefresh.setInterval(50);
    m_effectRefresh.callOnTimeout([this] {
        try {
            AudioIO::Get()->RefreshRealtimeEffectLatencies();
            refreshInstrumentLatencies();
        } catch (const std::exception& error) {
            m_startError = muse::String::fromUtf8(error.what());
            LOGE() << "Native processing latency refresh failed: " << error.what();
            stopStream();
            m_monitorResume.stop();
            if (m_liveInstrument) {
                m_liveInstrument->reportProcessingFailure(m_startError.toStdString());
            }
        }
    });
    m_effectRefresh.start();

    m_streamStatusSubscription = AudioIO::Get()->Subscribe([this](const AudioIOEvent& event) {
        if (!event.on && event.type != AudioIOEvent::PAUSE) {
            m_streamStopped.notify();
        }
    });
}

void Au3AudioEngine::refreshInstrumentLatencies()
{
    const auto pending = [](const auto& session) { return session->inTimeline() && session->latencyUpdatePending(); };
    if (!std::any_of(m_transportSessions.begin(), m_transportSessions.end(), pending)) {
        return;
    }
    // These sessions belong only to the disk worker, never the live monitor
    // callback. Its acknowledgement releases their native DSP for main-thread
    // control. Already-rendered Mixer packets keep their original positions.
    AudioIO::Get()->WithAudioThreadStopped([&] {
        try {
            for (const auto& session : m_transportSessions) {
                if (!pending(session)) {
                    continue;
                }
                for (unsigned attempt = 0; attempt < 8 && session->latencyUpdatePending(); ++attempt) {
                    session->refreshLatency();
                }
                if (session->latencyUpdatePending()) {
                    throw std::runtime_error("The instrument's processing delay did not stabilize.");
                }
            }
        } catch (const std::exception& error) {
            // The exclusive scope resumes its worker while unwinding. Gate
            // failed DSP before that happens; StopStream follows on main.
            for (const auto& session : m_transportSessions) {
                session->reportProcessingFailure(error.what());
            }
            throw;
        }
    });
}

void Au3AudioEngine::deinit()
{
    m_monitorResume.stop();
    m_monitorRefresh.stop();
    m_effectRefresh.stop();
    if (m_liveInstrument) {
        closeInstrument(m_liveInstrument);
    }
    m_streamStatusSubscription.Reset();
    AudioIO::Deinit();
    for (const auto& session : m_transportSessions) {
        session->endTimeline();
    }
    m_transportSessions.clear();
}

bool Au3AudioEngine::isBusy() const
{
    return AudioIO::Get()->IsBusy();
}

bool Au3AudioEngine::isCapturing() const
{
    return AudioIO::Get()->IsCapturing();
}

bool Au3AudioEngine::isStreamActive(AudacityProject& project) const
{
    const int token = ProjectAudioIO::Get(project).GetAudioIOToken();
    return AudioIO::Get()->IsStreamActive(token);
}

double Au3AudioEngine::streamTime() const
{
    return AudioIO::Get()->GetStreamTime();
}

bool Au3AudioEngine::isMonitoring() const
{
    return AudioIO::Get()->IsMonitoring();
}

std::optional<au::audio::AudioStreamDescriptor> Au3AudioEngine::currentStream() const
{
    const auto audioIO = AudioIO::Get();
    if (!audioIO->IsBusy() && !audioIO->IsStreamActive() && !audioIO->IsMonitoring()) {
        return std::nullopt;
    }

    const auto owner = audioIO->GetOwningProject();

    au::audio::AudioStreamKind kind = au::audio::AudioStreamKind::Playback;
    if (audioIO->IsMonitoring()) {
        kind = au::audio::AudioStreamKind::Monitoring;
    } else if (audioIO->GetNumCaptureChannels() > 0) {
        // Recording lead-in has capture channels before IsCapturing() becomes true.
        kind = au::audio::AudioStreamKind::Recording;
    }

    return au::audio::AudioStreamDescriptor {
        kind,
        owner.get(),
        audioIO->GetPlaybackSampleRate(),
    };
}

int Au3AudioEngine::startStream(const TransportSequences& sequences, const double startTime, const double endTime,
                                const double mixerEndTime,
                                AudacityProject& project, const StartStreamOptions& options)
{
    AudioIOStartStreamOptions au3Options = ProjectAudioIO::GetDefaultOptions(project, options.isDefaultPolicy);
    au3Options.inputMonitoring = recordConfiguration()->isInputMonitoringOn();
    au3Options.rate = options.sampleRate;
    au3Options.leadInTime = options.leadInTime;
    if (options.crossfadeData) {
        au3Options.pCrossfadeData = options.crossfadeData;
    }
    if (options.streamStartTime.has_value()) {
        au3Options.pStartTime = *options.streamStartTime;
    }
    auto& audioIO = *AudioIO::Get();
    if (audioIO.IsBusy()) {
        return 0;
    }
    m_monitorResume.stop();
    // Stop the callback before preparing/reusing an instrument on the disk
    // worker. Never run one native plug-in on both audio threads.
    audioIO.StopLiveOutput();
    m_monitorRefresh.stop();
    m_liveMonitor.reset();
    audioIO.StopMonitoring();
    m_liveMonitoring = false;
    m_startError.clear();
    auto transport = sequences;
    m_transportSessions.clear();
    try {
        std::set<const ChannelGroup*> included;
        for (const auto& sequence : transport.playbackSequences) {
            included.insert(sequence->FindChannelGroup());
        }
        auto& sessions = NativeInstrumentSessions::Get(project);
        const auto& signature = ProjectTimeSignature::Get(project);
        const auto& liveSignature = NativeTimeSignature::Get(project);
        const auto addMidi = [&](const std::shared_ptr<const MidiTrack>& midi, bool playClips) {
            if (!included.insert(midi.get()).second) {
                return;
            }
            auto session = sessions.get(project, std::const_pointer_cast<MidiTrack>(midi));
            session->beginTimeline();
            m_transportSessions.push_back(session);
            transport.playbackSequences.push_back(std::make_shared<MidiPlayableSequence>(midi, session, options.sampleRate, true, playClips,
                                                                                         signature.GetUpperTimeSignature(),
                                                                                         signature.GetLowerTimeSignature(), 0.0,
                                                                                         &liveSignature));
        };
        for (const auto& sequence : sequences.otherPlayableSequences) {
            if (const auto midi = std::dynamic_pointer_cast<const MidiTrack>(sequence)) {
                addMidi(midi, true);
            }
        }
        // Keep input-only sources for other instrument tracks. Opening an
        // editor during selected playback can then use the same routed graph
        // without starting the clips on an unselected track.
        for (auto* midi : TrackList::Get(project).Any<MidiTrack>()) {
            addMidi(std::static_pointer_cast<MidiTrack>(midi->shared_from_this()), false);
        }
        for (const auto* bus : TrackList::Get(project).Any<const WaveTrack>()) {
            if (!TrackRouting::Get(*bus).busId.empty() && included.insert(bus).second) {
                transport.playbackSequences.push_back(StretchingSequence::Create(*bus, bus->GetClipInterfaces()));
            }
        }
    } catch (const std::exception& error) {
        for (const auto& session : m_transportSessions) {
            session->endTimeline();
        }
        m_transportSessions.clear();
        m_startError = muse::String::fromUtf8(error.what());
        LOGE() << m_startError;
        m_monitorResume.start(0);
        return 0;
    }
    au3Options.interactivePlayback = !m_transportSessions.empty();
    const int token = audioIO.StartStream(transport, startTime, endTime, mixerEndTime, au3Options);
    if (token > 0) {
        ProjectAudioIO::Get(project).SetAudioIOToken(token);
        LOGI() << "PortAudio latency report (ms): outputLatency=" << audioIO.GetHardwarePlaybackLatencyMs() <<
            ", inputLatency=" << audioIO.GetHardwareCaptureLatencyMs();
    } else {
        for (const auto& session : m_transportSessions) {
            session->endTimeline();
        }
        m_transportSessions.clear();
        m_monitorResume.start(0);
    }
    return token;
}

void Au3AudioEngine::stopStream()
{
    AudioIO::Get()->StopStream();
    AudioIO::Get()->WaitWhileBusy();
    m_monitorRefresh.stop();
    m_liveMonitor.reset();
    for (const auto& session : m_transportSessions) {
        session->endTimeline();
    }
    m_transportSessions.clear();
    m_liveMonitoring = false;
    // Coalesce stop/start commands in one event loop pass. Reopening the
    // monitoring device must not add a second synchronous device start to Stop.
    if (m_liveInstrument && m_liveInstrument->active()) {
        m_monitorResume.start(0);
    }
}

std::shared_ptr<au::audio::IInstrumentEditor> Au3AudioEngine::existingInstrument(AudacityProject& project, const MidiTrack& track) const
{
    return NativeInstrumentSessions::Get(project).find(track);
}

muse::RetVal<std::shared_ptr<au::audio::IInstrumentEditor> > Au3AudioEngine::openInstrument(AudacityProject& project, MidiTrack& track)
{
    const auto stream = currentStream();
    if (stream && stream->ownerProject != &project) {
        return muse::make_ret(muse::Ret::Code::InternalError, "Stop audio in the other project first.");
    }
    if (m_liveInstrument && m_liveInstrument->active() && m_liveTrack == &track && m_liveProject.lock().get() == &project
        && m_liveInstrument->matchesDocument()) {
        if (m_liveInstrument->inTimeline() || resumeInstrumentMonitoring()) {
            return muse::RetVal<std::shared_ptr<audio::IInstrumentEditor> >::make_ok(m_liveInstrument);
        }
        return muse::make_ret(muse::Ret::Code::InternalError, m_startError.toStdString());
    }
    if (m_liveInstrument) {
        closeInstrument(m_liveInstrument);
    }
    try {
        auto session = NativeInstrumentSessions::Get(project).get(project, std::static_pointer_cast<MidiTrack>(track.shared_from_this()));
        if (stream && !AudioIO::Get()->IsMonitoring() && !session->inTimeline()) {
            return muse::make_ret(muse::Ret::Code::InternalError, "This instrument is not part of the active playback stream.");
        }
        session->open();
        m_liveInstrument = session;
        m_liveProject = project.shared_from_this();
        m_liveTrack = &track;
        if (!session->inTimeline()) {
            m_startError.clear();
            if (!resumeInstrumentMonitoring()) {
                closeInstrument(session);
                return muse::make_ret(muse::Ret::Code::InternalError,
                                      m_startError.isEmpty() ? AudioIO::Get()->LastPaErrorString().utf8_string() : m_startError.toStdString());
            }
        }
        return muse::RetVal<std::shared_ptr<audio::IInstrumentEditor> >::make_ok(session);
    } catch (const std::exception& error) {
        return muse::make_ret(muse::Ret::Code::InternalError, error.what());
    }
}

bool Au3AudioEngine::resumeInstrumentMonitoring()
{
    const auto project = m_liveProject.lock();
    if (!project || !m_liveInstrument || !m_liveInstrument->active()) {
        return false;
    }
    if (m_liveInstrument->inTimeline()) {
        return true;
    }
    auto& engine = *AudioIO::Get();
    if (engine.IsBusy()) {
        return false;
    }
    if (m_liveMonitor && m_liveMonitor->ready() && engine.IsMonitoring()) {
        return true;
    }
    try {
        engine.StopLiveOutput();
        m_liveMonitor.reset();
        auto monitor = std::make_shared<NativeMonitorSource>(project,
                                                             std::static_pointer_cast<MidiTrack>(const_cast<MidiTrack*>(m_liveTrack)->
                                                                                                 shared_from_this()), m_liveInstrument,
                                                             ViewInfo::Get(*project).selectedRegion.t0(), ProjectAudioIO::Get(
                                                                 *project).GetPlaybackMeter());
        if (engine.IsMonitoring() && engine.GetNumPlaybackChannels() > 0 && engine.IsInteractiveStream()) {
            monitor->Prepare(engine.GetPlaybackSampleRate());
            engine.SetLiveOutput(monitor);
        } else {
            engine.StopMonitoring();
            auto options = ProjectAudioIO::GetDefaultOptions(*project, true);
            options.rate = ProjectRate::Get(*project).GetRate();
            options.inputMonitoring = recordConfiguration()->isInputMonitoringOn();
            options.liveOutput = monitor;
            engine.StartMonitoring(options);
            m_liveMonitoring = engine.IsMonitoring();
        }
        m_liveMonitor = std::move(monitor);
        if (m_liveMonitor->ready()) {
            m_monitorRefresh.start();
        }
        LOGI() << "Instrument monitor latency report (ms): outputLatency=" << engine.GetHardwarePlaybackLatencyMs()
               << ", inputLatency=" << engine.GetHardwareCaptureLatencyMs();
    } catch (const std::exception& error) {
        m_startError = muse::String::fromUtf8(error.what());
        LOGE() << m_startError;
        return false;
    }
    const bool ready = engine.IsMonitoring() && engine.GetNumPlaybackChannels() > 0;
    if (ready) {
        m_startError.clear();
    }
    return ready;
}

void Au3AudioEngine::closeInstrument(const std::shared_ptr<audio::IInstrumentEditor>& editor)
{
    if (!editor) {
        return;
    }
    if (editor == m_liveInstrument) {
        m_monitorResume.stop();
        m_monitorRefresh.stop();
        auto& engine = *AudioIO::Get();
        const auto owner = m_liveProject.lock();
        if (owner && canStopAudioStream(*owner)) {
            engine.StopLiveOutput();
            m_liveMonitor.reset();
            if (m_liveMonitoring && engine.IsMonitoring()) {
                engine.StopMonitoring();
            }
        }
        m_liveInstrument.reset();
        m_liveProject.reset();
        m_liveTrack = nullptr;
        m_liveMonitoring = false;
    }
    editor->close();
}

std::shared_ptr<RealtimeEffectState> Au3AudioEngine::addRealtimeEffectState(AudacityProject& project, ChannelGroup* group,
                                                                            const std::string& effectId)
{
    return AudioIO::Get()->AddState(project, group, au::au3::wxFromStdString(effectId));
}

void Au3AudioEngine::removeRealtimeEffectState(AudacityProject& project, ChannelGroup* group,
                                               const std::shared_ptr<RealtimeEffectState>& state)
{
    AudioIO::Get()->RemoveState(project, group, state);
}

std::shared_ptr<RealtimeEffectState> Au3AudioEngine::replaceRealtimeEffectState(AudacityProject& project, ChannelGroup* group,
                                                                                size_t effectListIndex, const std::string& newEffectId)
{
    return AudioIO::Get()->ReplaceState(project, group, effectListIndex, au::au3::wxFromStdString(newEffectId));
}

void Au3AudioEngine::pauseStream(const bool pause)
{
    AudioIO::Get()->SetPaused(pause);
}

void Au3AudioEngine::seekStream(double time)
{
    AudioIO::Get()->SeekStream(time - AudioIO::Get()->GetStreamTime());
}

void Au3AudioEngine::startMonitoring(AudacityProject& project)
{
    if (m_liveInstrument && m_liveInstrument->active() && m_liveProject.lock().get() == &project
        && !m_liveInstrument->inTimeline() && resumeInstrumentMonitoring()) {
        return;
    }
    AudioIOStartStreamOptions options = ProjectAudioIO::GetDefaultOptions(project);
    options.inputMonitoring = recordConfiguration()->isInputMonitoringOn();
    AudioIO::Get()->StartMonitoring(options);
}

void Au3AudioEngine::stopMonitoring()
{
    m_monitorRefresh.stop();
    AudioIO::Get()->StopMonitoring();
    m_liveMonitor.reset();
}

void Au3AudioEngine::setInputVolume(const float newInputVolume)
{
    int inputSource;
    float inputVolume;
    float playbackVolume;
    AudioIO::Get()->GetMixer(&inputSource, &inputVolume, &playbackVolume);
    AudioIO::Get()->SetMixer(inputSource, newInputVolume, playbackVolume);
}

float Au3AudioEngine::getInputVolume() const
{
    int inputSource;
    float inputVolume;
    float playbackVolume;
    AudioIO::Get()->GetMixer(&inputSource, &inputVolume, &playbackVolume);
    return inputVolume;
}

void Au3AudioEngine::setPlaybackVolume(const float newPlaybackVolume)
{
    int inputSource;
    float inputVolume;
    float playbackVolume;
    AudioIO::Get()->GetMixer(&inputSource, &inputVolume, &playbackVolume);
    AudioIO::Get()->SetMixer(inputSource, inputVolume, newPlaybackVolume);
}

float Au3AudioEngine::getPlaybackVolume() const
{
    int inputSource;
    float inputVolume;
    float playbackVolume;
    AudioIO::Get()->GetMixer(&inputSource, &inputVolume, &playbackVolume);
    return playbackVolume;
}

bool Au3AudioEngine::canStopAudioStream(AudacityProject& project) const
{
    return !AudioIO::Get()->IsStreamActive()
           || AudioIO::Get()->IsMonitoring()
           || AudioIO::Get()->GetOwningProject().get() == &project;
}

void Au3AudioEngine::handleDeviceChange()
{
    AudioIO::Get()->HandleDeviceChange();
    if (m_liveInstrument && m_liveInstrument->active() && !m_liveInstrument->inTimeline()) {
        m_monitorResume.start(0);
    }
}

muse::String Au3AudioEngine::lastErrorString() const
{
    if (!m_startError.isEmpty()) {
        return m_startError;
    }
    return au::au3::wxToString(AudioIO::Get()->LastPaErrorString());
}

double Au3AudioEngine::getPlaybackSampleRate() const
{
    return AudioIO::Get()->GetPlaybackSampleRate();
}

void Au3AudioEngine::updateTimePosition(const unsigned long newlyConsumedSamples)
{
    AudioIO::Get()->UpdateTimePosition(newlyConsumedSamples);
}

std::optional<au::audio::AudioCallbackInfo> Au3AudioEngine::consumeNextCallbackInfo()
{
    AudioIoCallback::AudioCallbackInfo au3Info;
    if (AudioIO::Get()->GetAudioCallbackInfoQueue().Get(au3Info)) {
        return au::audio::AudioCallbackInfo { au3Info.dacTime, au3Info.numSamples };
    }
    return std::nullopt;
}

muse::async::Notification Au3AudioEngine::recordingUpdateRequested() const
{
    return s_audioIOListener->recordingUpdateRequested();
}

muse::async::Notification Au3AudioEngine::recordingCommitRequested() const
{
    return s_audioIOListener->recordingCommitRequested();
}

muse::async::Notification Au3AudioEngine::recordingFinished() const
{
    return s_audioIOListener->recordingFinished();
}

muse::async::Notification Au3AudioEngine::streamStopped() const
{
    return m_streamStopped;
}

muse::async::Channel<au::au3::Au3TrackId, au::au3::Au3ClipId> Au3AudioEngine::recordingClipChanged() const
{
    return s_audioIOListener->recordingClipChanged();
}
