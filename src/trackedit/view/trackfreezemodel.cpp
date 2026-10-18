/*
* Audacity: A Digital Audio Editor
*/
#include "trackfreezemodel.h"

#include <cmath>
#include <QElapsedTimer>
#include <QRegularExpression>
#include <set>
#include "au3-mixer/AudioRenderSource.h"
#include "au3-midi-track/MidiTrack.h"
#include "au3-playable-track/TrackRouting.h"
#include "au3-playable-track/TrackAutomation.h"
#include "au3-project-rate/ProjectRate.h"
#include "au3-realtime-effects/RealtimeEffectList.h"
#include "au3-wave-track/TrackFreeze.h"
#include "au3-wave-track/WaveTrack.h"
#include "au3-wave-track/WaveClip.h"
#include "au3-wave-track/Sequence.h"
#include "au3-xml/XMLFileReader.h"
#include "au3-xml/XMLWriter.h"
#include "au3wrap/internal/domaccessor.h"
#include "au3wrap/internal/domconverter.h"
#include "project/iaudacityproject.h"
#include "trackedit/itrackeditproject.h"
#include "global/translation.h"

using namespace au::trackedit;

namespace {
wxString sourceXml(const ::Track& source)
{
    XMLStringWriter writer;
    writer.StartTag("frozen-source");
    source.WriteXML(writer);
    writer.EndTag("frozen-source");
    return writer;
}

wxString readableSourceXml(const wxString& source)
{
    // Early MIDI freezes could contain the UI spectrogram gain followed by
    // MIDI volume under the same XML name. Repair only that known duplicate
    // before parsing; the retained source otherwise stays unchanged.
    auto text = QString::fromStdWString(source.ToStdWstring());
    const auto header = QRegularExpression(QStringLiteral("<miditrack\\b[^>]*>")).match(text);
    if (!header.hasMatch()) {
        return source;
    }
    auto attributes = header.captured();
    if (attributes.contains(QRegularExpression(QStringLiteral("\\smidigain\\s*=")))) {
        return source;
    }
    auto gains = QRegularExpression(QStringLiteral("\\s(gain)\\s*=\\s*\"[^\"]*\"")).globalMatch(attributes);
    QRegularExpressionMatch last;
    int count = 0;
    while (gains.hasNext()) {
        last = gains.next();
        ++count;
    }
    if (count != 2) {
        return source;
    }
    attributes.replace(last.capturedStart(1), last.capturedLength(1), QStringLiteral("midigain"));
    text.replace(header.capturedStart(), header.capturedLength(), attributes);
    return wxString(text.toStdWString());
}

class SourceReader final : public XMLTagHandler
{
public:
    explicit SourceReader(AudacityProject& project)
        : tracks(::TrackList::Create(&project)), factory(WaveTrackFactory::Get(project)) {}

    bool HandleXMLTag(const std::string_view& tag, const AttributesList&) override { return tag == "frozen-source"; }
    XMLTagHandler* HandleXMLChild(const std::string_view& tag) override
    {
        if (tag == "wavetrack") {
            return tracks->Add(factory.Create());
        }
        if (tag == "miditrack") {
            return tracks->Add(MidiTrack::Create());
        }
        return nullptr;
    }

    ::TrackListHolder tracks;
    WaveTrackFactory& factory;
};

void copyMixControls(const PlayableTrack& source, PlayableTrack& target)
{
    const auto* wave = dynamic_cast<const WaveTrack*>(&source);
    const auto* midi = dynamic_cast<const MidiTrack*>(&source);
    const double volume = wave ? wave->GetVolume() : midi->GetGain();
    const double pan = wave ? wave->GetPan() : midi->GetPan();
    if (auto* output = dynamic_cast<WaveTrack*>(&target)) {
        output->SetVolume(volume);
        output->SetPan(pan);
    } else if (auto* output = dynamic_cast<MidiTrack*>(&target)) {
        output->SetGain(volume);
        output->SetPan(pan);
    }
    target.SetName(source.GetName());
    target.SetMute(source.GetMute());
    target.SetSolo(source.GetSolo());
    target.SetSelected(source.IsSelected());
    TrackRouting::Get(target) = TrackRouting::Get(source);
    for (const auto* id : { "volume", "pan" }) {
        auto& data = TrackAutomation::Get(target);
        const auto* lane = TrackAutomation::Get(source).Find(id);
        if (lane) {
            data.SetLane(*lane);
        } else {
            data.RemoveLane(id);
        }
    }
}
}

struct TrackFreezeModel::Job {
    std::shared_ptr<project::IAudacityProject> owner;
    std::shared_ptr<PlayableTrack> original;
    std::shared_ptr<PlayableTrack> snapshot;
    std::shared_ptr<WaveTrack> rendered;
    std::unique_ptr<AudioRenderSource> source;
    std::shared_ptr<WaveClip> clip;
    QElapsedTimer notification;
    double progress = 0.0;
    double end = 0.0;
    QString error;
};

