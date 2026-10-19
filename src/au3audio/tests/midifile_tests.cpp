/*
* Audacity: A Digital Audio Editor
*/
#include <gtest/gtest.h>
#include "au3-midi-track/MidiFile.h"
#include "au3-midi-track/MidiSequence.h"
#include "au3-xml/XMLFileReader.h"
#include "au3-xml/XMLWriter.h"

namespace {
using Bytes = std::vector<uint8_t>;
void integer(Bytes& bytes, uint32_t value, int count)
{
    for (int i = count - 1; i >= 0; --i) {
        bytes.push_back((value >> (i * 8)) & 0xff);
    }
}

void variable(Bytes& bytes, uint32_t value)
{
    Bytes result { static_cast<uint8_t>(value & 0x7f) };
    while ((value >>= 7)) {
        result.insert(result.begin(), static_cast<uint8_t>((value & 0x7f) | 0x80));
    }
    bytes.insert(bytes.end(), result.begin(), result.end());
}

void event(Bytes& bytes, uint32_t delta, std::initializer_list<uint8_t> message)
{
    variable(bytes, delta);
    bytes.insert(bytes.end(), message);
}

void chunk(Bytes& bytes, const char* name, const Bytes& body)
{
    bytes.insert(bytes.end(), name, name + 4);
    integer(bytes, static_cast<uint32_t>(body.size()), 4);
    bytes.insert(bytes.end(), body.begin(), body.end());
}

Bytes file(std::vector<Bytes> tracks, int format = 0, int division = 480)
{
    Bytes header, result;
    integer(header, format, 2);
    integer(header, static_cast<uint32_t>(tracks.size()), 2);
    integer(header, division, 2);
    chunk(result, "MThd", header);
    for (const auto& track : tracks) {
        chunk(result, "MTrk", track);
    }
    return result;
}

Bytes phrase()
{
    Bytes track;
    event(track, 480, { 0x90, 60, 100 });
    event(track, 480, { 60, 0 }); // Running status and velocity-zero note-off.
    event(track, 480, { 0xff, 0x2f, 0 });
    return track;
}

bool contains(const std::vector<MidiEvent>& events, int status, int first, int second)
{
    return std::any_of(events.begin(), events.end(), [&](const auto& e) {
        return e.status == status && e.data1 == first && e.data2 == second;
    });
}
}

TEST(MidiFileTests, TypeZeroRunningStatusPreservesLeadingAndTrailingSilence)
{
    const auto result = ReadMidiFile(file({ phrase() }));
    ASSERT_TRUE(result.success) << result.error;
    ASSERT_EQ(result.tracks.size(), 1u);
    EXPECT_DOUBLE_EQ(result.tempo, 120.0);
    EXPECT_DOUBLE_EQ(result.duration, 1.5);
    const auto& clip = result.tracks[0]->GetClips()[0];
    EXPECT_DOUBLE_EQ(clip.start, 0.0);
    EXPECT_DOUBLE_EQ(clip.length, 3.0);
    ASSERT_EQ(clip.notes.size(), 1u);
    EXPECT_DOUBLE_EQ(clip.notes[0].start, 1.0);
    EXPECT_DOUBLE_EQ(clip.notes[0].length, 1.0);
    EXPECT_EQ(clip.notes[0].velocity, 100);
}

TEST(MidiFileTests, TypeOneTempoMapPreservesSecondsInAnExistingProject)
{
    Bytes conductor, notes;
    event(conductor, 0, { 0xff, 0x51, 3, 0x09, 0x27, 0xc0 }); // 100 BPM.
    event(conductor, 0, { 0xff, 0x58, 4, 7, 3, 24, 8 });
    event(conductor, 480, { 0xff, 0x51, 3, 0x04, 0x93, 0xe0 }); // 200 BPM.
    event(conductor, 960, { 0xff, 0x2f, 0 });
    event(notes, 0, { 0xff, 3, 5, 'P', 'i', 'a', 'n', 'o' });
    event(notes, 0, { 0x90, 60, 90 });
    event(notes, 480, { 0x80, 60, 0 });
    event(notes, 480, { 0x90, 64, 100 });
    event(notes, 480, { 0x80, 64, 0 });
    event(notes, 0, { 0xff, 0x2f, 0 });
    const auto bytes = file({ conductor, notes }, 1);
    const auto result = ReadMidiFile(bytes, 120);
    ASSERT_TRUE(result.success) << result.error;
    EXPECT_DOUBLE_EQ(result.tempo, 100.0);
    EXPECT_EQ(result.numerator, 7);
    EXPECT_EQ(result.denominator, 8);
    ASSERT_EQ(result.tracks.size(), 1u); // No empty conductor instrument track.
    EXPECT_EQ(result.tracks[0]->GetName(), "Piano");
    EXPECT_DOUBLE_EQ(result.tracks[0]->GetTempo(), 120.0);
    const auto& clip = result.tracks[0]->GetClips()[0];
    ASSERT_EQ(clip.notes.size(), 2u);
    EXPECT_NEAR(clip.notes[0].length, 1.2, 1e-12);
    EXPECT_NEAR(clip.notes[1].start, 1.8, 1e-12);
    EXPECT_NEAR(clip.notes[1].length, 0.6, 1e-12);
    EXPECT_NEAR(result.duration, 1.2, 1e-12);
    const auto fresh = ReadMidiFile(bytes);
    ASSERT_TRUE(fresh.success);
    EXPECT_DOUBLE_EQ(fresh.tracks[0]->GetTempo(), 100.0);
    EXPECT_NEAR(fresh.tracks[0]->GetClips()[0].notes[1].start, 1.5, 1e-12);
}

