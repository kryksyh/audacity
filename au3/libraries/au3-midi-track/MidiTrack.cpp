/*
* Audacity: A Digital Audio Editor
*/
#include "MidiTrack.h"
#include "au3-playable-track/TrackAutomation.h"
#include "au3-playable-track/TrackRouting.h"

#include <cmath>
#include <map>
#include <set>

#include "au3-project/Project.h"
#include "au3-realtime-effects/RealtimeEffectList.h"
#include "au3-stretching-sequence/TempoChange.h"
#include "au3-xml/XMLWriter.h"

namespace {
bool validNote(const MidiNoteData& note, double length)
{
    return note.id > 0 && std::isfinite(note.start) && std::isfinite(note.length)
           && note.start >= 0.0 && note.length > 0.0 && note.start + note.length <= length + 1e-9
           && note.pitch >= 0 && note.pitch <= 127 && note.velocity > 0 && note.velocity <= 127;
}

bool validClip(const MidiClipData& clip)
{
    if (clip.id <= 0 || !std::isfinite(clip.start) || !std::isfinite(clip.length)
        || clip.start < 0.0 || clip.length <= 0.0 || clip.channel < 1 || clip.channel > 16) {
        return false;
    }
    std::set<int64_t> ids;
    for (const auto& note : clip.notes) {
        if (!validNote(note, clip.length) || !ids.insert(note.id).second) {
            return false;
        }
    }
    return std::all_of(clip.controllers.begin(), clip.controllers.end(), [&](const MidiControllerData& event) {
        return std::isfinite(event.beat) && event.beat >= 0.0 && event.beat <= clip.length
               && event.number >= 0 && event.number <= 127 && event.value >= 0 && event.value <= 127;
    });
}

MidiClipData slice(const MidiClipData& source, double start, double end)
{
    MidiClipData result = source;
    result.start = source.start + start;
    result.length = end - start;
    result.notes.clear();
    result.controllers.clear();
    for (auto note : source.notes) {
        const double left = std::max(start, note.start);
        const double right = std::min(end, note.start + note.length);
        if (left < right) {
            note.start = left - start;
            note.length = right - left;
            result.notes.push_back(note);
        }
    }
    std::map<int, MidiControllerData> initial;
    for (auto event : source.controllers) {
        if (event.beat < start) {
            auto found = initial.find(event.number);
            if (found == initial.end() || found->second.beat <= event.beat) {
                initial[event.number] = event;
            }
        } else if (event.beat < end) {
            event.beat -= start;
            result.controllers.push_back(event);
        }
    }
    for (auto [number, event] : initial) {
        event.beat = 0.0;
        result.controllers.insert(result.controllers.begin(), event);
    }
    return result;
}

class MidiInterval final : public WideChannelGroupInterval
{
public:
    MidiInterval(double start, double end)
        : mStart(start), mEnd(end) {}
    double Start() const override { return mStart; }
    double End() const override { return mEnd; }
    size_t NChannels() const override { return 1; }
private:
    std::shared_ptr<ChannelInterval> DoGetChannel(size_t index) override
    {
        return index == 0 ? std::make_shared<ChannelInterval>() : nullptr;
    }

    double mStart;
    double mEnd;
};

const ProjectFileIORegistry::ObjectReaderEntry readerEntry { "miditrack", MidiTrack::New };
}

MidiTrack::MidiTrack() = default;

MidiTrack::MidiTrack(const MidiTrack& other, ProtectedCreationArg&& arg)
    : UniqueChannelTrack(other, std::move(arg)), mTempo(other.mTempo), mGain(other.mGain), mPan(other.mPan),
    mCopyLength(other.mCopyLength), mInstrumentDescription(other.mInstrumentDescription), mInstrumentState(other.mInstrumentState),
    mClips(other.mClips)
{
}

std::shared_ptr<MidiTrack> MidiTrack::Create()
{
    auto track = std::make_shared<MidiTrack>();
    track->AttachedTrackObjects::BuildAll();
    return track;
}

MidiTrack* MidiTrack::New(AudacityProject& project)
{
    return TrackList::Get(project).Add(Create());
}

const Track::TypeInfo& MidiTrack::ClassTypeInfo()
{
    static const TypeInfo info { { "midi", "midi", TranslatableString("midi-track", "Instrument Track") },
        true, &PlayableTrack::ClassTypeInfo() };
    return info;
}

