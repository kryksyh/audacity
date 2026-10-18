/*
 * Audacity: A Digital Audio Editor
 */
#include "projectapi.h"

#include <algorithm>
#include <cmath>
#include <limits>

#include <QJSEngine>
#include <QCryptographicHash>

#include "au3-project-rate/ProjectRate.h"
#include "au3-audio-devices/AudioIOBase.h"
#include "au3-project/Project.h"
#include "au3-project-file-io/ProjectFileIO.h"
#include "au3-midi-track/MidiTrack.h"
#include "au3-wave-track/WaveTrack.h"
#include "au3-wave-track/TrackFreeze.h"
#include "au3-xml/XMLWriter.h"
#include "log.h"

#include "projectedit.h"

using namespace au::trackedit;
using namespace au::trackedit::api;

namespace {
constexpr double INVALID_TIME = std::numeric_limits<double>::quiet_NaN();
}

ProjectApi::ProjectApi(muse::api::IApiEngine* e)
    : ApiObject(e)
{
}

QJSValue ProjectApi::fileInfo() const
{
    auto result = engine()->newObject();
    const auto current = globalContext()->currentProject();
    if (!current) {
        return result;
    }
    result.setProperty("path", current->path().toQString());
    result.setProperty("newlyCreated", current->isNewlyCreated());
    result.setProperty("modified", current->needSave().val);
    result.setProperty("needsAutoSave", current->needAutoSave());
    if (const auto* core = project()) {
        result.setProperty("databasePath", QString::fromUtf8(ProjectFileIO::Get(*core).GetFileName().ToUTF8().data()));
    }
    return result;
}

AudacityProject* ProjectApi::project() const
{
    const auto current = globalContext() ? globalContext()->currentProject() : nullptr;
    return current ? reinterpret_cast<AudacityProject*>(current->au3ProjectPtr()) : nullptr;
}

QObject* ProjectApi::selection()
{
    auto* current = project();
    if (!current || !selectionController()) {
        return nullptr;
    }
    auto* result = makeCurrentProjectSelection(
        current, selectionController()->dataSelectedStartTime(), selectionController()->dataSelectedEndTime(),
        selectionController()->selectedClips(), nullptr);
    QJSEngine::setObjectOwnership(result, QJSEngine::JavaScriptOwnership);
    return result;
}

QJSValue ProjectApi::playbackInfo() const
{
    auto result = engine()->newObject();
    auto* current = project();
    const auto state = globalContext() ? globalContext()->playbackState() : nullptr;
    const auto status = state ? state->playbackStatus() : playback::PlaybackStatus::Stopped;
    result.setProperty("projectOpen", current != nullptr);
    result.setProperty("status", status == playback::PlaybackStatus::Running ? "running"
                       : status == playback::PlaybackStatus::Paused ? "paused" : "stopped");
    result.setProperty("position", state ? static_cast<double>(state->playbackPosition()) : 0.0);
    result.setProperty("busy", state && state->isBusy());
    result.setProperty("reachedEnd", state && state->reachedEnd());
    result.setProperty("allowed", current && playbackController() && playbackController()->isPlayAllowed());
    if (recordController()) {
        result.setProperty("micMetering", recordController()->isMicMeteringOn());
        result.setProperty("inputMonitoring", recordController()->isInputMonitoringOn());
    }
    const auto stream = audioEngine() ? audioEngine()->currentStream() : std::nullopt;
    const bool owned = stream && current && stream->ownerProject == current;
    // Monitoring has no transport token; query its device activity directly.
    const bool active = owned && (stream->kind == audio::AudioStreamKind::Monitoring
                                  ? AudioIOBase::Get()->IsStreamActive() : audioEngine()->isStreamActive(*current));
    result.setProperty("streamPresent", stream.has_value());
    result.setProperty("streamOwned", owned);
    result.setProperty("streamActive", active);
    result.setProperty("streamKind", !stream ? "none" : stream->kind == audio::AudioStreamKind::Playback ? "playback"
                       : stream->kind == audio::AudioStreamKind::Monitoring ? "monitoring" : "recording");
    result.setProperty("streamRate", stream ? stream->sampleRate : 0.0);
    result.setProperty("streamTime", active ? audioEngine()->streamTime() : 0.0);
    result.setProperty("lastError", audioEngine() ? audioEngine()->lastErrorString().toQString() : QString());
    if (current && playbackController()) {
        const auto loop = playbackController()->loopRegion();
        result.setProperty("loopStart", static_cast<double>(loop.start));
        result.setProperty("loopEnd", static_cast<double>(loop.end));
        result.setProperty("loopActive", playbackController()->isLoopRegionActive());
        result.setProperty("lastSeek", static_cast<double>(playbackController()->lastPlaybackSeekTime()));
    }
    return result;
}

