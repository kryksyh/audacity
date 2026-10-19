/*
* Audacity: A Digital Audio Editor
*/
#pragma once

#include <QObject>
#include <QVariantList>
#include <QTimer>
#include <map>

#include "async/asyncable.h"
#include "modularity/ioc.h"
#include "context/iglobalcontext.h"
#include "audio/iaudioengine.h"
#include "audio/iinstrumenteditor.h"
#include "audioplugins/iknownaudiopluginsregister.h"
#include "trackedit/itracksviewrequestsservice.h"
#include "trackedit/iprojecthistory.h"

class MidiTrack;
struct MidiClipData;
struct MidiNoteData;
struct MidiControllerData;
class AudacityProject;

namespace au::trackedit {
class PianoRollModel : public QObject, public muse::async::Asyncable, public muse::Contextable
{
    Q_OBJECT
    Q_PROPERTY(QVariant trackId READ trackId WRITE setTrackId NOTIFY trackIdChanged)
    Q_PROPERTY(qlonglong clipId READ clipId WRITE setClipId NOTIFY dataChanged)
    Q_PROPERTY(QVariantList clips READ clips NOTIFY dataChanged)
    Q_PROPERTY(QVariantList notes READ notes NOTIFY dataChanged)
    Q_PROPERTY(QVariantList controllers READ controllers NOTIFY dataChanged)
    Q_PROPERTY(double clipLength READ clipLength NOTIFY dataChanged)
    Q_PROPERTY(double tempo READ tempo NOTIFY dataChanged)
    Q_PROPERTY(double playbackBeat READ playbackBeat NOTIFY playbackPositionChanged)
    Q_PROPERTY(QString title READ title NOTIFY dataChanged)
    Q_PROPERTY(QVariantList instruments READ instruments NOTIFY instrumentsChanged)
    Q_PROPERTY(int instrumentIndex READ instrumentIndex NOTIFY dataChanged)
    Q_PROPERTY(QVariantList parameters READ parameters NOTIFY parametersChanged)
    Q_PROPERTY(QString error READ error NOTIFY errorChanged)
    Q_PROPERTY(bool editable READ editable NOTIFY dataChanged)
    Q_PROPERTY(QVariantList midiInputs READ midiInputs NOTIFY midiInputsChanged)
    Q_PROPERTY(int midiInputIndex READ midiInputIndex NOTIFY midiInputsChanged)
    Q_PROPERTY(bool inputRecording READ inputRecording NOTIFY inputRecordingChanged)
    Q_PROPERTY(QVariantList heldNotes READ heldNotes NOTIFY heldNotesChanged)
    Q_PROPERTY(QVariantList inputNotes READ inputNotes NOTIFY inputProgressChanged)
    Q_PROPERTY(double inputLength READ inputLength NOTIFY inputProgressChanged)

    muse::GlobalInject<audio::IAudioEngine> audioEngine;
    muse::GlobalInject<audio::IInstrumentPlayback> instrumentPlayback;
    muse::GlobalInject<audio::IInstrumentEditorFactory> instrumentEditorFactory;
    muse::GlobalInject<muse::audioplugins::IKnownAudioPluginsRegister> knownPlugins;

    muse::ContextInject<context::IGlobalContext> globalContext { this };
    muse::ContextInject<IProjectHistory> history { this };
    muse::ContextInject<ITracksViewRequestsService> viewRequests { this };

public:
    explicit PianoRollModel(QObject* parent = nullptr);
    PianoRollModel(const muse::modularity::ContextPtr& ctx, QObject* parent = nullptr);
    ~PianoRollModel() override;
    Q_INVOKABLE void init();
    Q_INVOKABLE void initPanel();
    QVariant trackId() const;
    void setTrackId(const QVariant& id);
    qlonglong clipId() const { return m_clipId; }
    void setClipId(qlonglong id);
    QVariantList clips() const;
    QVariantList notes() const;
    QVariantList controllers() const;
    double clipLength() const;
    double tempo() const;
    double playbackBeat() const;
    QString title() const;
    QVariantList instruments() const;
    int instrumentIndex() const;
    QVariantList parameters() const;
    QString error() const { return m_error; }
    bool editable() const;