const Track::TypeInfo& MidiTrack::GetTypeInfo() const
{
    return ClassTypeInfo();
}

void MidiTrack::SetTempo(double tempo)
{
    if (std::isfinite(tempo) && tempo > 0.0 && mTempo != tempo) {
        mTempo = tempo;
        Notify(true);
    }
}

void MidiTrack::SetGain(double gain)
{
    if (std::isfinite(gain) && gain >= 0.0 && mGain != gain) {
        mGain = gain;
        Notify(true);
    }
}

void MidiTrack::SetPan(double pan)
{
    if (std::isfinite(pan) && pan >= -1.0 && pan <= 1.0 && mPan != pan) {
        mPan = pan;
        Notify(true);
    }
}

bool MidiTrack::SetClips(std::vector<MidiClipData> clips)
{
    std::set<int64_t> ids;
    for (const auto& clip : clips) {
        if (!validClip(clip) || !ids.insert(clip.id).second) {
            return false;
        }
    }
    mClips = std::move(clips);
    Notify(true);
    return true;
}

int64_t MidiTrack::NextClipId() const
{
    int64_t result = 1;
    for (const auto& clip : mClips) {
        result = std::max(result, clip.id + 1);
    }
    return result;
}

void MidiTrack::SetInstrument(const wxString& description, const wxString& state)
{
    mInstrumentDescription = description;
    mInstrumentState = state;
    Notify(true);
}

std::shared_ptr<WideChannelGroupInterval> MidiTrack::DoGetInterval(size_t index)
{
    if (index >= mClips.size()) {
        return {};
    }
    const auto& clip = mClips[index];
    return std::make_shared<MidiInterval>(clip.start * 60.0 / mTempo, (clip.start + clip.length) * 60.0 / mTempo);
}

Track::Holder MidiTrack::Clone(bool) const
{
    auto result = std::make_shared<MidiTrack>(*this, ProtectedCreationArg {});
    result->Init(*this);
    return result;
}

Track::Holder MidiTrack::TrackEmptyCopy() const
{
    auto result = std::static_pointer_cast<MidiTrack>(Clone(false));
    result->mClips.clear();
    return result;
}

void MidiTrack::MoveTo(double time)
{
    const double delta = (time - GetStartTime()) * mTempo / 60.0;
    for (auto& clip : mClips) {
        clip.start += delta;
    }
    Notify(true);
}

void MidiTrack::ShiftBy(double time, double delta)
{
    const double beat = time * mTempo / 60.0;
    for (auto& clip : mClips) {
        if (clip.start >= beat) {
            clip.start += delta * mTempo / 60.0;
        }
    }
    Notify(true);
}

Track::Holder MidiTrack::Copy(double start, double end, bool) const
{
    auto result = std::static_pointer_cast<MidiTrack>(TrackEmptyCopy());
    const double first = start * mTempo / 60.0;
    const double last = end * mTempo / 60.0;
    result->mCopyLength = last - first;
    for (const auto& clip : mClips) {
        const double left = std::max(first, clip.start);
        const double right = std::min(last, clip.start + clip.length);
        if (left < right) {
            auto copied = slice(clip, left - clip.start, right - clip.start);
            copied.start -= first;
            result->mClips.push_back(std::move(copied));
        }
    }
    return result;
}

void MidiTrack::SplitAt(double time)
{
    const double beat = time * mTempo / 60.0;
    auto clips = mClips;
    int64_t nextId = NextClipId();
    bool changed = false;
    std::vector<MidiClipData> result;
    for (const auto& clip : clips) {
        if (beat > clip.start && beat < clip.start + clip.length) {
            result.push_back(slice(clip, 0.0, beat - clip.start));
            auto right = slice(clip, beat - clip.start, clip.length);
            right.id = nextId++;
            result.push_back(std::move(right));
            changed = true;
        } else {
            result.push_back(clip);
        }
    }
    if (changed) {
        SetClips(std::move(result));
    }
}

Track::Holder MidiTrack::Cut(double start, double end, bool moveClips)
{
    auto result = Copy(start, end);
    Clear(start, end, moveClips);
    return result;
}

