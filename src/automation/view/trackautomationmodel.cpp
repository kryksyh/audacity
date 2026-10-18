/*
* Audacity: A Digital Audio Editor
*/
#include "trackautomationmodel.h"

#include <algorithm>
#include <cmath>

#include "au3-midi-track/MidiTrack.h"
#include "au3-wave-track/WaveTrack.h"
#include "au3-playable-track/TrackAutomation.h"
#include "au3-realtime-effects/RealtimeEffectList.h"
#include "au3-realtime-effects/RealtimeEffectState.h"
#include "au3wrap/internal/domaccessor.h"
#include "au3wrap/internal/domconverter.h"
#include "au3wrap/internal/wxtypes_convert.h"
#include "project/iaudacityproject.h"
#include "trackedit/itrackeditproject.h"
#include "global/translation.h"

using namespace au::automation;

TrackAutomationModel::TrackAutomationModel(QObject* parent)
    : QObject(parent), muse::Contextable(muse::iocCtxForQmlObject(this))
{
}

TrackAutomationModel::TrackAutomationModel(const muse::modularity::ContextPtr& ctx, QObject* parent)
    : QObject(parent), muse::Contextable(ctx)
{
}

void TrackAutomationModel::init()
{
    if (m_initialised) {
        return;
    }
    m_initialised = true;
    globalContext()->currentTrackeditProjectChanged().onNotify(this, [this] { reload(); });
    globalContext()->isRecordingChanged().onNotify(this, [this] { emit dataChanged(); });
    history()->historyChanged().onReceive(this, [this](auto) { reload(); });
    reload();
}

void TrackAutomationModel::initPanel()
{
    init();
    viewRequests()->automationRequested().onReceive(this, [this](trackedit::TrackId id) {
        setTrackId(id);
        // Opening this dock again may follow edits in a floating effect
        // window that did not push a new project-history entry.
        reload();
        emit editorRequested();
    });
}

void TrackAutomationModel::openEditor()
{
    viewRequests()->requestAutomation(m_trackId);
}

PlayableTrack* TrackAutomationModel::track() const
{
    const auto project = globalContext()->currentProject();
    if (!project) {
        return nullptr;
    }
    auto& au3 = *reinterpret_cast<AudacityProject*>(project->au3ProjectPtr());
    return dynamic_cast<PlayableTrack*>(au::au3::DomAccessor::findTrack(au3, ::TrackId(m_trackId)));
}

void TrackAutomationModel::setTrackId(qlonglong id)
{
    if (m_trackId == id) {
        return;
    }
    m_trackId = id;
    m_targetIndex = 0;
    m_parameterIndex = 0;
    refreshParameters();
    emit dataChanged();
}

QString TrackAutomationModel::title() const
{
    return track() ? au::au3::wxToString(track()->GetName()).toQString() : QString();
}

QVariantList TrackAutomationModel::targets() const
{
    QVariantList result;
    const auto* source = track();
    if (!source) {
        return result;
    }
    result.push_back(QVariantMap { { "title", muse::qtrc("automation", "Track") }, { "effect", -1 } });
    if (dynamic_cast<const MidiTrack*>(source)) {
        result.push_back(QVariantMap { { "title", muse::qtrc("automation", "Instrument") }, { "effect", -2 } });
    }
    const auto& effects = RealtimeEffectList::Get(*source);
    for (size_t index = 0; index < effects.GetStatesCount(); ++index) {
        const auto* effect = effects.GetStateAt(index)->GetEffect();
        result.push_back(QVariantMap { { "title", effect ? QString::fromStdWString(effect->GetName().Translation().ToStdWstring())
                             : muse::qtrc("automation", "Unavailable effect") },
                             { "effect", static_cast<int>(index) } });
    }
    return result;
}

std::shared_ptr<RealtimeEffectState> TrackAutomationModel::effect() const
{
    const auto list = targets();
    if (!track() || m_targetIndex < 0 || m_targetIndex >= list.size()) {
        return {};
    }
    const int index = list[m_targetIndex].toMap().value("effect").toInt();
    return index >= 0 ? RealtimeEffectList::Get(*track()).GetStateAt(static_cast<size_t>(index)) : nullptr;
}

void TrackAutomationModel::setTargetIndex(int index)
{
    if (index != m_targetIndex && index >= 0 && index < targets().size()) {
        m_targetIndex = index;
        m_parameterIndex = 0;
        refreshParameters();
        emit dataChanged();
    }
}