TEST(MidiFileTests, SplitsChannelsAndKeepsDistinctFileTracks)
{
    Bytes first, second;
    event(first, 0, { 0x90, 60, 80 });
    event(first, 0, { 0x99, 36, 120 });
    event(first, 480, { 0x80, 60, 0 });
    event(first, 0, { 0x89, 36, 0 });
    event(first, 0, { 0xff, 0x2f, 0 });
    event(second, 0, { 0x90, 67, 90 });
    event(second, 480, { 0x80, 67, 0 });
    event(second, 0, { 0xff, 0x2f, 0 });
    const auto result = ReadMidiFile(file({ first, second }, 1));
    ASSERT_TRUE(result.success);
    ASSERT_EQ(result.tracks.size(), 3u);
    EXPECT_EQ(result.tracks[0]->GetClips()[0].channel, 1);
    EXPECT_EQ(result.tracks[1]->GetClips()[0].channel, 10);
    EXPECT_EQ(result.tracks[2]->GetClips()[0].channel, 1);
    EXPECT_EQ(result.tracks[1]->GetClips()[0].notes[0].pitch, 36);
}

TEST(MidiFileTests, OverlappingIdenticalPitchesPairInArrivalOrderAndDanglingNotesEndAtTrackEnd)
{
    Bytes track;
    event(track, 0, { 0x90, 60, 80 });
    event(track, 240, { 60, 90 });
    event(track, 240, { 0x80, 60, 0 });
    event(track, 480, { 0xff, 0x2f, 0 });
    const auto result = ReadMidiFile(file({ track }));
    ASSERT_TRUE(result.success);
    const auto& notes = result.tracks[0]->GetClips()[0].notes;
    ASSERT_EQ(notes.size(), 2u);
    EXPECT_DOUBLE_EQ(notes[0].length, 1.0);
    EXPECT_DOUBLE_EQ(notes[1].start, 0.5);
    EXPECT_DOUBLE_EQ(notes[1].length, 1.5);
}

TEST(MidiFileTests, SupportsSmpteIncludingDropFrameTiming)
{
    for (const auto [division, duration] : { std::pair{ 0xe728, 3.0 }, std::pair{ 0xe364, 1.001 } }) {
        Bytes track;
        event(track, 0, { 0x90, 60, 100 });
        event(track, 3000, { 0x80, 60, 0 });
        event(track, 0, { 0xff, 0x2f, 0 });
        const auto result = ReadMidiFile(file({ track }, 0, division));
        ASSERT_TRUE(result.success) << result.error;
        EXPECT_NEAR(result.duration, duration, 1e-12);
    }
}

TEST(MidiFileTests, ExpressionProgramAndBankStateSurviveXmlTrimmingAndSeek)
{
    Bytes track;
    event(track, 0, { 0xb2, 0, 2 });
    event(track, 0, { 32, 3 });
    event(track, 0, { 0xc2, 42 });
    event(track, 0, { 0xe2, 0, 80 });
    event(track, 0, { 0xd2, 23 });
    event(track, 0, { 0x92, 60, 100 });
    event(track, 240, { 0xa2, 60, 70 });
    event(track, 720, { 0x82, 60, 0 });
    event(track, 0, { 0xff, 0x2f, 0 });
    const auto result = ReadMidiFile(file({ track }));
    ASSERT_TRUE(result.success) << result.error;
    auto original = result.tracks[0];
    XMLStringWriter xml;
    original->WriteXML(xml);
    auto restored = MidiTrack::Create();
    XMLFileReader reader;
    ASSERT_TRUE(reader.ParseString(restored.get(), xml));
    MidiSequence sequence(*restored, 48000);
    std::vector<MidiEvent> chased;
    sequence.Chase(24000, chased);
    EXPECT_TRUE(contains(chased, 0xb2, 0, 2));
    EXPECT_TRUE(contains(chased, 0xb2, 32, 3));
    EXPECT_TRUE(contains(chased, 0xc2, 42, 0));
    EXPECT_TRUE(contains(chased, 0xe2, 0, 80));
    EXPECT_TRUE(contains(chased, 0xd2, 23, 0));
    EXPECT_TRUE(contains(chased, 0xa2, 60, 70));
    auto clips = restored->GetClips();
    clips[0].trimLeft = 1.0;
    ASSERT_TRUE(restored->SetClips(clips));
    MidiSequence trimmed(*restored, 48000);
    EXPECT_TRUE(contains(trimmed.Events(), 0xc2, 42, 0));
    EXPECT_TRUE(contains(trimmed.Events(), 0xe2, 0, 80));
    auto copy = std::dynamic_pointer_cast<MidiTrack>(restored->Copy(0.5, 1.0));
    ASSERT_TRUE(copy);
    MidiSequence copied(*copy, 48000);
    EXPECT_TRUE(contains(copied.Events(), 0xc2, 42, 0));
    EXPECT_TRUE(contains(copied.Events(), 0xe2, 0, 80));
    const auto bank = std::find_if(copied.Events().begin(), copied.Events().end(), [](const auto& event) {
        return event.status == 0xb2 && event.data1 == 32;
    });
    const auto program
        = std::find_if(copied.Events().begin(), copied.Events().end(), [](const auto& event) { return event.status == 0xc2; });
    EXPECT_LT(bank, program);
}