void MidiTrack::Clear(double start, double end, bool moveClips)
{
    if (end <= start) {
        return;
    }
    const double first = start * mTempo / 60.0;
    const double last = end * mTempo / 60.0;
    std::vector<MidiClipData> clips;
    int64_t nextId = NextClipId();
    for (const auto& clip : mClips) {
        if (clip.start + clip.length <= first) {
            clips.push_back(clip);
        } else if (clip.start >= last) {
            auto shifted = clip;
            shifted.start -= moveClips ? last - first : 0.0;
            clips.push_back(std::move(shifted));
        } else {
            if (clip.start < first) {
                clips.push_back(slice(clip, 0.0, first - clip.start));
            }
            if (clip.start + clip.length > last) {
                auto right = slice(clip, last - clip.start, clip.length);
                right.id = nextId++;
                right.start -= moveClips ? last - first : 0.0;
                clips.push_back(std::move(right));
            }
        }
    }
    SetClips(std::move(clips));
}

void MidiTrack::Silence(double start, double end, ProgressReporter)
{
    Clear(start, end, false);
}

void MidiTrack::InsertSilence(double time, double length)
{
    if (length <= 0.0) {
        return;
    }
    const double beat = time * mTempo / 60.0;
    const double duration = length * mTempo / 60.0;
    std::vector<MidiClipData> clips;
    int64_t nextId = NextClipId();
    for (const auto& clip : mClips) {
        if (clip.start < beat && clip.start + clip.length > beat) {
            clips.push_back(slice(clip, 0.0, beat - clip.start));
            auto right = slice(clip, beat - clip.start, clip.length);
            right.id = nextId++;
            right.start += duration;
            clips.push_back(std::move(right));
        } else {
            auto shifted = clip;
            shifted.start += clip.start >= beat ? duration : 0.0;
            clips.push_back(std::move(shifted));
        }
    }
    SetClips(std::move(clips));
}

void MidiTrack::Paste(double time, const Track& source, bool moveClips)
{
    const auto* midi = track_cast<const MidiTrack*>(&source);
    if (!midi) {
        return;
    }
    if (moveClips) {
        InsertSilence(time, midi->mCopyLength * 60.0 / mTempo);
    }
    auto clips = mClips;
    int64_t nextId = NextClipId();
    for (auto clip : midi->mClips) {
        clip.id = nextId++;
        clip.start += time * mTempo / 60.0;
        clips.push_back(std::move(clip));
    }
    SetClips(std::move(clips));
}

Track::Holder MidiTrack::PasteInto(AudacityProject&, TrackList& list) const
{
    auto result = std::static_pointer_cast<MidiTrack>(Clone(false));
    list.Add(result);
    return result;
}