void TrackAutomationModel::refreshParameters()
{
    setError({});
    m_parameters.clear();
    m_parameterOwner.clear();
    m_instrumentState.clear();
    m_parameterEffect.reset();
    m_effectSettingsCounter = 0;
    const auto* source = track();
    if (!source) {
        emit parametersChanged();
        return;
    }
    std::vector<audio::InstrumentParameter> parameters;
    const auto state = effect();
    if (state) {
        parameters = pluginAutomation()->parameters(*state);
        m_parameterOwner = au::au3::wxToString(state->GetID()).toQString();
        m_parameterEffect = state;
        m_effectSettingsCounter = state->GetAccess()->modificationCounter();
    } else if (m_targetIndex == 0) {
        const auto* wave = dynamic_cast<const WaveTrack*>(source);
        const auto* midi = dynamic_cast<const MidiTrack*>(source);
        const double volume = wave ? wave->GetVolume() : midi->GetGain();
        const double pan = wave ? wave->GetPan() : midi->GetPan();
        parameters = { { "volume", muse::qtrc("automation", "Volume").toStdString(), {}, volume / TrackAutomation::MaximumVolume },
            { "pan", muse::qtrc("automation", "Pan").toStdString(), {}, (pan + 1.0) / 2.0 } };
        m_parameterOwner = "track";
    } else {
        const auto* midi = dynamic_cast<const MidiTrack*>(source);
        if (midi) {
            m_parameterOwner = au::au3::wxToString(midi->GetInstrumentDescription()).toQString();
            m_instrumentState = midi->GetInstrumentState();
            auto& project = *reinterpret_cast<AudacityProject*>(globalContext()->currentProject()->au3ProjectPtr());
            const auto existing = instrumentPlayback()->existingInstrument(project, *midi);
            if (existing) {
                parameters = existing->parameters();
            } else {
                const auto editor = instrumentFactory()->create(
                    midi->GetInstrumentDescription().utf8_string(), m_instrumentState.utf8_string());
                if (editor.ret) {
                    parameters = editor.val->parameters();
                } else {
                    setError(QString::fromStdString(editor.ret.toString()));
                }
            }
        }
    }
    for (const auto& parameter : parameters) {
        m_parameters.push_back(QVariantMap { { "id", QString::fromStdString(parameter.id) },
                                   { "title", QString::fromStdString(parameter.name) }, { "value", parameter.value } });
    }
    if (m_parameterIndex >= m_parameters.size()) {
        m_parameterIndex = 0;
    }
    emit parametersChanged();
}

void TrackAutomationModel::setParameterIndex(int index)
{
    if (index != m_parameterIndex && index >= 0 && index < m_parameters.size()) {
        m_parameterIndex = index;
        emit dataChanged();
    }
}

std::string TrackAutomationModel::parameterId() const
{
    if (m_parameterIndex < 0 || m_parameterIndex >= m_parameters.size()) {
        return {};
    }
    const auto prefix = m_targetIndex > 0 && !effect() ? "instrument/" : "";
    return prefix + m_parameters[m_parameterIndex].toMap().value("id").toString().toStdString();
}

AutomationData* TrackAutomationModel::automation() const
{
    if (!track()) {
        return nullptr;
    }
    const auto state = effect();
    return state ? &state->Automation() : &TrackAutomation::Get(*track());
}

QVariantList TrackAutomationModel::points() const
{
    QVariantList result;
    const auto* data = automation();
    const auto* lane = data ? data->Find(parameterId()) : nullptr;
    if (lane) {
        for (const auto& point : lane->points) {
            result.push_back(QVariantMap { { "time", point.time }, { "value", point.value }, { "curve", point.curve } });
        }
    }
    return result;
}

double TrackAutomationModel::defaultValue() const
{
    const auto* source = track();
    if (source && m_targetIndex == 0) {
        const auto* wave = dynamic_cast<const WaveTrack*>(source);
        const auto* midi = dynamic_cast<const MidiTrack*>(source);
        return m_parameterIndex == 0 ? (wave ? wave->GetVolume() : midi->GetGain()) / TrackAutomation::MaximumVolume
               : ((wave ? wave->GetPan() : midi->GetPan()) + 1.0) / 2.0;
    }
    return m_parameterIndex >= 0 && m_parameterIndex < m_parameters.size()
           ? m_parameters[m_parameterIndex].toMap().value("value").toDouble() : 0.5;
}

double TrackAutomationModel::valueScale() const
{
    return m_targetIndex == 0 ? (m_parameterIndex == 0 ? TrackAutomation::MaximumVolume * 100.0 : 200.0) : 100.0;
}

double TrackAutomationModel::duration() const
{
    double end = track() ? track()->GetEndTime() : 0.0;
    for (const auto& point : points()) {
        end = std::max(end, point.toMap().value("time").toDouble());
    }
    return std::max(4.0, end);
}