QJSValue ProjectApi::musicalContext() const
{
    auto result = engine()->newObject();
    const auto current = trackeditProject();
    if (!current) {
        return result;
    }
    const auto signature = current->timeSignature();
    result.setProperty("tempo", signature.tempo);
    result.setProperty("numerator", signature.upper);
    result.setProperty("denominator", signature.lower);
    return result;
}

ITrackeditProjectPtr ProjectApi::trackeditProject() const
{
    return globalContext() ? globalContext()->currentTrackeditProject() : nullptr;
}

Clips ProjectApi::sortedClips(int trackIndex) const
{
    auto prj = trackeditProject();
    if (!prj) {
        return {};
    }
    auto ids = prj->trackIdList();
    if (trackIndex < 0 || trackIndex >= static_cast<int>(ids.size())) {
        LOGE() << "trackIndex out of range: " << trackIndex << " (have " << ids.size() << " tracks)";
        return {};
    }
    auto clipList = prj->clipList(ids[trackIndex]);
    Clips clips(clipList.begin(), clipList.end());
    std::sort(clips.begin(), clips.end(), [](const Clip& a, const Clip& b) {
        return a.startTime < b.startTime;
    });
    return clips;
}

int ProjectApi::trackCount() const
{
    auto prj = trackeditProject();
    return prj ? static_cast<int>(prj->trackIdList().size()) : 0;
}

QJSValue ProjectApi::trackInfo(int trackIndex) const
{
    const auto current = trackeditProject();
    if (!current || !project()) {
        return QJSValue(QJSValue::NullValue);
    }
    const auto ids = current->trackIdList();
    if (trackIndex < 0 || trackIndex >= static_cast<int>(ids.size())) {
        return QJSValue(QJSValue::NullValue);
    }
    const auto* track = ::TrackList::Get(*project()).FindById(::TrackId(ids[trackIndex]));
    if (!track) {
        return QJSValue(QJSValue::NullValue);
    }
    QJSValue info = engine()->newObject();
    info.setProperty("id", static_cast<double>(static_cast<int64_t>(track->GetId())));
    info.setProperty("name", QString::fromStdWString(track->GetName().ToStdWstring()));
    const auto* wave = dynamic_cast<const WaveTrack*>(track);
    info.setProperty("type", dynamic_cast<const MidiTrack*>(track) ? "midi" : wave ? "wave" : "label");
    info.setProperty("frozen", wave && !TrackFreeze::Get(*wave).sourceXml.empty());
    XMLStringWriter writer;
    track->WriteXML(writer);
    info.setProperty("xml", QString::fromStdWString(writer.ToStdWstring()));
    return info;
}

