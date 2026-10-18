/*
* Audacity: A Digital Audio Editor
*/
#pragma once

#include <atomic>

#include "au3-playable-track/PlayableTrack.h"
#include "au3-mixer/AudioIOSequences.h"

struct MidiNoteData {
    int64_t id = 0;
    double start = 0.0;
    double length = 1.0;
    int pitch = 60;
    int velocity = 100;
};

struct MidiControllerData {
    double beat = 0.0;
    int number = 64;
    int value = 0;
};

struct MidiClipData {
    int64_t id = 0;
    wxString name;
    double start = 0.0;
    double length = 4.0;
    int channel = 1;
    std::vector<MidiNoteData> notes;
    std::vector<MidiControllerData> controllers;
    double trimLeft = 0.0;
    double trimRight = 0.0;
    int colorIndex = -1;
    int64_t groupId = -1;
    bool selected = false;

    double visibleStart() const { return start + trimLeft; }
    double visibleEnd() const { return start + length - trimRight; }
    double visibleLength() const { return length - trimLeft - trimRight; }
};

class MIDI_TRACK_API MidiTrack final : public UniqueChannelTrack<PlayableTrack>, public OtherPlayableSequence
{
public:
    MidiTrack();
    MidiTrack(const MidiTrack& other, ProtectedCreationArg&& arg);

    static std::shared_ptr<MidiTrack> Create();
    static MidiTrack* New(AudacityProject& project);
    static const TypeInfo& ClassTypeInfo();
    const TypeInfo& GetTypeInfo() const override;

    double GetTempo() const { return mTempo; }
    void SetTempo(double tempo);
    double GetGain() const { return mGain.load(std::memory_order_relaxed); }
    void SetGain(double gain);
    double GetPan() const { return mPan.load(std::memory_order_relaxed); }
    void SetPan(double pan);

    const std::vector<MidiClipData>& GetClips() const { return mClips; }
    bool SetClips(std::vector<MidiClipData> clips);
    int64_t NextClipId() const;
    void SetClipSelected(int64_t id, bool selected);
    std::shared_ptr<MidiTrack> CopyClip(int64_t id) const;

    const wxString& GetInstrumentDescription() const { return mInstrumentDescription; }
    const wxString& GetInstrumentState() const { return mInstrumentState; }
    void SetInstrument(const wxString& description, const wxString& state);

    void MoveTo(double time) override;
    void ShiftBy(double time, double delta) override;
    void SplitAt(double time);
    Track::Holder Cut(double start, double end, bool moveClips) override;
    Track::Holder Copy(double start, double end, bool forClipboard = true) const override;
    void Clear(double start, double end, bool moveClips) override;
    void Paste(double time, const Track& source, bool moveClips) override;
    void Silence(double start, double end, ProgressReporter reportProgress = {}) override;
    void InsertSilence(double time, double length) override;
    Track::Holder PasteInto(AudacityProject& project, TrackList& list) const override;

    size_t NIntervals() const override { return mClips.size(); }
    bool HandleXMLTag(const std::string_view& tag, const AttributesList& attrs) override;
    XMLTagHandler* HandleXMLChild(const std::string_view& tag) override;
    void WriteXML(XMLWriter& writer) const override;

private:
    Track::Holder Clone(bool backup) const override;
    Track::Holder TrackEmptyCopy() const override;
    std::shared_ptr<WideChannelGroupInterval> DoGetInterval(size_t index) override;

    double mTempo = 120.0;
    std::atomic<double> mGain { 1.0 };
    std::atomic<double> mPan { 0.0 };
    double mCopyLength = 0.0;
    wxString mInstrumentDescription;
    wxString mInstrumentState;
    std::vector<MidiClipData> mClips;
};

ENUMERATE_TRACK_TYPE(MidiTrack);
