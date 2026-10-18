/*
* Audacity: A Digital Audio Editor
*/
#include <gtest/gtest.h>
#include <limits>
#include <QTemporaryDir>
#include "au3wrap/internal/au3project.h"
#include "au3-project-history/ProjectHistory.h"
#include "au3-project-history/UndoManager.h"
#include "project/tests/testtools.h"
#include "au3-midi-track/MidiTrack.h"
#include "au3-playable-track/TrackAutomation.h"
#include "au3-realtime-effects/RealtimeEffectState.h"
#include "au3-xml/XMLFileReader.h"
#include "au3-xml/XMLWriter.h"
namespace au::au3audio {
namespace {
MidiClipData testClip()
{
    MidiClipData clip;
    clip.id = 8;
    clip.name = "Piano <one> & two";
    clip.start = 2.0;
    clip.length = 8.0;
    clip.channel = 3;
    clip.notes = { { 4, 0.5, 2.0, 60, 83 }, { 9, 4.0, 1.5, 72, 120 } };
    clip.controllers = { { 0.0, 64, 127 }, { 3.0, 64, 0 } };
    return clip;
}
}

TEST(MidiTrackTests, XmlPreservesNotesControllersAndInstrumentState)
{
    auto original = MidiTrack::Create();
    original->SetName("Instrument & one");
    original->SetTempo(93.0);
    original->SetGain(0.6);
    original->SetPan(-0.25);
    original->SetMute(true);
    original->SetSolo(true);
    original->SetInstrument("<PLUGIN name=\"Synth\"/>", "<PLUGIN type=\"4osc\"/>");
    ASSERT_TRUE(original->SetClips({ testClip() }));
    XMLStringWriter xml;
    original->WriteXML(xml);
    auto reopened = MidiTrack::Create();
    XMLFileReader reader;
    ASSERT_TRUE(reader.ParseString(reopened.get(), xml));
    EXPECT_EQ(reopened->GetName(), original->GetName());
    EXPECT_DOUBLE_EQ(reopened->GetTempo(), 93.0);
    EXPECT_DOUBLE_EQ(reopened->GetGain(), 0.6);
    EXPECT_DOUBLE_EQ(reopened->GetPan(), -0.25);
    EXPECT_TRUE(reopened->GetMute());
    EXPECT_TRUE(reopened->GetSolo());
    EXPECT_EQ(reopened->GetInstrumentDescription(), original->GetInstrumentDescription());
    EXPECT_EQ(reopened->GetInstrumentState(), original->GetInstrumentState());
    ASSERT_EQ(reopened->GetClips().size(), 1u);
    const auto& clip = reopened->GetClips().front();
    EXPECT_EQ(clip.name, testClip().name);
    EXPECT_DOUBLE_EQ(clip.start, 2.0);
    EXPECT_DOUBLE_EQ(clip.length, 8.0);
    EXPECT_EQ(clip.channel, 3);
    ASSERT_EQ(clip.notes.size(), 2u);
    EXPECT_EQ(clip.notes[0].id, 4);
    EXPECT_DOUBLE_EQ(clip.notes[0].start, 0.5);
    EXPECT_DOUBLE_EQ(clip.notes[0].length, 2.0);
    EXPECT_EQ(clip.notes[0].pitch, 60);
    EXPECT_EQ(clip.notes[0].velocity, 83);
    ASSERT_EQ(clip.controllers.size(), 2u);
    EXPECT_EQ(clip.controllers[0].value, 127);
    EXPECT_DOUBLE_EQ(clip.controllers[1].beat, 3.0);
}

TEST(MidiTrackTests, MidiVolumeDoesNotCollideWithUiSpectrogramGainAndLegacyMidiVolumeStillLoads)
{
    auto original = MidiTrack::Create();
    original->SetGain(0.37);
    XMLStringWriter xml;
    original->WriteXML(xml);
    ASSERT_NE(xml.Find("midigain="), wxNOT_FOUND);
    wxString withUi(xml);
    withUi.Replace("<miditrack ", "<miditrack gain=\"20\" ");
    auto reopened = MidiTrack::Create();
    XMLFileReader reader;
    ASSERT_TRUE(reader.ParseString(reopened.get(), withUi));
    EXPECT_DOUBLE_EQ(reopened->GetGain(), 0.37);
    wxString legacy(xml);
    legacy.Replace("midigain=", "gain=");
    XMLFileReader legacyReader;
    ASSERT_TRUE(legacyReader.ParseString(reopened.get(), legacy));
    EXPECT_DOUBLE_EQ(reopened->GetGain(), 0.37);
}

TEST(MidiTrackTests, InvalidEditDoesNotChangeExistingNotes)
{
    auto track = MidiTrack::Create();
    ASSERT_TRUE(track->SetClips({ testClip() }));
    auto clip = testClip();
    clip.notes[0].pitch = 128;
    EXPECT_FALSE(track->SetClips({ clip }));
    clip = testClip();
    clip.notes[0].length = std::numeric_limits<double>::infinity();
    EXPECT_FALSE(track->SetClips({ clip }));
    clip = testClip();
    clip.notes.push_back(clip.notes[0]);
    EXPECT_FALSE(track->SetClips({ clip }));
    EXPECT_EQ(track->GetClips()[0].notes.size(), 2u);
}

TEST(MidiTrackTests, CopyKeepsSustainedNoteAndControllerAtRangeStart)
{
    auto track = MidiTrack::Create();
    ASSERT_TRUE(track->SetClips({ testClip() }));
    const auto copy = std::static_pointer_cast<MidiTrack>(track->Copy(2.0, 3.0));
    ASSERT_EQ(copy->GetClips().size(), 1u);
    const auto& clip = copy->GetClips()[0];
    EXPECT_DOUBLE_EQ(clip.start, 0.0);
    EXPECT_DOUBLE_EQ(clip.length, 2.0);
    ASSERT_EQ(clip.notes.size(), 1u);
    EXPECT_DOUBLE_EQ(clip.notes[0].start, 0.0);
    EXPECT_DOUBLE_EQ(clip.notes[0].length, 0.5);
    ASSERT_EQ(clip.controllers.size(), 2u);
    EXPECT_DOUBLE_EQ(clip.controllers[0].beat, 0.0);
    EXPECT_EQ(clip.controllers[0].value, 127);
    EXPECT_DOUBLE_EQ(clip.controllers[1].beat, 1.0);
    EXPECT_EQ(clip.controllers[1].value, 0);
}

TEST(MidiTrackTests, ClearAndInsertSplitClipsWithoutChangingTheSnapshot)
{
    auto track = MidiTrack::Create();
    ASSERT_TRUE(track->SetClips({ testClip() }));
    const auto snapshot = std::static_pointer_cast<MidiTrack>(track->Duplicate());
    track->Clear(2.0, 3.0, true);
    ASSERT_EQ(track->GetClips().size(), 2u);
    EXPECT_DOUBLE_EQ(track->GetClips()[0].visibleLength(), 2.0);
    EXPECT_DOUBLE_EQ(track->GetClips()[1].visibleStart(), 4.0);
    EXPECT_DOUBLE_EQ(track->GetClips()[1].visibleLength(), 4.0);
    EXPECT_NE(track->GetClips()[0].id, track->GetClips()[1].id);
    EXPECT_DOUBLE_EQ(snapshot->GetClips()[0].length, 8.0);
    track->InsertSilence(3.0, 1.0);
    ASSERT_EQ(track->GetClips().size(), 3u);
    EXPECT_DOUBLE_EQ(track->GetEndTime(), 5.0);
    EXPECT_DOUBLE_EQ(snapshot->GetEndTime(), 5.0);
}

TEST(MidiTrackTests, TrimsAndClipMetadataSurviveCopyXmlAndSplittingWithoutLosingHiddenNotes)
{
    auto source = MidiTrack::Create();
    auto clip = testClip();
    clip.trimLeft = 1.0;
    clip.trimRight = 2.0;
    clip.colorIndex = 3;
    clip.groupId = 12;
    ASSERT_TRUE(source->SetClips({ clip }));
    EXPECT_DOUBLE_EQ(source->GetStartTime(), 1.5);
    EXPECT_DOUBLE_EQ(source->GetEndTime(), 4.0);
    auto copy = source->CopyClip(clip.id);
    ASSERT_NE(copy, nullptr);
    EXPECT_DOUBLE_EQ(copy->GetStartTime(), 0.0);
    EXPECT_DOUBLE_EQ(copy->GetEndTime(), 2.5);
    ASSERT_EQ(copy->GetClips().front().notes.size(), clip.notes.size());
    for (size_t i = 0; i < clip.notes.size(); ++i) {
        EXPECT_EQ(copy->GetClips().front().notes[i].id, clip.notes[i].id);
        EXPECT_DOUBLE_EQ(copy->GetClips().front().notes[i].start, clip.notes[i].start);
        EXPECT_DOUBLE_EQ(copy->GetClips().front().notes[i].length, clip.notes[i].length);
        EXPECT_EQ(copy->GetClips().front().notes[i].pitch, clip.notes[i].pitch);
        EXPECT_EQ(copy->GetClips().front().notes[i].velocity, clip.notes[i].velocity);
    }
    XMLStringWriter xml;
    copy->WriteXML(xml);
    auto reopened = MidiTrack::Create();
    XMLFileReader reader;
    ASSERT_TRUE(reader.ParseString(reopened.get(), xml));
    const auto saved = reopened->GetClips().front();
    EXPECT_DOUBLE_EQ(saved.start, -1.0);
    EXPECT_DOUBLE_EQ(saved.trimLeft, 1.0);
    EXPECT_DOUBLE_EQ(saved.trimRight, 2.0);
    EXPECT_EQ(saved.colorIndex, 3);
    EXPECT_EQ(saved.groupId, 12);
    ASSERT_EQ(saved.notes.size(), clip.notes.size());
    for (size_t i = 0; i < clip.notes.size(); ++i) {
        EXPECT_EQ(saved.notes[i].id, clip.notes[i].id);
        EXPECT_DOUBLE_EQ(saved.notes[i].start, clip.notes[i].start);
        EXPECT_DOUBLE_EQ(saved.notes[i].length, clip.notes[i].length);
        EXPECT_EQ(saved.notes[i].pitch, clip.notes[i].pitch);
        EXPECT_EQ(saved.notes[i].velocity, clip.notes[i].velocity);
    }
    source->SplitAt(2.5);
    ASSERT_EQ(source->GetClips().size(), 2u);
    EXPECT_DOUBLE_EQ(source->GetClips()[0].visibleEnd(), 5.0);
    EXPECT_DOUBLE_EQ(source->GetClips()[1].visibleStart(), 5.0);
    ASSERT_EQ(source->GetClips()[0].notes.size(), clip.notes.size());
    for (size_t i = 0; i < clip.notes.size(); ++i) {
        EXPECT_EQ(source->GetClips()[0].notes[i].id, clip.notes[i].id);
        EXPECT_DOUBLE_EQ(source->GetClips()[0].notes[i].start, clip.notes[i].start);
        EXPECT_DOUBLE_EQ(source->GetClips()[0].notes[i].length, clip.notes[i].length);
        EXPECT_EQ(source->GetClips()[0].notes[i].pitch, clip.notes[i].pitch);
        EXPECT_EQ(source->GetClips()[0].notes[i].velocity, clip.notes[i].velocity);
    }
    ASSERT_EQ(source->GetClips()[1].notes.size(), clip.notes.size());
    for (size_t i = 0; i < clip.notes.size(); ++i) {
        EXPECT_EQ(source->GetClips()[1].notes[i].id, clip.notes[i].id);
        EXPECT_DOUBLE_EQ(source->GetClips()[1].notes[i].start, clip.notes[i].start);
        EXPECT_DOUBLE_EQ(source->GetClips()[1].notes[i].length, clip.notes[i].length);
        EXPECT_EQ(source->GetClips()[1].notes[i].pitch, clip.notes[i].pitch);
        EXPECT_EQ(source->GetClips()[1].notes[i].velocity, clip.notes[i].velocity);
    }
}

TEST(AutomationDataTests, XmlPreservesEnabledStatePointsAndCurves)
{
    AutomationData data;
    ASSERT_TRUE(data.SetLane({ "volume", true, { { 0.0, 0.25, 0.0 }, { 1.0, 0.75, -0.5 } } }));
    ASSERT_TRUE(data.SetLane({ "instrument/cutoff", false, { { 2.0, 0.123456789, 1.0 } } }));
    XMLStringWriter xml;
    data.WriteXML(xml);
    AutomationData restored;
    XMLFileReader reader;
    ASSERT_TRUE(reader.ParseString(&restored, xml));
    EXPECT_EQ(restored.Lanes(), data.Lanes());
}

TEST(AutomationDataTests, InvalidPointsDoNotChangeTheLane)
{
    AutomationData data;
    const AutomationLaneData original { "volume", true, { { 0.0, 0.5, 0.0 } } };
    ASSERT_TRUE(data.SetLane(original));
    EXPECT_FALSE(data.SetLane({ "volume", true, { { -1.0, 0.5, 0.0 } } }));
    EXPECT_FALSE(data.SetLane({ "volume", true, { { 0.0, 1.01, 0.0 } } }));
    EXPECT_FALSE(data.SetLane({ "volume", true, { { 0.0, 0.0, 0.0 }, { 0.0, 1.0, 0.0 } } }));
    EXPECT_EQ(*data.Find("volume"), original);
}

TEST(AutomationDataTests, EffectClonesAndXmlKeepIndependentAutomation)
{
    const auto original = std::make_shared<RealtimeEffectState>(wxString());
    ASSERT_TRUE(original->Automation().SetLane({ "gain", true, { { 0.0, 0.25, 0.0 }, { 1.0, 0.75, 0.0 } } }));
    const auto clone = original->Clone();
    ASSERT_TRUE(clone->Automation().SetLane({ "gain", false, { { 0.0, 1.0, 0.0 } } }));
    EXPECT_TRUE(original->Automation().Find("gain")->enabled);
    EXPECT_EQ(original->Automation().Find("gain")->points.size(), 2u);
    XMLStringWriter xml;
    original->WriteXML(xml);
    const auto restored = std::make_shared<RealtimeEffectState>(wxString());
    XMLFileReader reader;
    ASSERT_TRUE(reader.ParseString(restored.get(), xml));
    EXPECT_EQ(restored->Automation().Lanes(), original->Automation().Lanes());
}

class NativeMidiProjectTests : public testing::Test
{
protected:
    void SetUp() override
    {
        ASSERT_TRUE(m_directory.isValid());
        m_path = m_directory.filePath("project.aup4").toStdString();
        const auto fixture = std::string(au3audio_tests_DATA_ROOT) + "/../../trackedit/tests/data/empty.aup4";
        ASSERT_TRUE(testtools::copyFile(fixture, m_path.toStdString()));
        m_project = std::make_shared<au3::Au3ProjectAccessor>(muse::modularity::globalCtx());
        ASSERT_TRUE(m_project->load(m_path, false));
    }

