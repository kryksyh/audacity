/*
* Audacity: A Digital Audio Editor
*/
#include "trackroutingmodel.h"

#include <algorithm>
#include <cmath>
#include <QUuid>
#include "au3-playable-track/PlayableTrack.h"
#include "au3-playable-track/TrackRouting.h"
#include "au3-project-rate/ProjectRate.h"
#include "au3-wave-track/WaveTrack.h"
#include "au3wrap/internal/domaccessor.h"
#include "au3wrap/internal/domconverter.h"
#include "au3wrap/internal/wxtypes_convert.h"
#include "project/iaudacityproject.h"
#include "trackedit/itrackeditproject.h"
#include "global/translation.h"

using namespace au::trackedit;

TrackRoutingModel::TrackRoutingModel(QObject* parent)
    : QObject(parent), muse::Contextable(muse::iocCtxForQmlObject(this))
{
}

TrackRoutingModel::TrackRoutingModel(const muse::modularity::ContextPtr& ctx, QObject* parent)
    : QObject(parent), muse::Contextable(ctx)
{
}

void TrackRoutingModel::init()
{
    if (m_initialised) {
        return;
    }
    m_initialised = true;
    globalContext()->currentTrackeditProjectChanged().onNotify(this, [this] { emit dataChanged(); });
    globalContext()->isRecordingChanged().onNotify(this, [this] { emit dataChanged(); });
    history()->historyChanged().onReceive(this, [this](auto) { emit dataChanged(); });
}

void TrackRoutingModel::initPanel()
{
    init();
    viewRequests()->routingRequested().onReceive(this, [this](TrackId id) {
        setTrackId(id);
        emit editorRequested();
    });
}

void TrackRoutingModel::openEditor()
{
    viewRequests()->requestRouting(m_trackId);
}

void TrackRoutingModel::setTrackId(qlonglong id)
{
    m_trackId = id;
    m_error.clear();
    emit dataChanged();
}

PlayableTrack* TrackRoutingModel::track() const
{
    const auto project = globalContext()->currentProject();
    if (!project) {
        return nullptr;
    }
    auto& au3 = *reinterpret_cast<AudacityProject*>(project->au3ProjectPtr());
    return dynamic_cast<PlayableTrack*>(au::au3::DomAccessor::findTrack(au3, ::TrackId(m_trackId)));
}

QString TrackRoutingModel::title() const
{
    const auto* source = track();
    return source ? au::au3::wxToString(source->GetName()).toQString() : QString();
}

QVariantList TrackRoutingModel::buses() const
{
    QVariantList result { QVariantMap { { "title", muse::qtrc("routing", "Master") }, { "id", QString() } } };
    const auto project = globalContext()->currentProject();
    if (!project) {
        return result;
    }
    auto& au3 = *reinterpret_cast<AudacityProject*>(project->au3ProjectPtr());
    for (const auto* bus : ::TrackList::Get(au3).Any<const WaveTrack>()) {
        const auto& route = TrackRouting::Get(*bus);
        if (bus != track() && !route.busId.empty()) {
            result.push_back(QVariantMap { { "title", au::au3::wxToString(bus->GetName()).toQString() },
                                 { "id", QString::fromStdString(route.busId) } });
        }
    }
    return result;
}

QVariantList TrackRoutingModel::sends() const
{
    QVariantList result;
    const auto* source = track();
    if (!source) {
        return result;
    }
    const auto targets = buses();
    const auto& sends = TrackRouting::Get(*source).sends;
    for (int i = 1; i < targets.size(); ++i) {
        auto row = targets[i].toMap();
        const auto id = row.value("id").toString().toStdString();
        const auto send = std::find_if(sends.begin(), sends.end(), [&](const auto& item) { return item.bus == id; });
        row.insert("index", i);
        row.insert("enabled", send != sends.end());
        row.insert("gain", send == sends.end() ? 1.0 : send->gain);
        row.insert("preFader", send != sends.end() && send->preFader);
        result.push_back(row);
    }
    return result;
}

int TrackRoutingModel::outputIndex() const
{
    const auto* source = track();
    if (source) {
        const auto id = QString::fromStdString(TrackRouting::Get(*source).output);
        const auto targets = buses();
        for (int i = 0; i < targets.size(); ++i) {
            if (targets[i].toMap().value("id").toString() == id) {
                return i;
            }
        }
    }
    return 0;
}

