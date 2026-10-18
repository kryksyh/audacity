/*
* Audacity: A Digital Audio Editor
*/
#include "mixrenderscenario.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <algorithm>
#include <cmath>
#include <stdexcept>
#include "global/defer.h"
#include "global/progress.h"
#include "global/translation.h"
#include "audio/iaudioengine.h"
#include "context/iglobalcontext.h"
#include "interactive/iinteractive.h"
#include "trackedit/iprojecthistory.h"
#include "trackedit/itracksviewrequestsservice.h"
#include "au3-midi-track/MidiTrack.h"
#include "au3-mixer/AudioRenderSource.h"
#include "au3-playable-track/TrackRouting.h"
#include "au3-project-rate/ProjectRate.h"
#include "au3-project/Project.h"
#include "au3-realtime-effects/RealtimeEffectList.h"
#include "au3-wave-track/WaveClip.h"
#include "au3-wave-track/WaveTrack.h"
#include "au3-xml/XMLWriter.h"
#include "project/iaudacityproject.h"
#include "trackedit/itrackeditproject.h"

using namespace au::trackedit;

namespace {
wxString documentState(const ::TrackList& tracks)
{
    XMLStringWriter writer;
    writer.StartTag("mix-sources");
    for (const auto* track : tracks.Any<const PlayableTrack>()) {
        track->WriteXML(writer);
    }
    writer.EndTag("mix-sources");
    return writer;
}
}

MixRenderScenario::MixRenderScenario(const muse::modularity::ContextPtr& ctx)
    : muse::Contextable(ctx) {}

muse::Ret MixRenderScenario::run()
{
    const auto owner = globalContext()->currentProject();
    if (m_running || !owner || globalContext()->isRecording()) {
        return muse::make_ret(muse::Ret::Code::NotSupported);
    }
    m_running = true;
    DEFER { m_running = false;
    };
    auto& project = *reinterpret_cast<AudacityProject*>(owner->au3ProjectPtr());
    const auto projectLifetime = project.shared_from_this();
    const auto historyService = history();
    viewRequests()->requestFinishInstrumentEditing();
    const auto stopAudio = [&] {
        const auto stream = audioEngine()->currentStream();
        if (!stream) {
            return true;
        }
        if (stream->ownerProject != &project || !audioEngine()->canStopAudioStream(project)) {
            return false;
        }
        audioEngine()->stopStream();
        return true;
    };
    if (!stopAudio()) {
        return muse::make_ret(muse::Ret::Code::NotSupported);
    }

    auto& tracks = ::TrackList::Get(project);
    const auto sources = TrackRouting::AudibleSources(tracks, true, TrackRouting::Audibility::Ignore);
    if (sources.empty()) {
        return muse::make_ret(muse::Ret::Code::NotSupported);
    }
    muse::Progress progress;
    progress.setMaxNumIncrements(200);
    auto result = muse::make_ok();
    DEFER { if (progress.isStarted()) {
                progress.finish(result);
            }
    };
    try {
        const auto before = documentState(tracks);
        AudioRenderSource::Tracks waves;
        AudioRenderSource::Options options;
        options.startTime = sources.front()->GetStartTime();
        options.stopTime = sources.front()->GetEndTime();
        options.sampleRate = ProjectRate::Get(project).GetRate();
        for (const auto* original : sources) {
            options.startTime = std::min(options.startTime, original->GetStartTime());
            options.stopTime = std::max(options.stopTime, original->GetEndTime());
            auto snapshot = original->Duplicate();
            RealtimeEffectList::Get(*static_cast<PlayableTrack*>(snapshot.get())).CloneStates();
            if (auto wave = std::dynamic_pointer_cast<WaveTrack>(snapshot)) {
                waves.push_back(std::move(wave));
            } else if (auto midi = std::dynamic_pointer_cast<MidiTrack>(snapshot)) {
                options.midiTracks.push_back(std::move(midi));
            }
        }
        if (options.stopTime <= options.startTime) {
            return muse::make_ret(muse::Ret::Code::NotSupported);
        }
        auto source = AudioRenderSource::Create(project, waves, options);
        if (!source) {
            throw std::runtime_error("The audio renderer is not available.");
        }
        auto rendered = WaveTrackFactory::Get(project).Create(2, floatSample, options.sampleRate);
        rendered->SetName(tracks.MakeUniqueTrackName(wxString::FromUTF8(muse::trc("trackedit", "Mix"))));
        auto clip = rendered->CreateClip(options.startTime, rendered->GetName());
        interactive()->showProgress(muse::trc("trackedit", "Mix and Render"), progress);
        progress.start();
        QElapsedTimer events;
        events.start();
        while (!progress.isCanceled()) {
            const auto count = source->Process();
            if (!count) {
                break;
            }
            constSamplePtr channels[] { source->GetBuffer(0), source->GetBuffer(1) };
            clip->Append(channels, floatSample, count, 1, floatSample);
            const double fraction = (source->MixGetCurrentTime() - options.startTime) / (options.stopTime - options.startTime);
            progress.progress(static_cast<int64_t>(std::clamp(fraction, 0.0, 1.0) * 1000), 1000);
            if (events.elapsed() >= 20) {
                QCoreApplication::processEvents();
                events.restart();
            }
            if (globalContext()->currentProject() != owner || owner->au3ProjectPtr() != reinterpret_cast<uintptr_t>(&project)
                || globalContext()->isRecording()) {
                result = muse::make_ret(muse::Ret::Code::Cancel);
                return result;
            }
        }
        if (progress.isCanceled()) {
            return muse::make_ret(muse::Ret::Code::Cancel);
        }
        clip->Flush();
        rendered->InsertInterval(clip, true);
        // Progress pumps window events. Never commit a mix over an edited,
        // replaced or closed document, or while another project owns audio.
        if (globalContext()->currentProject() != owner || owner->au3ProjectPtr() != reinterpret_cast<uintptr_t>(&project)
            || globalContext()->isRecording()
            || documentState(tracks) != before || !stopAudio()) {
            result = muse::make_ret(muse::Ret::Code::Cancel);
            return result;
        }
        source.reset(); // Dispose native processors before publishing document changes.
        tracks.Add(rendered);
        globalContext()->currentTrackeditProject()->reload();
        historyService->pushHistoryState(muse::trc("trackedit", "Mixed and rendered to a new track"),
                                         muse::trc("trackedit", "Mix and Render"));
    } catch (const std::exception& exception) {
        result = muse::make_ret(muse::Ret::Code::UnknownError, exception.what());
    } catch (...) {
        result = muse::make_ret(muse::Ret::Code::UnknownError);
    }
    return result;
}