QJSValue ProjectApi::waveformStatistics(int trackIndex, double start, double end) const
{
    const auto current = trackeditProject();
    if (!current || !project() || !std::isfinite(start) || !std::isfinite(end) || end < start || end - start > 60.0) {
        return QJSValue(QJSValue::NullValue);
    }
    const auto ids = current->trackIdList();
    if (trackIndex < 0 || trackIndex >= static_cast<int>(ids.size())) {
        return QJSValue(QJSValue::NullValue);
    }
    const auto* track = dynamic_cast<const WaveTrack*>(::TrackList::Get(*project()).FindById(::TrackId(ids[trackIndex])));
    if (!track || !std::isfinite(track->GetRate()) || track->GetRate() <= 0.0 || track->NChannels() == 0) {
        return QJSValue(QJSValue::NullValue);
    }
    const auto limit = static_cast<long double>(std::numeric_limits<int64_t>::max()) / track->GetRate();
    if (std::abs(start) >= limit || std::abs(end) >= limit) {
        return QJSValue(QJSValue::NullValue);
    }
    const auto first = track->TimeToLongSamples(start), last = track->TimeToLongSamples(end);
    const auto frames = (last - first).as_long_long();
    std::vector<std::vector<float> > buffers(track->NChannels(), std::vector<float>(4096));
    std::vector<float*> pointers;
    for (auto& buffer : buffers) {
        pointers.push_back(buffer.data());
    }
    double peak = 0.0, sum = 0.0;
    QCryptographicHash hash(QCryptographicHash::Sha256);
    for (int64_t offset = 0; offset < frames;) {
        const auto count = static_cast<size_t>(std::min<int64_t>(4096, frames - offset));
        if (!track->GetFloats(0, buffers.size(), pointers.data(), first + sampleCount(offset), count,
                              false, FillFormat::fillZero, false)) {
            return QJSValue(QJSValue::NullValue);
        }
        for (const auto& buffer : buffers) {
            hash.addData(QByteArrayView(reinterpret_cast<const char*>(buffer.data()), static_cast<qsizetype>(count * sizeof(float))));
            for (size_t sample = 0; sample < count; ++sample) {
                const auto value = static_cast<double>(buffer[sample]);
                if (!std::isfinite(value)) {
                    return QJSValue(QJSValue::NullValue);
                }
                peak = std::max(peak, std::abs(value));
                sum += value * value;
            }
        }
        offset += static_cast<int64_t>(count);
    }
    auto result = engine()->newObject();
    result.setProperty("sampleRate", track->GetRate());
    result.setProperty("channelCount", static_cast<int>(buffers.size()));
    result.setProperty("sampleCount", static_cast<double>(frames));
    result.setProperty("peak", peak);
    result.setProperty("rms", frames > 0 ? std::sqrt(sum / (static_cast<double>(frames) * buffers.size())) : 0.0);
    result.setProperty("sha256", QString::fromLatin1(hash.result().toHex()));
    return result;
}

int ProjectApi::clipCount(int trackIndex) const
{
    return static_cast<int>(sortedClips(trackIndex).size());
}

QJSValue ProjectApi::clipsOnTrack(int trackIndex) const
{
    const auto clips = sortedClips(trackIndex);
    QJSValue arr = engine()->newArray(clips.size());
    for (size_t i = 0; i < clips.size(); ++i) {
        QJSValue obj = engine()->newObject();
        obj.setProperty("start", static_cast<double>(clips[i].startTime));
        obj.setProperty("end", static_cast<double>(clips[i].endTime));
        arr.setProperty(static_cast<quint32>(i), obj);
    }
    return arr;
}

double ProjectApi::totalTime() const
{
    auto prj = trackeditProject();
    if (!prj) {
        return INVALID_TIME;
    }
    return static_cast<double>(prj->totalTime());
}

double ProjectApi::defaultSampleRate() const
{
    const auto* current = project();
    return current ? ProjectRate::Get(*current).GetRate() : INVALID_TIME;
}

QObject* ProjectApi::beginEdit(const QString& name, QObject* session)
{
    if (projectApiScopeActive()) {
        auto* editSession = dynamic_cast<ProjectEditSession*>(session);
        if (!editSession || editSession != currentProjectEditSession()) {
            throwProjectError(this, QStringLiteral("Project edits are not available in this callback"));
            return nullptr;
        }
        return editSession->beginEdit(this);
    }

    if (session) {
        throwProjectError(this, QStringLiteral("The project edit session is no longer active"));
        return nullptr;
    }

    auto* current = project();
    if (!current || !selectionController()) {
        throwProjectError(this, QStringLiteral("No project is active"));
        return nullptr;
    }
    if (name.isEmpty()) {
        throwProjectError(this, QStringLiteral("An undo description is required"));
        return nullptr;
    }
    if (recordController() && recordController()->isRecording()) {
        throwProjectError(this, QStringLiteral("Project edits are not available while recording"));
        return nullptr;
    }
    if (playbackController()) {
        playbackController()->stop();
    }
    const auto appContext = globalContext();
    const auto selection = selectionController();
    const auto target = appContext->currentProject();
    return beginRootProjectEdit(
        current, selectionController()->dataSelectedStartTime(), selectionController()->dataSelectedEndTime(),
        selectionController()->selectedClips(), name, projectHistory(),
        [appContext, target, current] {
        const auto active = appContext->currentProject();
        return active == target && active && reinterpret_cast<AudacityProject*>(active->au3ProjectPtr()) == current;
    },
        [selection](double start, double end, const std::vector<int64_t>& tracks) {
        selection->resetSelectedClips();
        selection->resetSelectedLabels();
        selection->setSelectedTracks(tracks, true);
        selection->setDataSelectedStartTime(start, true);
        selection->setDataSelectedEndTime(end, true);
    },
        this);
}