    void TearDown() override
    {
        m_project->clearSavedState();
        m_project->close();
        m_project.reset();
    }

    AudacityProject& project() { return *reinterpret_cast<AudacityProject*>(m_project->au3ProjectPtr()); }
    std::shared_ptr<MidiTrack> addTrack()
    {
        auto track = MidiTrack::Create();
        track->SetName("Instrument");
        TrackList::Get(project()).Add(track);
        return track;
    }

    QTemporaryDir m_directory;
    muse::io::path_t m_path;
    std::shared_ptr<au3::Au3ProjectAccessor> m_project;
};

TEST_F(NativeMidiProjectTests, RealProjectSaveReopenKeepsTheInstrumentTrack)
{
    auto track = addTrack();
    ASSERT_TRUE(track->SetClips({ testClip() }));
    track->SetInstrument({}, "{\"0\":0.75}");
    ASSERT_TRUE(m_project->save(m_path));
    m_project->clearSavedState();
    m_project->close();
    m_project = std::make_shared<au3::Au3ProjectAccessor>(muse::modularity::globalCtx());
    ASSERT_TRUE(m_project->load(m_path, false));
    auto tracks = TrackList::Get(project()).Any<MidiTrack>();
    ASSERT_EQ(tracks.size(), 1u);
    const auto* reopened = *tracks.begin();
    EXPECT_EQ(reopened->GetInstrumentState(), track->GetInstrumentState());
    ASSERT_EQ(reopened->GetClips().size(), 1u);
    EXPECT_EQ(reopened->GetClips()[0].notes[0].velocity, 83);
}

TEST_F(NativeMidiProjectTests, AudacityUndoRestoresNotesAndInstrumentState)
{
    auto track = addTrack();
    ASSERT_TRUE(track->SetClips({ testClip() }));
    auto& history = ProjectHistory::Get(project());
    auto& undo = UndoManager::Get(project());
    history.PushState(TranslatableString::untranslatable("Add notes"), TranslatableString::untranslatable("Notes"));
    auto clips = track->GetClips();
    clips[0].notes[0].pitch = 65;
    ASSERT_TRUE(track->SetClips(clips));
    track->SetInstrument("changed", "state");
    history.PushState(TranslatableString::untranslatable("Edit notes"), TranslatableString::untranslatable("Notes"));
    undo.Undo([&](const UndoStackElem& element) { history.PopState(element.state); });
    const auto* restored = *TrackList::Get(project()).Any<MidiTrack>().begin();
    EXPECT_EQ(restored->GetClips()[0].notes[0].pitch, 60);
    EXPECT_TRUE(restored->GetInstrumentState().empty());
    undo.Redo([&](const UndoStackElem& element) { history.PopState(element.state); });
    restored = *TrackList::Get(project()).Any<MidiTrack>().begin();
    EXPECT_EQ(restored->GetClips()[0].notes[0].pitch, 65);
    EXPECT_EQ(restored->GetInstrumentState(), "state");
}

TEST_F(NativeMidiProjectTests, InstrumentSwitchDropsOnlyItsOwnCurvesAndUndoAndFileKeepTheirOwnership)
{
    auto track = addTrack();
    ASSERT_TRUE(track->SetClips({ testClip() }));
    track->SetInstrument("synth-one", "preset-one");
    auto& data = TrackAutomation::Get(*track);
    ASSERT_TRUE(data.SetLane({ "instrument/0", true, { { 0.0, 0.25, 0.0 } } }));
    ASSERT_TRUE(data.SetLane({ "instrument/cutoff", false, { { 0.0, 0.75, 0.0 } } }));
    ASSERT_TRUE(data.SetLane({ "volume", true, { { 0.0, 0.6, 0.0 } } }));
    ASSERT_TRUE(data.SetLane({ "pan", true, { { 0.0, 0.3, 0.0 } } }));
    const auto originalLanes = data.Lanes();
    track->SetInstrument("synth-one", "another-preset");
    EXPECT_EQ(data.Lanes(), originalLanes); // Preset/manual edits keep this plugin's curves.
    auto& history = ProjectHistory::Get(project());
    auto& undo = UndoManager::Get(project());
    history.PushState(TranslatableString::untranslatable("Automated synth"), TranslatableString::untranslatable("Synth"));
    track->SetInstrument("synth-two", "preset-two");
    history.PushState(TranslatableString::untranslatable("Switch synth"), TranslatableString::untranslatable("Synth"));
    EXPECT_EQ(data.Find("instrument/0"), nullptr);
    EXPECT_EQ(data.Find("instrument/cutoff"), nullptr);
    ASSERT_EQ(data.Lanes().size(), 2u);
    EXPECT_NE(data.Find("volume"), nullptr);
    EXPECT_NE(data.Find("pan"), nullptr);
    undo.Undo([&](const UndoStackElem& element) { history.PopState(element.state); });
    auto* current = *TrackList::Get(project()).Any<MidiTrack>().begin();
    EXPECT_EQ(current->GetInstrumentDescription(), "synth-one");
    EXPECT_EQ(current->GetInstrumentState(), "another-preset");
    EXPECT_EQ(TrackAutomation::Get(*current).Lanes(), originalLanes);
    undo.Redo([&](const UndoStackElem& element) { history.PopState(element.state); });
    current = *TrackList::Get(project()).Any<MidiTrack>().begin();
    EXPECT_EQ(current->GetInstrumentDescription(), "synth-two");
    EXPECT_EQ(current->GetInstrumentState(), "preset-two");
    const auto switchedLanes = TrackAutomation::Get(*current).Lanes();
    ASSERT_EQ(switchedLanes.size(), 2u);
    ASSERT_TRUE(m_project->save(m_path));
    track.reset();
    m_project->clearSavedState();
    m_project->close();
    m_project = std::make_shared<au3::Au3ProjectAccessor>(muse::modularity::globalCtx());
    ASSERT_TRUE(m_project->load(m_path, false));
    current = *TrackList::Get(project()).Any<MidiTrack>().begin();
    EXPECT_EQ(current->GetInstrumentDescription(), "synth-two");
    EXPECT_EQ(TrackAutomation::Get(*current).Lanes(), switchedLanes);
    EXPECT_EQ(current->GetClips()[0].notes[0].pitch, 60);
}
}