TrackFreezeModel::TrackFreezeModel(QObject* parent)
    : QObject(parent), muse::Contextable(muse::iocCtxForQmlObject(this))
{
    connect(&m_timer, &QTimer::timeout, this, &TrackFreezeModel::finish);
}

TrackFreezeModel::TrackFreezeModel(const muse::modularity::ContextPtr& ctx, QObject* parent)
    : QObject(parent), muse::Contextable(ctx)
{
    connect(&m_timer, &QTimer::timeout, this, &TrackFreezeModel::finish);
}

TrackFreezeModel::~TrackFreezeModel() = default;

void TrackFreezeModel::init()
{
    if (m_initialised) {
        return;
    }
    m_initialised = true;
    globalContext()->currentTrackeditProjectChanged().onNotify(this, [this] { cancel(); emit dataChanged(); });
    globalContext()->isRecordingChanged().onNotify(this, [this] { emit dataChanged(); });
    history()->historyChanged().onReceive(this, [this](auto) { emit dataChanged(); });
}

void TrackFreezeModel::setTrackId(qlonglong id)
{
    if (m_trackId != id) {
        cancel();
        m_trackId = id;
        m_error.clear();
        emit dataChanged();
    }
}

PlayableTrack* TrackFreezeModel::track() const
{
    const auto project = globalContext()->currentProject();
    if (!project) {
        return nullptr;
    }
    auto& au3 = *reinterpret_cast<AudacityProject*>(project->au3ProjectPtr());
    return dynamic_cast<PlayableTrack*>(au::au3::DomAccessor::findTrack(au3, ::TrackId(m_trackId)));
}

bool TrackFreezeModel::frozen() const
{
    const auto* wave = dynamic_cast<const WaveTrack*>(track());
    return wave && !TrackFreeze::Get(*wave).sourceXml.empty();
}

bool TrackFreezeModel::canFreeze() const
{
    const auto* source = track();
    return source && !busy() && !frozen() && !globalContext()->isRecording()
           && TrackRouting::Get(*source).busId.empty() && source->GetEndTime() > 0.0;
}

bool TrackFreezeModel::busy() const { return static_cast<bool>(m_job); }
double TrackFreezeModel::progress() const { return m_job ? m_job->progress : 0.0; }

bool TrackFreezeModel::prepareEdit()
{
    if (!track() || globalContext()->isRecording()) {
        return false;
    }
    auto& project = *reinterpret_cast<AudacityProject*>(globalContext()->currentProject()->au3ProjectPtr());
    const auto stream = audioEngine()->currentStream();
    if (stream) {
        if (stream->ownerProject != &project || !audioEngine()->canStopAudioStream(project)) {
            m_error = muse::qtrc("freeze", "Stop audio in the other project first.");
            emit dataChanged();
            return false;
        }
        audioEngine()->stopStream();
    }
    m_error.clear();
    return true;
}

void TrackFreezeModel::freeze(double tailSeconds)
{
    if (!canFreeze() || !std::isfinite(tailSeconds) || tailSeconds < 0.0 || tailSeconds > 30.0) {
        return;
    }
    viewRequests()->requestFinishInstrumentEditing();
    if (!prepareEdit()) {
        return;
    }
    auto job = std::make_unique<Job>();
    job->owner = globalContext()->currentProject();
    job->original = std::static_pointer_cast<PlayableTrack>(track()->shared_from_this());
    job->snapshot = std::static_pointer_cast<PlayableTrack>(track()->Duplicate());
    RealtimeEffectList::Get(*job->snapshot).CloneStates();
    auto& project = *reinterpret_cast<AudacityProject*>(job->owner->au3ProjectPtr());
    const double rate = ProjectRate::Get(project).GetRate();
    job->rendered = WaveTrackFactory::Get(project).Create(2, floatSample, rate);
    copyMixControls(*track(), *job->rendered);
    auto& frozen = TrackFreeze::Get(*job->rendered);
    frozen.sourceXml = sourceXml(*track());
    if (const auto* wave = dynamic_cast<const WaveTrack*>(track())) {
        std::set<SampleBlockID> seen;
        for (const auto& clip : wave->Intervals()) {
            for (const auto& channel : clip->Channels()) {
                for (const auto& block : *channel->GetSequenceBlockArray()) {
                    if (seen.insert(block.sb->GetBlockID()).second) {
                        frozen.blocks.push_back(block.sb);
                    }
                }
            }
        }
    }
    auto* wave = dynamic_cast<WaveTrack*>(job->snapshot.get());
    auto* midi = dynamic_cast<MidiTrack*>(job->snapshot.get());
    if (wave) {
        wave->SetVolume(1.0);
        wave->SetPan(0.0);
    } else {
        midi->SetGain(1.0);
        midi->SetPan(0.0);
    }
    TrackAutomation::Get(*job->snapshot).RemoveLane("volume");
    TrackAutomation::Get(*job->snapshot).RemoveLane("pan");
    job->snapshot->SetMute(false);
    job->snapshot->SetSolo(false);
    job->end = track()->GetEndTime() + tailSeconds;
    AudioRenderSource::Options options;
    options.stopTime = job->end;
    options.sampleRate = rate;
    options.trackRouting = false;
    options.format = floatSample;
    if (midi) {
        options.midiTracks.push_back(std::static_pointer_cast<MidiTrack>(job->snapshot));
    }
    try {
        job->source = AudioRenderSource::Create(project, wave ? AudioRenderSource::Tracks {
            std::static_pointer_cast<WaveTrack>(job->snapshot) } : AudioRenderSource::Tracks {}, options);
        if (!job->source) {
            m_error = muse::qtrc("freeze", "The audio renderer is not available.");
            emit dataChanged();
            return;
        }
    } catch (const std::exception& exception) {
        m_error = QString::fromUtf8(exception.what());
        emit dataChanged();
        return;
    }
    job->clip = job->rendered->CreateClip(0.0, job->rendered->GetName());
    job->notification.start();
    m_job = std::move(job);
    m_timer.start(0);
    emit dataChanged();
}

