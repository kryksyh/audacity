/*
* Audacity: A Digital Audio Editor
*/
#pragma once

#include <memory>
#include <string>
#include <vector>

#include "global/modularity/imoduleinterface.h"
#include "global/types/retval.h"

class AudacityProject;
class MidiTrack;

namespace au::audio {
struct InstrumentParameter {
    std::string id;
    std::string name;
    std::string display;
    double value = 0.0;
};

struct InstrumentMidiInput {
    std::string id;
    std::string name;
};

struct InstrumentMidiMessage {
    int status = 0;
    int data1 = 0;
    int data2 = 0;
    double time = 0.0;
};

struct InstrumentEditorSize {
    int width = 0;
    int height = 0;
};

class IInstrumentEditor
{
public:
    virtual ~IInstrumentEditor() = default;
    virtual std::vector<InstrumentParameter> parameters() const = 0;
    virtual bool setParameter(const std::string& id, double normalizedValue) = 0;
    virtual std::string state() const = 0;
    virtual bool active() const { return true; }
    virtual void close() {}
    virtual bool takeChanges() { return false; }
    virtual std::vector<InstrumentMidiInput> midiInputs() const { return {}; }
    virtual bool setMidiInput(const std::string&) { return false; }
    virtual void sendMidi(int, int, int) {}
    virtual void releaseNotes() {}
    virtual std::vector<InstrumentMidiMessage> takeMidiMessages() { return {}; }
    virtual bool attachView(void*, int, int, int, int) { return false; }
    virtual void detachView() {}
    virtual InstrumentEditorSize editorSize() const { return {}; }
};

class IInstrumentEditorFactory : MODULE_GLOBAL_INTERFACE
{
    INTERFACE_ID(IInstrumentEditorFactory)
public:
    virtual muse::RetVal<std::shared_ptr<IInstrumentEditor> > create(const std::string& description, const std::string& state) const = 0;
};
}

namespace au::audio {
class IInstrumentPlayback : MODULE_GLOBAL_INTERFACE
{
    INTERFACE_ID(IInstrumentPlayback)
public:
    virtual muse::RetVal<std::shared_ptr<IInstrumentEditor> > openInstrument(AudacityProject& project, MidiTrack& track) = 0;
    virtual void closeInstrument(const std::shared_ptr<IInstrumentEditor>& editor) = 0;
};
}
