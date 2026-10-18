/*
* Audacity: A Digital Audio Editor
*/
#include "pianorollmodel.h"

#include <algorithm>
#include <cmath>
#include <chrono>

#include "au3-midi-track/MidiTrack.h"
#include "au3-project/Project.h"
#include "au3wrap/internal/domaccessor.h"
#include "au3wrap/internal/domconverter.h"
#include "au3wrap/internal/wxtypes_convert.h"
#include "effects/effects_base/effectstypes.h"
#include "project/iaudacityproject.h"
#include "trackedit/itrackeditproject.h"
#include "global/translation.h"

using namespace au::trackedit;

namespace {
double inputTime()
{
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

QString instrumentTitle(const muse::audioplugins::AudioPluginInfo& plugin)
{
    if (plugin.meta.attributeVal(au::effects::EFFECT_TYPE_ATTRIBUTE) != u"Instrument") {
        return {};
    }
    return plugin.meta.attributeVal(au::effects::EFFECT_TITLE_ATTRIBUTE).toQString();
}
}

PianoRollModel::PianoRollModel(QObject* parent)
    : QObject(parent), muse::Contextable(muse::iocCtxForQmlObject(this))
{
}

PianoRollModel::PianoRollModel(const muse::modularity::ContextPtr& ctx, QObject* parent)
    : QObject(parent), muse::Contextable(ctx)
{
}

PianoRollModel::~PianoRollModel()
{
    m_inputTimer.stop();
    m_instrumentSaveTimer.stop();
    if (m_liveInput && m_editor) {
        instrumentPlayback()->closeInstrument(m_editor);
    }
}

void PianoRollModel::init()
{
    if (m_initialised) {
        return;
    }
    m_initialised = true;
    connect(this, &PianoRollModel::dataChanged, this, &PianoRollModel::playbackPositionChanged);
    viewRequests()->finishInstrumentEditingRequested().onNotify(this, [this] {
        closeInput(true);
        emit instrumentEditorCloseRequested();
    });
    viewRequests()->flushInstrumentChangesRequested().onNotify(this, [this] { flushInstrumentChanges(); });
    m_instrumentSaveTimer.setSingleShot(true);
    m_instrumentSaveTimer.setInterval(250);
    connect(&m_instrumentSaveTimer, &QTimer::timeout, this, [this] {
        if (!m_inputRecording) {
            flushInstrumentChanges();
        }
    });
    m_inputTimer.setInterval(10);
    connect(&m_inputTimer, &QTimer::timeout, this, &PianoRollModel::pollInput);
    globalContext()->currentTrackeditProjectChanged().onNotify(this, [this] { onProjectChanged(); });
    globalContext()->isRecordingChanged().onNotify(this, [this] { emit dataChanged(); });
    history()->historyChanged().onReceive(this, [this](auto) { reload(); });
    knownPlugins()->pluginInfoListChanged().onNotify(this, [this] { reloadInstruments(); });
    reloadInstruments();
    onProjectChanged();
}

void PianoRollModel::onProjectChanged()
{
    m_projectSubscriptions.async_disconnectAll();
    if (const auto current = globalContext()->currentProject()) {
        current->aboutCloseBegin().onNotify(&m_projectSubscriptions, [this] { flushInstrumentChanges(); });
    }
    const auto project = globalContext()->currentTrackeditProject();
    if (project) {
        project->timeSignatureChanged().onReceive(&m_projectSubscriptions, [this](const TimeSignature& signature) {
            if (m_inputRecording && signature.tempo != m_recordTempo) {
                // Keep the pending take's driver times at the new constant
                // project tempo; notes already in the document keep their beats.
                const double ratio = signature.tempo / m_recordTempo;
                for (auto& note : m_recordedNotes) {
                    note.start *= ratio;
                    note.length *= ratio;
                }
                for (auto& controller : m_recordedControllers) {
                    controller.beat *= ratio;
                }
                m_recordTempo = signature.tempo;
                emit inputProgressChanged();
            }
            emit dataChanged();
        });
    }
    reload();
}

void PianoRollModel::initPanel()
{
    init();
    globalContext()->playbackState()->playbackPositionChanged().onReceive(this, [this](muse::secs_t) {
        emit playbackPositionChanged();
    });
    viewRequests()->pianoRollRequested().onReceive(this, [this](TrackId trackId, int64_t clipId) {
        setTrackId(QVariant::fromValue(trackId));
        setClipId(clipId);
        emit editorRequested();
    });
}

QVariant PianoRollModel::trackId() const
{
    return QVariant::fromValue(m_trackId);
}

void PianoRollModel::setTrackId(const QVariant& id)
{
    const long trackId = id.toLongLong();
    if (m_trackId == trackId) {
        return;
    }
    disableInput();
    m_trackId = trackId;
    m_clipId = -1;
    m_editor.reset();
    emit trackIdChanged();
    emit dataChanged();
}

MidiTrack* PianoRollModel::track() const
{
    const auto project = globalContext()->currentProject();
    if (!project) {
        return nullptr;
    }
    auto& au3 = *reinterpret_cast<AudacityProject*>(project->au3ProjectPtr());
    return dynamic_cast<MidiTrack*>(au::au3::DomAccessor::findTrack(au3, ::TrackId(m_trackId)));
}

const MidiClipData* PianoRollModel::clip() const
{
    const auto* source = track();
    if (source) {
        for (const auto& clip : source->GetClips()) {
            if (clip.id == m_clipId) {
                return &clip;
            }
        }
    }
    return nullptr;
}

void PianoRollModel::setClipId(qlonglong id)
{
    if (m_clipId != id) {
        stopInputRecording();
        m_clipId = id;
        emit dataChanged();
    }
}

double PianoRollModel::playbackBeat() const
{
    const auto* source = track();
    const auto* selected = clip();
    if (!source || !selected) {
        return -1.0;
    }
    return globalContext()->playbackState()->playbackPosition().to_double() * source->GetTempo() / 60.0 - selected->visibleStart();
}

QVariantList PianoRollModel::clips() const
{
    QVariantList result;
    const auto* source = track();
    if (!source) {
        return result;
    }
    for (const auto& clip : source->GetClips()) {
        QVariantList notes;
        for (const auto& note : clip.notes) {
            notes.push_back(QVariantMap { { "start", note.start - clip.trimLeft }, { "length", note.length }, { "pitch", note.pitch } });
        }
        result.push_back(QVariantMap { { "id", QVariant::fromValue<qlonglong>(clip.id) }, { "title", au::au3::wxToString(
                                                                                                clip.name).toQString() },
                             { "start", clip.visibleStart() * 60.0 / source->GetTempo() },
                             { "length", clip.visibleLength() * 60.0 / source->GetTempo() },
                             { "beats", clip.visibleLength() }, { "notes", notes } });
    }
    return result;
}

QVariantList PianoRollModel::notes() const
{
    QVariantList result;
    const auto* data = clip();
    if (data) {
        for (const auto& note : data->notes) {
            result.push_back(QVariantMap { { "id", QVariant::fromValue<qlonglong>(note.id) }, { "start", note.start - data->trimLeft },
                                 { "length", note.length }, { "pitch", note.pitch }, { "velocity", note.velocity } });
        }
    }
    return result;
}

QVariantList PianoRollModel::controllers() const
{
    QVariantList result;
    const auto* data = clip();
    if (data) {
        for (const auto& event : data->controllers) {
            result.push_back(QVariantMap { { "beat", event.beat - data->trimLeft }, { "number", event.number }, { "value", event.value } });
        }
    }
    return result;
}

double PianoRollModel::clipLength() const { return clip() ? clip()->visibleLength() : 0.0; }
double PianoRollModel::tempo() const { return track() ? track()->GetTempo() : 120.0; }
int PianoRollModel::upperTimeSignature() const
{
    const auto project = globalContext()->currentTrackeditProject();
    return project ? project->timeSignature().upper : 4;
}

int PianoRollModel::lowerTimeSignature() const
{
    const auto project = globalContext()->currentTrackeditProject();
    return project ? project->timeSignature().lower : 4;
}

QString PianoRollModel::title() const { return clip() ? au::au3::wxToString(clip()->name).toQString() : QString(); }
bool PianoRollModel::editable() const { return track() && !globalContext()->isRecording() && !m_inputRecording; }

bool PianoRollModel::prepareEdit()
{
    if (!editable()) {
        return false;
    }
    flushInstrumentChanges();
    auto& project = *reinterpret_cast<AudacityProject*>(globalContext()->currentProject()->au3ProjectPtr());
    if (audioEngine()->isBusy()) {
        if (!audioEngine()->canStopAudioStream(project)) {
            setError(muse::qtrc("pianoroll", "Stop playback in the other project first."));
            return false;
        }
        audioEngine()->stopStream();
        closeInput(false);
    }
    return true;
}

void PianoRollModel::changed(const char* description)
{
    globalContext()->currentTrackeditProject()->notifyAboutTrackChanged(au::au3::DomConverter::track(track()));
    history()->pushHistoryState(muse::trc("pianoroll", description), muse::trc("pianoroll", description));
    emit dataChanged();
}

qlonglong PianoRollModel::addClip(double seconds, double beats)
{
    if (!prepareEdit() || !std::isfinite(seconds) || !std::isfinite(beats) || seconds < 0.0 || beats <= 0.0) {
        return -1;
    }
    auto clips = track()->GetClips();
    MidiClipData clip;
    clip.id = track()->NextClipId();
    clip.name = au::au3::wxFromString(muse::mtrc("pianoroll", "MIDI clip"));
    clip.start = std::round(seconds * tempo() / 60.0 * 4.0) / 4.0;
    clip.length = beats;
    clips.push_back(clip);
    if (!track()->SetClips(std::move(clips))) {
        return -1;
    }
    m_clipId = clip.id;
    changed("Added MIDI clip");
    return clip.id;
}

void PianoRollModel::removeClip(qlonglong id)
{
    if (!prepareEdit()) {
        return;
    }
    auto clips = track()->GetClips();
    const auto removed = std::erase_if(clips, [id](const auto& clip) { return clip.id == id; });
    if (removed && track()->SetClips(std::move(clips))) {
        changed("Removed MIDI clip");
    }
}

void PianoRollModel::moveClip(qlonglong id, double seconds)
{
    if (!prepareEdit() || !std::isfinite(seconds) || seconds < 0.0) {
        return;
    }
    auto clips = track()->GetClips();
    for (auto& clip : clips) {
        if (clip.id == id) {
            clip.start = std::round(seconds * tempo() / 60.0 * 4.0) / 4.0 - clip.trimLeft;
            if (track()->SetClips(std::move(clips))) {
                changed("Moved MIDI clip");
            }
            return;
        }
    }
}

void PianoRollModel::openEditor(qlonglong id)
{
    viewRequests()->requestPianoRoll(m_trackId, id);
}

void PianoRollModel::addNote(double start, double length, int pitch, int velocity)
{
    if (!prepareEdit() || !clip()) {
        return;
    }
    auto clips = track()->GetClips();
    for (auto& clip : clips) {
        if (clip.id != m_clipId) {
            continue;
        }
        int64_t nextId = 1;
        for (const auto& note : clip.notes) {
            nextId = std::max(nextId, note.id + 1);
        }
        clip.notes.push_back({ nextId, start + clip.trimLeft, length, pitch, velocity });
        if (track()->SetClips(std::move(clips))) {
            changed("Added MIDI note");
        }
        return;
    }
}

void PianoRollModel::editNote(qlonglong id, double start, double length, int pitch, int velocity)
{
    if (!prepareEdit() || !clip()) {
        return;
    }
    auto clips = track()->GetClips();
    for (auto& clip : clips) {
        if (clip.id != m_clipId) {
            continue;
        }
        for (auto& note : clip.notes) {
            if (note.id == id) {
                const char* description = "Changed MIDI note";
                if (note.pitch != pitch) {
                    description = "Changed MIDI note pitch";
                } else if (note.start != start + clip.trimLeft) {
                    description = "Moved MIDI note";
                } else if (note.length != length) {
                    description = "Resized MIDI note";
                } else if (note.velocity != velocity) {
                    description = "Changed MIDI note velocity";
                } else {
                    return;
                }
                note = { id, start + clip.trimLeft, length, pitch, velocity };
                if (track()->SetClips(std::move(clips))) {
                    changed(description);
                }
                return;
            }
        }
    }
}

void PianoRollModel::removeNote(qlonglong id)
{
    if (!prepareEdit() || !clip()) {
        return;
    }
    auto clips = track()->GetClips();
    for (auto& clip : clips) {
        if (clip.id == m_clipId) {
            const auto removed = std::erase_if(clip.notes, [id](const auto& note) { return note.id == id; });
            if (removed && track()->SetClips(std::move(clips))) {
                changed("Removed MIDI note");
            }
            return;
        }
    }
}

void PianoRollModel::setSustain(double beat, bool down)
{
    if (!prepareEdit() || !clip() || !std::isfinite(beat) || beat < 0.0 || beat >= clipLength()) {
        return;
    }
    auto clips = track()->GetClips();
    for (auto& clip : clips) {
        if (clip.id == m_clipId) {
            std::erase_if(clip.controllers, [beat, &clip](const auto& event) {
                return event.number == 64 && std::abs(event.beat - clip.trimLeft - beat) < 1e-6;
            });
            clip.controllers.push_back({ beat + clip.trimLeft, 64, down ? 127 : 0 });
            if (track()->SetClips(std::move(clips))) {
                changed("Changed sustain pedal");
            }
            return;
        }
    }
}

QVariantList PianoRollModel::instruments() const
{
    return m_instruments;
}

void PianoRollModel::reloadInstruments()
{
    QVariantList result { QVariantMap { { "title", muse::qtrc("pianoroll", "Audacity Synth") }, { "description", QString() },
                              { "format", muse::qtrc("pianoroll", "Built-in") }, { "vendor", "Audacity" } } };
    for (const auto& plugin : knownPlugins()->pluginInfoList()) {
        if (plugin.state != muse::audioplugins::AudioPluginState::Validated
            || plugin.meta.attributeVal(effects::EFFECT_ACTIVATED_ATTRIBUTE) == u"false") {
            continue;
        }
        const auto title = instrumentTitle(plugin);
        if (!title.isEmpty()) {
            const auto format = plugin.meta.type == "AudioUnit" ? QStringLiteral("AU")
                                : plugin.meta.type == "VstPlugin" ? QStringLiteral("VST3")
                                : plugin.meta.type == "Lv2Plugin" ? QStringLiteral("LV2") : QString::fromStdString(plugin.meta.type);
            result.push_back(QVariantMap { { "title", title }, { "description", QString::fromStdString(plugin.meta.id) },
                                 { "format", format }, { "vendor", QString::fromStdString(plugin.meta.vendor) } });
        }
    }
    // Preserve familiar short names until they are ambiguous. AU and VST3
    // versions of the same instrument otherwise look identical in this menu.
    for (int pass = 0; pass < 3; ++pass) {
        std::map<QString, int> counts;
        for (const auto& item : result) {
            ++counts[item.toMap().value("title").toString()];
        }
        for (auto& item : result) {
            auto fields = item.toMap();
            const auto title = fields.value("title").toString();
            if (counts[title] > 1) {
                const auto detail = fields.value(pass == 0 ? "format" : pass == 1 ? "vendor" : "description").toString();
                fields["title"] = title + " (" + detail + ")";
                item = fields;
            }
        }
    }
    m_instruments = std::move(result);
    emit instrumentsChanged();
    emit dataChanged();
}

int PianoRollModel::instrumentIndex() const
{
    if (!track()) {
        return -1;
    }
    const auto list = instruments();
    const QString description = au::au3::wxToString(track()->GetInstrumentDescription()).toQString();
    for (int i = 0; i < list.size(); ++i) {
        if (list[i].toMap().value("description").toString() == description) {
            return i;
        }
    }
    return -1;
}

void PianoRollModel::selectInstrument(int index)
{
    const auto list = instruments();
    if (index < 0 || index >= list.size() || !prepareEdit()) {
        return;
    }
    const bool resumeInput = m_inputEnabled;
    closeInput(true);
    const auto description = list[index].toMap().value("description").toString().toStdString();
    const auto editor = instrumentEditorFactory()->create(description, {});
    if (!editor.ret) {
        setError(QString::fromStdString(editor.ret.toString()));
        return;
    }
    m_editor = editor.val;
    m_editorDescription = description;
    m_editorState = m_editor->state();
    track()->SetInstrument(wxString::FromUTF8(description), wxString::FromUTF8(m_editorState));
    changed("Changed instrument");
    if (resumeInput) {
        enableInput();
    }
    setError({});
    emit parametersChanged();
}

void PianoRollModel::loadParameters()
{
    if (m_liveInput && !inputMatchesTrack()) {
        m_inputEnabled = false;
        closeInput(false);
    }
    if (!track()) {
        return;
    }
    const auto description = track()->GetInstrumentDescription().utf8_string();
    const auto state = track()->GetInstrumentState().utf8_string();
    if (m_editor && m_editor->active() && description == m_editorDescription && state == m_editorState) {
        return;
    }
    m_editor.reset();
    const auto editor = instrumentEditorFactory()->create(description, state);
    if (!editor.ret) {
        setError(QString::fromStdString(editor.ret.toString()));
        return;
    }
    m_editor = editor.val;
    m_editorDescription = description;
    m_editorState = state;
    setError({});
    emit parametersChanged();
}

QVariantList PianoRollModel::parameters() const
{
    QVariantList result;
    if (m_editor) {
        for (const auto& parameter : m_editor->parameters()) {
            result.push_back(QVariantMap { { "id", QString::fromStdString(parameter.id) }, { "title", QString::fromStdString(
                                                                                                 parameter.name) },
                                 { "display", QString::fromStdString(parameter.display) }, { "value", parameter.value } });
        }
    }
    return result;
}

void PianoRollModel::setParameter(const QString& id, double value)
{
    const bool live = m_editor && m_editor->active() && inputMatchesTrack();
    if (!track() || globalContext()->isRecording()) {
        return;
    }
    if (!live && !prepareEdit()) {
        return;
    }
    if (!live) {
        loadParameters();
    }
    if (m_editor && m_editor->setParameter(id.toStdString(), value)) {
        m_editorState = m_editor->state();
        track()->SetInstrument(wxString::FromUTF8(m_editorDescription), wxString::FromUTF8(m_editorState));
        changed("Changed instrument parameter");
        emit parametersChanged();
    }
}

void PianoRollModel::reload()
{
    if (m_editor && ((m_liveInput && !inputMatchesTrack()) || !track()
                     || track()->GetInstrumentDescription().utf8_string() != m_editorDescription
                     || track()->GetInstrumentState().utf8_string() != m_editorState)) {
        m_inputEnabled = false;
        closeInput(false);
        m_editor.reset();
        emit parametersChanged();
        emit instrumentEditorCloseRequested();
    }
    emit dataChanged();
}

void PianoRollModel::setError(const QString& error)
{
    if (m_error != error) {
        m_error = error;
        emit errorChanged();
    }
}

void PianoRollModel::undo()
{
    if (history()->undoAvailable() && prepareEdit()) {
        history()->undo();
    }
}

void PianoRollModel::redo()
{
    if (history()->redoAvailable() && prepareEdit()) {
        history()->redo();
    }
}

bool PianoRollModel::inputMatchesTrack() const
{
    const auto project = globalContext()->currentProject();
    const auto* source = track();
    return m_liveInput && project && source && source == m_inputTrack.lock().get()
           && project->au3ProjectPtr() == reinterpret_cast<uintptr_t>(m_inputProject.lock().get())
           && source->GetInstrumentDescription().utf8_string() == m_editorDescription
           && source->GetInstrumentState().utf8_string() == m_editorState;
}

bool PianoRollModel::enableInput()
{
    if (!track() || globalContext()->isRecording()) {
        return false;
    }
    if (m_editor && m_editor->active() && inputMatchesTrack()) {
        return true;
    }
    closeInput(!m_liveInput || inputMatchesTrack());
    auto& project = *reinterpret_cast<AudacityProject*>(globalContext()->currentProject()->au3ProjectPtr());
    const auto result = instrumentPlayback()->openInstrument(project, *track());
    if (!result.ret) {
        setError(QString::fromStdString(result.ret.toString()));
        return false;
    }
    m_editor = result.val;
    m_liveInput = true;
    m_inputEnabled = true;
    m_inputProject = project.shared_from_this();
    m_inputTrack = std::static_pointer_cast<MidiTrack>(track()->shared_from_this());
    m_editorDescription = track()->GetInstrumentDescription().utf8_string();
    m_editorState = track()->GetInstrumentState().utf8_string();
    setError({});
    refreshMidiInputs();
    if (!m_midiInputId.empty() && !m_editor->setMidiInput(m_midiInputId)) {
        setError(muse::qtrc("pianoroll", "The MIDI input is not available."));
    }
    m_inputTimer.setInterval(10);
    m_inputTimer.start();
    emit instrumentEditorChanged();
    emit parametersChanged();
    return true;
}

void PianoRollModel::disableInput()
{
    m_inputEnabled = false;
    closeInput(true);
}

void PianoRollModel::closeInput(bool save)
{
    if (save) {
        stopInputRecording();
    } else {
        m_inputRecording = false;
        m_recordedNotes.clear();
        m_recordedControllers.clear();
        emit inputRecordingChanged();
    }
    m_inputTimer.stop();
    m_instrumentSaveTimer.stop();
    if (m_liveInput && m_editor) {
        if (save) {
            flushInstrumentChanges();
        }
        instrumentPlayback()->closeInstrument(m_editor);
        m_editor.reset();
        m_held.clear();
        emit heldNotesChanged();
        emit instrumentEditorChanged();
        emit parametersChanged();
    }
    m_liveInput = false;
    m_instrumentChangesPending = false;
    m_inputProject.reset();
    m_inputTrack.reset();
}

void PianoRollModel::refreshMidiInputs()
{
    m_midiInputs = { QVariantMap { { "title", muse::qtrc("pianoroll", "No MIDI input") }, { "id", QString() } } };
    if (m_editor && m_editor->active()) {
        for (const auto& input : m_editor->midiInputs()) {
            m_midiInputs.push_back(QVariantMap { { "title", QString::fromStdString(input.name) },
                                       { "id", QString::fromStdString(input.id) } });
        }
    }
    emit midiInputsChanged();
}

bool PianoRollModel::connectMidiInput()
{
    if (!enableInput()) {
        return false;
    }
    refreshMidiInputs();
    if (!m_midiInputId.empty() && !m_editor->setMidiInput(m_midiInputId)) {
        setError(muse::qtrc("pianoroll", "The MIDI input is not available."));
        return false;
    }
    setError({});
    return true;
}

int PianoRollModel::midiInputIndex() const
{
    for (int i = 0; i < m_midiInputs.size(); ++i) {
        if (m_midiInputs[i].toMap().value("id").toString().toStdString() == m_midiInputId) {
            return i;
        }
    }
    return 0;
}

void PianoRollModel::selectMidiInput(int index)
{
    if (!enableInput() || index < 0 || index >= m_midiInputs.size()) {
        return;
    }
    const auto id = m_midiInputs[index].toMap().value("id").toString().toStdString();
    if (m_editor->setMidiInput(id)) {
        m_midiInputId = id;
        setError({});
        emit midiInputsChanged();
    } else {
        setError(muse::qtrc("pianoroll", "The MIDI input is not available."));
    }
}

void PianoRollModel::noteOn(int pitch, int velocity)
{
    if (pitch < 0 || pitch > 127 || velocity < 1 || velocity > 127 || !enableInput()) {
        return;
    }
    const int channel = clip() ? clip()->channel - 1 : 0;
    m_editor->sendMidi(0x90 | channel, pitch, velocity);
    receiveMidi({ 0x90 | channel, pitch, velocity, inputTime() });
}

void PianoRollModel::noteOff(int pitch)
{
    if (pitch < 0 || pitch > 127 || !m_editor || !m_editor->active()) {
        return;
    }
    const int channel = clip() ? clip()->channel - 1 : 0;
    m_editor->sendMidi(0x80 | channel, pitch, 0);
    receiveMidi({ 0x80 | channel, pitch, 0, inputTime() });
}

QVariantList PianoRollModel::heldNotes() const
{
    QVariantList notes;
    for (const auto& [key, message] : m_held) {
        notes.push_back(message.data1);
    }
    return notes;
}

QVariantList PianoRollModel::inputNotes() const
{
    QVariantList notes;
    if (!m_inputRecording) {
        return notes;
    }
    for (const auto& note : m_recordedNotes) {
        notes.push_back(QVariantMap { { "start", note.start }, { "length", note.length }, { "pitch", note.pitch },
                            { "velocity", note.velocity } });
    }
    const double end = (inputTime() - m_recordStart) * m_recordTempo / 60.0;
    for (const auto& [key, message] : m_held) {
        const double start = std::max(0.0, (message.time - m_recordStart) * m_recordTempo / 60.0);
        notes.push_back(QVariantMap { { "start", start }, { "length", std::max(1.0 / 64.0, end - start) },
                            { "pitch", message.data1 }, { "velocity", message.data2 } });
    }
    return notes;
}

double PianoRollModel::inputLength() const
{
    return m_inputRecording ? (inputTime() - m_recordStart) * m_recordTempo / 60.0 : 0.0;
}

void PianoRollModel::finishNote(int key, double time)
{
    const auto held = m_held.find(key);
    if (held == m_held.end()) {
        return;
    }
    if (m_inputRecording) {
        const auto& message = held->second;
        const double start = std::max(0.0, (message.time - m_recordStart) * m_recordTempo / 60.0);
        const double end = std::max(start + 1.0 / 64.0, (time - m_recordStart) * m_recordTempo / 60.0);
        m_recordedNotes.push_back({ 0, start, end - start, message.data1, message.data2 });
    }
    m_held.erase(held);
}

void PianoRollModel::receiveMidi(const audio::InstrumentMidiMessage& message)
{
    const auto heldCount = m_held.size();
    const int type = message.status & 0xf0;
    const int channel = message.status & 0xf;
    const int sourceOffset = static_cast<int>(message.source) * 16 * 128;
    const int key = sourceOffset + channel * 128 + message.data1;
    if (type == 0x90 && message.data2 > 0) {
        finishNote(key, message.time);
        m_held[key] = message;
    } else if (type == 0x80 || (type == 0x90 && message.data2 == 0)) {
        finishNote(key, message.time);
    } else if (type == 0xb0) {
        if (message.data1 == 120 || message.data1 >= 123) {
            for (int note = 0; note < 128; ++note) {
                finishNote(sourceOffset + channel * 128 + note, message.time);
            }
        }
        if (m_inputRecording && (message.data1 < 120 || message.data1 == 121)) {
            m_recordedControllers.push_back({ std::max(0.0, (message.time - m_recordStart) * m_recordTempo / 60.0),
                                              message.data1, message.data2 });
        }
    }
    if (type == 0x90 || type == 0x80 || m_held.size() != heldCount) {
        emit heldNotesChanged();
    }
}

void PianoRollModel::releaseNotes()
{
    if (m_editor && m_editor->active()) {
        m_editor->releaseNotes();
    }
    const double time = inputTime();
    while (!m_held.empty()) {
        finishNote(m_held.begin()->first, time);
    }
    emit heldNotesChanged();
}

void PianoRollModel::startInputRecording()
{
    if (!editable() || !clip() || !enableInput()) {
        return;
    }
    releaseNotes();
    m_recordedNotes.clear();
    m_recordedControllers.clear();
    m_recordStart = inputTime();
    m_recordTempo = tempo();
    m_inputRecording = true;
    emit inputProgressChanged();
    emit inputRecordingChanged();
    emit dataChanged();
}

void PianoRollModel::stopInputRecording()
{
    if (!m_inputRecording) {
        return;
    }
    if (m_editor && m_editor->active()) {
        for (const auto& message : m_editor->takeMidiMessages()) {
            receiveMidi(message);
        }
        if (const auto error = m_editor->takeMidiInputError(); !error.empty()) {
            setError(QString::fromStdString(error));
        }
    }
    releaseNotes();
    m_inputRecording = false;
    if (inputMatchesTrack() && clip() && (!m_recordedNotes.empty() || !m_recordedControllers.empty())) {
        auto clips = track()->GetClips();
        for (auto& target : clips) {
            if (target.id != m_clipId) {
                continue;
            }
            int64_t nextId = 1;
            double end = target.length - target.trimRight;
            for (const auto& note : target.notes) {
                nextId = std::max(nextId, note.id + 1);
            }
            for (auto note : m_recordedNotes) {
                note.id = nextId++;
                note.start += target.trimLeft;
                end = std::max(end, note.start + note.length);
                target.notes.push_back(note);
            }
            for (auto controller : m_recordedControllers) {
                controller.beat += target.trimLeft;
                end = std::max(end, controller.beat + 0.25);
                target.controllers.push_back(controller);
            }
            const auto pedal = std::find_if(m_recordedControllers.rbegin(), m_recordedControllers.rend(), [](const auto& event) {
                return event.number == 64 || event.number == 121;
            });
            if (pedal != m_recordedControllers.rend() && pedal->number == 64 && pedal->value >= 64) {
                const double beat = std::max(end, (inputTime() - m_recordStart) * m_recordTempo / 60.0 + target.trimLeft);
                target.controllers.push_back({ beat, 64, 0 });
                end = beat + 0.25;
            }
            const double visibleEnd = std::max(target.length - target.trimRight, end);
            target.length = std::ceil(std::max(target.length, visibleEnd) * 4.0) / 4.0;
            target.trimRight = target.length - visibleEnd;
        }
        if (track()->SetClips(std::move(clips))) {
            changed("Recorded MIDI notes");
        }
    }
    m_recordedNotes.clear();
    m_recordedControllers.clear();
    emit inputRecordingChanged();
    emit inputProgressChanged();
    emit dataChanged();
}

void PianoRollModel::saveInstrumentChanges()
{
    m_instrumentSaveTimer.stop();
    m_instrumentChangesPending = false;
    if (!m_editor || !inputMatchesTrack()) {
        return;
    }
    m_editor->takeChanges();
    const auto state = m_editor->state();
    if (state != m_editorState) {
        m_editorState = state;
        track()->SetInstrument(wxString::FromUTF8(m_editorDescription), wxString::FromUTF8(state));
        changed("Changed instrument settings");
        emit parametersChanged();
    }
}

void PianoRollModel::flushInstrumentChanges()
{
    if (!m_editor || !inputMatchesTrack()) {
        return;
    }
    // Save/Undo can arrive before the next input tick. Include those latest
    // controller messages without interrupting monitoring or closing the UI.
    for (const auto& message : m_editor->takeMidiMessages()) {
        receiveMidi(message);
    }
    const bool changed = m_editor->takeChanges();
    if (m_instrumentChangesPending || changed) {
        saveInstrumentChanges();
    }
}

void PianoRollModel::pollInput()
{
    if (!m_editor || !m_editor->active()) {
        closeInput(true);
        if (m_inputEnabled && !enableInput()) {
            m_inputTimer.setInterval(250);
            m_inputTimer.start();
        }
        return;
    }
    for (const auto& message : m_editor->takeMidiMessages()) {
        receiveMidi(message);
    }
    if (const auto error = m_editor->takeMidiInputError(); !error.empty()) {
        setError(QString::fromStdString(error));
    }
    const double time = inputTime();
    if (m_inputRecording && time - m_lastInputUpdate >= 0.05) {
        m_lastInputUpdate = time;
        emit inputProgressChanged();
    }
    if (!m_inputRecording && m_editor->takeChanges()) {
        // Live parameter delivery above never waits for preset serialization,
        // project autosave, or undo snapshots. Commit the completed gesture
        // once, or synchronously at an explicit Save/Undo/lifecycle boundary.
        m_instrumentChangesPending = true;
        m_instrumentSaveTimer.start();
        if (time - m_lastParameterUpdate >= 0.05) {
            m_lastParameterUpdate = time;
            emit parametersChanged();
        }
    }
}

void PianoRollModel::showInstrumentEditor()
{
    if (enableInput()) {
        emit parametersChanged();
        emit instrumentEditorRequested();
    }
}

void PianoRollModel::hideInstrumentEditor()
{
    saveInstrumentChanges();
    if (m_editor) {
        m_editor->detachView();
    }
}