void TrackFreezeModel::cancel()
{
    m_timer.stop();
    m_job.reset();
    emit dataChanged();
}

void TrackFreezeModel::finish()
{
    if (!m_job) {
        return;
    }
    auto& data = *m_job;
    if (globalContext()->currentProject() != data.owner) {
        cancel();
        return;
    }
    try {
        // One audio block per event keeps cancellation and window input available.
        const auto count = data.source->Process();
        if (count > 0) {
            constSamplePtr channels[] { data.source->GetBuffer(0), data.source->GetBuffer(1) };
            data.clip->Append(channels, floatSample, count, 1, floatSample);
            data.progress = data.source->MixGetCurrentTime() / data.end;
            if (data.notification.elapsed() >= 50) {
                data.notification.restart();
                emit dataChanged();
            }
            return;
        }
        data.clip->Flush();
        data.rendered->InsertInterval(data.clip, true);
    } catch (const std::exception& exception) {
        data.error = QString::fromUtf8(exception.what());
    } catch (...) {
        data.error = muse::qtrc("freeze", "Cannot render this track.");
    }
    m_timer.stop();
    auto job = std::move(m_job);
    m_error = job->error;
    if (m_error.isEmpty()) {
        if (track() != job->original.get() || sourceXml(*track()) != TrackFreeze::Get(*job->rendered).sourceXml) {
            m_error = muse::qtrc("freeze", "The track changed during rendering. Freeze it again.");
        } else if (prepareEdit()) {
            auto& project = *reinterpret_cast<AudacityProject*>(job->owner->au3ProjectPtr());
            auto replacement = ::TrackList::Temporary(nullptr, job->rendered);
            ::TrackList::Get(project).ReplaceOne(*track(), std::move(*replacement));
            globalContext()->currentTrackeditProject()->reload();
            history()->pushHistoryState(muse::trc("freeze", "Froze track"), muse::trc("freeze", "Freeze"));
        }
    }
    emit dataChanged();
}

void TrackFreezeModel::unfreeze()
{
    if (busy() || !frozen() || !prepareEdit()) {
        return;
    }
    const auto* frozen = static_cast<const WaveTrack*>(track());
    auto& project = *reinterpret_cast<AudacityProject*>(globalContext()->currentProject()->au3ProjectPtr());
    SourceReader reader(project);
    XMLFileReader parser;
    if (!parser.ParseString(&reader, readableSourceXml(TrackFreeze::Get(*frozen).sourceXml)) || reader.tracks->empty()) {
        m_error = muse::qtrc("freeze", "Cannot restore the source track.");
        emit dataChanged();
        return;
    }
    for (auto* item : *reader.tracks) {
        item->LinkConsistencyFix();
    }
    auto restored = reader.tracks->DetachFirst();
    auto* playable = dynamic_cast<PlayableTrack*>(restored.get());
    const auto* restoredWave = dynamic_cast<const WaveTrack*>(restored.get());
    if (!playable || !reader.tracks->empty() || (restoredWave && !TrackFreeze::Get(*restoredWave).sourceXml.empty())) {
        m_error = muse::qtrc("freeze", "The source track data is invalid.");
        emit dataChanged();
        return;
    }
    copyMixControls(*frozen, *playable);
    auto replacement = ::TrackList::Temporary(nullptr, restored);
    ::TrackList::Get(project).ReplaceOne(*track(), std::move(*replacement));
    globalContext()->currentTrackeditProject()->reload();
    history()->pushHistoryState(muse::trc("freeze", "Unfroze track"), muse::trc("freeze", "Unfreeze"));
    emit dataChanged();
}