bool TrackRoutingModel::editable() const
{
    return track() && !globalContext()->isRecording();
}

bool TrackRoutingModel::prepareEdit()
{
    if (!editable()) {
        return false;
    }
    auto& project = *reinterpret_cast<AudacityProject*>(globalContext()->currentProject()->au3ProjectPtr());
    const auto stream = audioEngine()->currentStream();
    if (stream) {
        if (stream->ownerProject != &project || !audioEngine()->canStopAudioStream(project)) {
            m_error = muse::qtrc("routing", "Stop audio in the other project first.");
            emit dataChanged();
            return false;
        }
        audioEngine()->stopStream();
    }
    m_error.clear();
    return true;
}

void TrackRoutingModel::apply(TrackRouting routing)
{
    if (!track()) {
        return;
    }
    auto& project = *reinterpret_cast<AudacityProject*>(globalContext()->currentProject()->au3ProjectPtr());
    if (!TrackRouting::Validate(::TrackList::Get(project), track(), &routing)) {
        m_error = muse::qtrc("routing", "This route would create a feedback loop.");
        emit dataChanged();
        return;
    }
    if (!prepareEdit()) {
        return;
    }
    TrackRouting::Get(*track()) = std::move(routing);
    globalContext()->currentTrackeditProject()->notifyAboutTrackChanged(au::au3::DomConverter::track(track()));
    history()->pushHistoryState(muse::trc("routing", "Changed track routing"), muse::trc("routing", "Routing"));
    emit dataChanged();
}

void TrackRoutingModel::setOutput(int index)
{
    const auto targets = buses();
    if (!editable() || index < 0 || index >= targets.size()) {
        return;
    }
    auto route = TrackRouting::Get(*track());
    const auto id = targets[index].toMap().value("id").toString().toStdString();
    if (route.output != id) {
        route.output = id;
        apply(std::move(route));
    }
}

void TrackRoutingModel::setSend(int busIndex, bool enabled, double gain, bool preFader)
{
    const auto targets = buses();
    if (!editable() || busIndex <= 0 || busIndex >= targets.size() || !std::isfinite(gain) || gain < 0.0 || gain > 4.0) {
        return;
    }
    auto route = TrackRouting::Get(*track());
    const auto id = targets[busIndex].toMap().value("id").toString().toStdString();
    const auto previous = route.sends;
    const auto existing = std::find_if(route.sends.begin(), route.sends.end(), [&](const auto& send) { return send.bus == id; });
    if (!enabled) {
        std::erase_if(route.sends, [&](const auto& send) { return send.bus == id; });
    } else if (existing == route.sends.end()) {
        route.sends.push_back({ id, gain, preFader });
    } else {
        *existing = { id, gain, preFader };
    }
    if (route.sends != previous) {
        apply(std::move(route));
    }
}

void TrackRoutingModel::addBus()
{
    if (!prepareEdit()) {
        return;
    }
    auto& project = *reinterpret_cast<AudacityProject*>(globalContext()->currentProject()->au3ProjectPtr());
    const auto bus = WaveTrackFactory::Get(project).Create(2, floatSample, ProjectRate::Get(project).GetRate());
    TrackRouting::Get(*bus).busId = QUuid::createUuid().toString(QUuid::WithoutBraces).toStdString();
    int number = 1;
    const auto prefix = muse::qtrc("routing", "Bus");
    QString name;
    bool exists = false;
    do {
        name = prefix + " " + QString::number(number++);
        exists = false;
        for (const auto* source : ::TrackList::Get(project)) {
            exists |= au::au3::wxToString(source->GetName()).toQString() == name;
        }
    } while (exists);
    bus->SetName(wxString::FromUTF8(name.toStdString()));
    ::TrackList::Get(project).Add(bus);
    globalContext()->currentTrackeditProject()->notifyAboutTrackAdded(au::au3::DomConverter::track(bus.get()));
    history()->pushHistoryState(muse::trc("routing", "Added bus"), muse::trc("routing", "Bus"));
    emit dataChanged();
}