    Q_INVOKABLE qlonglong addClip(double seconds, double beats = 4.0);
    Q_INVOKABLE void removeClip(qlonglong id);
    Q_INVOKABLE void moveClip(qlonglong id, double seconds);
    Q_INVOKABLE void openEditor(qlonglong id);
    Q_INVOKABLE void addNote(double start, double length, int pitch, int velocity);
    Q_INVOKABLE void editNote(qlonglong id, double start, double length, int pitch, int velocity);
    Q_INVOKABLE void removeNote(qlonglong id);
    Q_INVOKABLE void setSustain(double beat, bool down);
    Q_INVOKABLE void selectInstrument(int index);
    Q_INVOKABLE void loadParameters();
    Q_INVOKABLE void setParameter(const QString& id, double value);
    Q_INVOKABLE void undo();
    Q_INVOKABLE void redo();
    Q_INVOKABLE bool enableInput();
    Q_INVOKABLE void disableInput();
    Q_INVOKABLE void refreshMidiInputs();
    Q_INVOKABLE void selectMidiInput(int index);
    Q_INVOKABLE void noteOn(int pitch, int velocity);
    Q_INVOKABLE void noteOff(int pitch);
    Q_INVOKABLE void releaseNotes();
    Q_INVOKABLE void startInputRecording();
    Q_INVOKABLE void stopInputRecording();
    Q_INVOKABLE void showInstrumentEditor();
    Q_INVOKABLE void hideInstrumentEditor();
    QVariantList midiInputs() const { return m_midiInputs; }
    int midiInputIndex() const;
    QVariantList heldNotes() const;
    QVariantList inputNotes() const;
    double inputLength() const;
    bool inputRecording() const { return m_inputRecording; }
    std::shared_ptr<audio::IInstrumentEditor> instrumentEditor() const { return m_editor; }

signals:
    void editorRequested();
    void instrumentEditorChanged();
    void instrumentEditorRequested();
    void instrumentEditorCloseRequested();
    void midiInputsChanged();
    void inputRecordingChanged();
    void heldNotesChanged();
    void inputProgressChanged();
    void playbackPositionChanged();
    void trackIdChanged();
    void dataChanged();
    void instrumentsChanged();
    void parametersChanged();
    void errorChanged();

private:
    MidiTrack* track() const;
    const MidiClipData* clip() const;
    bool prepareEdit();
    void changed(const char* description);
    void reload();
    void reloadInstruments();
    void setError(const QString& error);
    void closeInput(bool save);
    void pollInput();
    void receiveMidi(const audio::InstrumentMidiMessage& message);
    void finishNote(int key, double time);
    void saveInstrumentChanges();
    void savePendingInstrumentChanges();

    long m_trackId = -1;
    int64_t m_clipId = -1;
    bool m_initialised = false;
    bool m_liveInput = false;
    bool m_inputEnabled = false;
    std::string m_midiInputId;
    std::weak_ptr<AudacityProject> m_inputProject;
    bool m_inputRecording = false;
    double m_recordStart = 0.0;
    double m_recordTempo = 120.0;
    double m_lastInputUpdate = 0.0;
    double m_lastInstrumentChange = 0.0;
    bool m_instrumentChangesPending = false;
    QTimer m_inputTimer;
    QVariantList m_midiInputs;
    std::map<int, audio::InstrumentMidiMessage> m_held;
    std::vector<MidiNoteData> m_recordedNotes;
    std::vector<MidiControllerData> m_recordedControllers;

    QString m_error;
    QVariantList m_instruments;
    std::shared_ptr<audio::IInstrumentEditor> m_editor;
    std::string m_editorDescription;
    std::string m_editorState;
};
}