TEST(MidiFileTests, ResetControllersRestoresExpressionBeforeSeekTrimAndCopy)
{
    Bytes track;
    event(track, 0, { 0xc0, 42 });
    event(track, 0, { 0xe0, 0, 80 });
    event(track, 0, { 0xd0, 23 });
    event(track, 0, { 0x90, 60, 100 });
    event(track, 240, { 0xb0, 121, 0 });
    event(track, 720, { 0x80, 60, 0 });
    event(track, 0, { 0xff, 0x2f, 0 });
    const auto result = ReadMidiFile(file({ track }));
    ASSERT_TRUE(result.success);
    MidiSequence original(*result.tracks[0], 48000);
    std::vector<MidiEvent> events;
    original.Chase(24000, events);
    EXPECT_TRUE(contains(events, 0xc0, 42, 0));
    EXPECT_TRUE(contains(events, 0xe0, 0, 64));
    EXPECT_TRUE(contains(events, 0xd0, 0, 0));
    auto copy = std::dynamic_pointer_cast<MidiTrack>(result.tracks[0]->Copy(0.5, 1.0));
    ASSERT_TRUE(copy);
    MidiSequence copied(*copy, 48000);
    EXPECT_TRUE(contains(copied.Events(), 0xc0, 42, 0));
    EXPECT_TRUE(contains(copied.Events(), 0xe0, 0, 64));
    EXPECT_FALSE(contains(copied.Events(), 0xe0, 0, 80));
    EXPECT_TRUE(contains(copied.Events(), 0xd0, 0, 0));
    auto clips = result.tracks[0]->GetClips();
    clips[0].trimLeft = 1;
    ASSERT_TRUE(result.tracks[0]->SetClips(clips));
    MidiSequence trimmed(*result.tracks[0], 48000);
    EXPECT_TRUE(contains(trimmed.Events(), 0xe0, 0, 64));
    EXPECT_FALSE(contains(trimmed.Events(), 0xe0, 0, 80));
}

TEST(MidiFileTests, SkipsUnknownChunksAndSystemExclusiveWithoutLosingNotes)
{
    auto bytes = file({ phrase() });
    Bytes unknown;
    chunk(unknown, "JUNK", { 1, 2, 3 });
    bytes.insert(bytes.begin() + 14, unknown.begin(), unknown.end());
    ASSERT_TRUE(ReadMidiFile(bytes).success);
    Bytes track;
    event(track, 0, { 0xf0, 3, 0x7e, 0, 0xf7 });
    const auto notes = phrase();
    track.insert(track.end(), notes.begin(), notes.end());
    const auto result = ReadMidiFile(file({ track }));
    ASSERT_TRUE(result.success);
    EXPECT_TRUE(result.hasSystemExclusive);
    EXPECT_EQ(result.tracks[0]->GetClips()[0].notes.size(), 1u);
}

TEST(MidiFileTests, TruncatedAndUnsupportedFilesNeverReturnPartialTracks)
{
    const auto valid = file({ phrase() });
    for (size_t end = 0; end < valid.size(); ++end) {
        const auto result = ReadMidiFile(std::span(valid).first(end));
        EXPECT_FALSE(result.success) << end;
        EXPECT_TRUE(result.tracks.empty()) << end;
    }
    for (const auto& bytes : {
        file({ phrase() }, 2), file({ phrase() }, 0, 0), file({ phrase(), phrase() }, 0),
        file({ Bytes { 0, 60, 100, 0, 0xff, 0x2f, 0 } }),
        file({ Bytes { 0x81, 0x80, 0x80, 0x80, 0, 0xff, 0x2f, 0 } }),
        file({ Bytes { 0, 0x90, 128, 100, 0, 0xff, 0x2f, 0 } }),
        file({ Bytes { 0, 0xff, 0x51, 3, 0, 0, 0, 0, 0xff, 0x2f, 0 } }),
        file({ phrase(), Bytes { 0, 0x90 } }, 1) }) {
        const auto result = ReadMidiFile(bytes);
        EXPECT_FALSE(result.success);
        EXPECT_FALSE(result.error.empty());
        EXPECT_TRUE(result.tracks.empty());
    }
}