bool MidiTrack::HandleXMLTag(const std::string_view& tag, const AttributesList& attrs)
{
    if (tag == "miditrack") {
        for (const auto& [name, value] : attrs) {
            if (HandleCommonXMLAttribute(name, value) || PlayableTrack::HandleXMLAttribute(name, value)) {
                continue;
            }
            if (name == "tempo") {
                double tempo = 0.0;
                if (!value.TryGet(tempo) || !std::isfinite(tempo) || tempo <= 0.0) {
                    return false;
                }
                mTempo = tempo;
            } else if (name == "gain") {
                if (!value.TryGet(mGain) || !std::isfinite(mGain) || mGain < 0.0) {
                    return false;
                }
            } else if (name == "pan") {
                if (!value.TryGet(mPan) || !std::isfinite(mPan) || std::abs(mPan) > 1.0) {
                    return false;
                }
            } else if (name == "instrument") {
                mInstrumentDescription = value.ToWString();
            } else if (name == "instrumentstate") {
                mInstrumentState = value.ToWString();
            }
        }
        mClips.clear();
        return true;
    }
    if (tag == "midiclip") {
        MidiClipData clip;
        for (const auto& [name, value] : attrs) {
            if (name == "id") {
                value.TryGet(clip.id);
            } else if (name == "name") {
                clip.name = value.ToWString();
            } else if (name == "start") {
                value.TryGet(clip.start);
            } else if (name == "length") {
                value.TryGet(clip.length);
            } else if (name == "channel") {
                value.TryGet(clip.channel);
            }
        }
        if (!validClip(clip) || std::any_of(mClips.begin(), mClips.end(), [&](const auto& existing) { return existing.id == clip.id; })) {
            return false;
        }
        mClips.push_back(std::move(clip));
        return true;
    }
    if (mClips.empty()) {
        return false;
    }
    if (tag == "midinote") {
        MidiNoteData note;
        for (const auto& [name, value] : attrs) {
            if (name == "id") {
                value.TryGet(note.id);
            } else if (name == "start") {
                value.TryGet(note.start);
            } else if (name == "length") {
                value.TryGet(note.length);
            } else if (name == "pitch") {
                value.TryGet(note.pitch);
            } else if (name == "velocity") {
                value.TryGet(note.velocity);
            }
        }
        auto& clip = mClips.back();
        if (!validNote(note, clip.length)
            || std::any_of(clip.notes.begin(), clip.notes.end(), [&](const auto& existing) { return existing.id == note.id; })) {
            return false;
        }
        clip.notes.push_back(note);
        return true;
    }
    if (tag == "midicontroller") {
        MidiControllerData event;
        for (const auto& [name, value] : attrs) {
            if (name == "beat") {
                value.TryGet(event.beat);
            } else if (name == "number") {
                value.TryGet(event.number);
            } else if (name == "value") {
                value.TryGet(event.value);
            }
        }
        if (!std::isfinite(event.beat) || event.beat < 0.0 || event.beat > mClips.back().length
            || event.number < 0 || event.number > 127 || event.value < 0 || event.value > 127) {
            return false;
        }
        mClips.back().controllers.push_back(event);
        return true;
    }
    return false;
}

XMLTagHandler* MidiTrack::HandleXMLChild(const std::string_view& tag)
{
    if (tag == TrackRouting::XMLTag) {
        return &TrackRouting::Get(*this);
    }
    if (tag == AutomationData::XMLTag) {
        return &TrackAutomation::Get(*this);
    }
    if (tag == RealtimeEffectList::XMLTag()) {
        return &RealtimeEffectList::Get(*this);
    }
    return tag == "midiclip" || tag == "midinote" || tag == "midicontroller" ? this : nullptr;
}

void MidiTrack::WriteXML(XMLWriter& writer) const
{
    writer.StartTag("miditrack");
    WriteCommonXMLAttributes(writer);
    PlayableTrack::WriteXMLAttributes(writer);
    writer.WriteAttr("tempo", mTempo);
    writer.WriteAttr("gain", mGain);
    writer.WriteAttr("pan", mPan);
    writer.WriteAttr("instrument", mInstrumentDescription);
    writer.WriteAttr("instrumentstate", mInstrumentState);
    for (const auto& clip : mClips) {
        writer.StartTag("midiclip");
        writer.WriteAttr("id", static_cast<long long>(clip.id));
        writer.WriteAttr("name", clip.name);
        writer.WriteAttr("start", clip.start);
        writer.WriteAttr("length", clip.length);
        writer.WriteAttr("channel", clip.channel);
        for (const auto& note : clip.notes) {
            writer.StartTag("midinote");
            writer.WriteAttr("id", static_cast<long long>(note.id));
            writer.WriteAttr("start", note.start);
            writer.WriteAttr("length", note.length);
            writer.WriteAttr("pitch", note.pitch);
            writer.WriteAttr("velocity", note.velocity);
            writer.EndTag("midinote");
        }
        for (const auto& event : clip.controllers) {
            writer.StartTag("midicontroller");
            writer.WriteAttr("beat", event.beat);
            writer.WriteAttr("number", event.number);
            writer.WriteAttr("value", event.value);
            writer.EndTag("midicontroller");
        }
        writer.EndTag("midiclip");
    }
    TrackRouting::Get(*this).WriteXML(writer);
    TrackAutomation::Get(*this).WriteXML(writer);
    RealtimeEffectList::Get(*this).WriteXML(writer);
    writer.EndTag("miditrack");
}

using OnMidiTrackTempoChange = OnProjectTempoChange::Override<MidiTrack>;
DEFINE_ATTACHED_VIRTUAL_OVERRIDE(OnMidiTrackTempoChange)
{
    return [](MidiTrack& track, const std::optional<double>&, double tempo) { track.SetTempo(tempo); };
}