bool TrackAutomationModel::laneEnabled() const
{
    const auto* data = automation();
    const auto* lane = data ? data->Find(parameterId()) : nullptr;
    return !lane || lane->enabled;
}

bool TrackAutomationModel::editable() const
{
    return track() && !globalContext()->isRecording() && !m_parameters.empty();
}

bool TrackAutomationModel::prepareEdit()
{
    if (!editable()) {
        return false;
    }
    auto& project = *reinterpret_cast<AudacityProject*>(globalContext()->currentProject()->au3ProjectPtr());
    const auto stream = audioEngine()->currentStream();
    if (stream) {
        if (stream->ownerProject != &project || !audioEngine()->canStopAudioStream(project)) {
            setError(muse::qtrc("automation", "Stop audio in the other project first."));
            return false;
        }
        audioEngine()->stopStream();
    }
    setError({});
    return true;
}

void TrackAutomationModel::changed()
{
    if (const auto state = effect()) {
        state->RefreshProcessingAutomation();
    }
    globalContext()->currentTrackeditProject()->notifyAboutTrackChanged(au::au3::DomConverter::track(track()));
    history()->pushHistoryState(muse::trc("automation", "Changed automation"), muse::trc("automation", "Automation"));
    emit dataChanged();
}

void TrackAutomationModel::setPoint(int index, double time, double value, double curve)
{
    if (!editable() || !std::isfinite(time) || time < 0.0 || !std::isfinite(value) || value < 0.0 || value > 1.0
        || !std::isfinite(curve) || std::abs(curve) > 1.0) {
        return;
    }
    auto* data = automation();
    const auto id = parameterId();
    const auto* existing = data->Find(id);
    AutomationLaneData lane = existing ? *existing : AutomationLaneData { id, true, {} };
    if (index < -1 || index >= static_cast<int>(lane.points.size())) {
        return;
    }
    if (index >= 0) {
        lane.points.erase(lane.points.begin() + index);
    }
    std::erase_if(lane.points, [time](const auto& point) { return std::abs(time - point.time) < 1e-7; });
    lane.points.push_back({ time, value, curve });
    std::sort(lane.points.begin(), lane.points.end(), [](const auto& a, const auto& b) { return a.time < b.time; });
    if ((existing && lane == *existing) || !prepareEdit()) {
        return;
    }
    if (data->SetLane(std::move(lane))) {
        changed();
    }
}

void TrackAutomationModel::removePoint(int index)
{
    if (!prepareEdit()) {
        return;
    }
    auto* data = automation();
    const auto* existing = data->Find(parameterId());
    if (existing && index >= 0 && index < static_cast<int>(existing->points.size())) {
        auto lane = *existing;
        lane.points.erase(lane.points.begin() + index);
        data->SetLane(std::move(lane));
        changed();
    }
}

void TrackAutomationModel::setLaneEnabled(bool enabled)
{
    if (!prepareEdit()) {
        return;
    }
    auto* data = automation();
    const auto id = parameterId();
    const auto* existing = data->Find(id);
    AutomationLaneData lane = existing ? *existing : AutomationLaneData { id, true, {} };
    if (lane.enabled != enabled) {
        lane.enabled = enabled;
        data->SetLane(std::move(lane));
        changed();
    }
}

void TrackAutomationModel::clearLane()
{
    if (prepareEdit() && automation()->RemoveLane(parameterId())) {
        changed();
    }
}

void TrackAutomationModel::reload()
{
    if (m_targetIndex >= targets().size()) {
        m_targetIndex = 0;
        m_parameterIndex = 0;
        refreshParameters();
    }
    QString owner = "track";
    bool instrumentChanged = false;
    const auto state = effect();
    if (state) {
        owner = au::au3::wxToString(state->GetID()).toQString();
    } else if (m_targetIndex > 0) {
        const auto* midi = dynamic_cast<const MidiTrack*>(track());
        owner = midi ? au::au3::wxToString(midi->GetInstrumentDescription()).toQString() : QString();
        instrumentChanged = midi && midi->GetInstrumentState() != m_instrumentState;
    }
    const bool effectSettingsChanged = state && state->GetAccess()->modificationCounter() != m_effectSettingsCounter;
    if (owner != m_parameterOwner || instrumentChanged || state != m_parameterEffect.lock() || effectSettingsChanged) {
        refreshParameters();
    }
    emit dataChanged();
}

void TrackAutomationModel::setError(const QString& error)
{
    if (m_error != error) {
        m_error = error;
        emit errorChanged();
    }
}
