/*
* Audacity: A Digital Audio Editor
*/
#include <tracktion_engine/tracktion_engine.h>
#include <gtest/gtest.h>
#include <gmock/gmock.h>
#include <QEventLoop>
#include <QQuickWindow>
#include <chrono>

#include "au3-midi-track/MidiTrack.h"
#include "au3-playable-track/TrackAutomation.h"
#include "au3-playable-track/TrackRouting.h"
#include "trackedit/view/trackroutingmodel.h"
#include "trackedit/view/trackfreezemodel.h"
#include "au3-wave-track/TrackFreeze.h"
#include "au3-wave-track/WaveTrackUtilities.h"
#include <thread>
#include <set>
#include "au3-realtime-effects/RealtimeEffectState.h"
#include "au3-wave-track/WaveTrack.h"
#include "au3-wave-track/WaveClip.h"
#include "automation/view/trackautomationmodel.h"
#include "au3-numeric-formats/ProjectTimeSignature.h"
#include "au3-project-history/ProjectHistory.h"
#include "au3-project-history/UndoManager.h"
#include "au3-xml/XMLFileReader.h"
#include "au3-xml/XMLWriter.h"
#include "au3wrap/internal/au3project.h"
#include "project/tests/testtools.h"
#include "../internal/tracktionruntime.h"
#include "../internal/audacityaudiosource.h"
#include "../internal/tracktionsession.h"
#include "../internal/tracktionrendersource.h"
#include "../internal/tracktionmixsource.h"
#include "../internal/tracktioninstrumenteditor.h"
#include "../internal/tracktioninstrument.h"
#include "trackedit/internal/tracksviewrequestsservice.h"
#include "../internal/tracktionpluginscanner.h"
#include "../internal/tracktionplugindescriptions.h"
#include "au3-import-export/ExportPluginHelpers.h"
#include "au3-import-export/ExportUtils.h"
#include "effects/effects_base/internal/effectsutils.h"
#include "trackedit/view/pianorollmodel.h"
#include "trackedit/view/instrumentview.h"
#include "trackedit/internal/au3/au3projecthistory.h"
#include "trackedit/tests/mocks/trackeditprojectmock.h"
#include "context/tests/mocks/globalcontextmock.h"
#include "context/tests/mocks/playbackstatemock.h"
#include "project/tests/mocks/audacityprojectmock.h"
#include "au3audio/tests/mocks/audioenginemock.h"
#include "audioplugins/tests/mocks/knownaudiopluginsregistermock.h"
#include "testing/testcontext.h"

namespace au::tracktionaudio {
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
    EXPECT_DOUBLE_EQ(track->GetClips()[0].length, 2.0);
    EXPECT_DOUBLE_EQ(track->GetClips()[1].start, 4.0);
    EXPECT_DOUBLE_EQ(track->GetClips()[1].length, 4.0);
    EXPECT_NE(track->GetClips()[0].id, track->GetClips()[1].id);
    EXPECT_DOUBLE_EQ(snapshot->GetClips()[0].length, 8.0);
    track->InsertSilence(3.0, 1.0);
    ASSERT_EQ(track->GetClips().size(), 3u);
    EXPECT_DOUBLE_EQ(track->GetEndTime(), 5.0);
    EXPECT_DOUBLE_EQ(snapshot->GetEndTime(), 5.0);
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

class MidiProjectTests : public testing::Test
{
protected:
    void SetUp() override
    {
        m_directory = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("audacity-midi-" + juce::Uuid().toString());
        ASSERT_TRUE(m_directory.createDirectory().wasOk());
        m_path = m_directory.getChildFile("project.aup4").getFullPathName().toStdString();
        const auto fixture = std::string(tracktionaudio_tests_DATA_ROOT) + "/../../trackedit/tests/data/empty.aup4";
        ASSERT_TRUE(testtools::copyFile(fixture, m_path.toStdString()));
        m_project = std::make_shared<au3::Au3ProjectAccessor>(muse::modularity::globalCtx());
        ASSERT_TRUE(m_project->load(m_path, false));
        m_runtime.init(m_directory.getChildFile("cache").getFullPathName().toStdString(),
                       m_directory.getChildFile("settings").getFullPathName().toStdString());
    }

    void TearDown() override
    {
        m_runtime.deinit();
        m_project->clearSavedState();
        m_project->close();
        m_project.reset();
        m_directory.deleteRecursively();
    }

    AudacityProject& project() { return *reinterpret_cast<AudacityProject*>(m_project->au3ProjectPtr()); }
    std::shared_ptr<MidiTrack> addTrack()
    {
        auto track = MidiTrack::Create();
        track->SetName("Instrument");
        TrackList::Get(project()).Add(track);
        return track;
    }

    juce::File m_directory;
    muse::io::path_t m_path;
    std::shared_ptr<au3::Au3ProjectAccessor> m_project;
    TracktionRuntime m_runtime;
};

TEST_F(MidiProjectTests, RealProjectSaveReopenKeepsTheInstrumentTrack)
{
    auto track = addTrack();
    ASSERT_TRUE(track->SetClips({ testClip() }));
    track->SetInstrument("<PLUGIN name=\"Test\"/>", "<PLUGIN type=\"4osc\"/>");
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

TEST_F(MidiProjectTests, AudacityUndoRestoresNotesAndInstrumentState)
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

TEST_F(MidiProjectTests, TracktionReceivesNotesVelocitySustainAndChannel)
{
    auto track = addTrack();
    ASSERT_TRUE(track->SetClips({ testClip() }));
    TransportSequences sequences;
    sequences.otherPlayableSequences.push_back(track);
    TracktionSession session(m_runtime, TrackList::Get(project()), sequences, TracktionSession::Role::Render);
    const auto tracks = tracktion::getAudioTracks(session.edit());
    ASSERT_EQ(tracks.size(), 1);
    ASSERT_EQ(tracks[0]->getClips().size(), 1);
    const auto* clip = dynamic_cast<tracktion::MidiClip*>(tracks[0]->getClips()[0]);
    ASSERT_NE(clip, nullptr);
    EXPECT_EQ(clip->getMidiChannel().getChannelNumber(), 3);
    const auto& sequence = clip->getSequence();
    ASSERT_EQ(sequence.getNotes().size(), 2);
    EXPECT_EQ(sequence.getNotes()[0]->getVelocity(), 83);
    EXPECT_EQ(sequence.getNotes()[1]->getNoteNumber(), 72);
    ASSERT_EQ(sequence.getControllerEvents().size(), 2);
    EXPECT_EQ(sequence.getControllerEvents()[0]->getControllerValue(), 127 << 7);
    EXPECT_EQ(sequence.getControllerEvents()[1]->getControllerValue(), 0);
}

TEST_F(MidiProjectTests, RestoredInstrumentsHaveUniqueTracktionIdentifiers)
{
    TracktionInstrumentEditorFactory factory;
    const auto editor = factory.create({}, {});
    ASSERT_TRUE(editor.ret);
    const auto state = wxString::FromUTF8(editor.val->state());
    addTrack()->SetInstrument({}, state);
    addTrack()->SetInstrument({}, state);
    TracktionSession session(m_runtime, TrackList::Get(project()), TracktionSession::Role::Render);
    std::set<tracktion::EditItemID> ids;
    for (const auto* track : tracktion::getAudioTracks(session.edit())) {
        EXPECT_TRUE(ids.insert(track->itemID).second);
        for (const auto* plugin : track->pluginList.getPlugins()) {
            EXPECT_TRUE(ids.insert(plugin->itemID).second);
        }
    }
}

TEST_F(MidiProjectTests, InstrumentRendersAudioAndMuteProducesSilence)
{
    auto track = addTrack();
    MidiClipData clip;
    clip.id = 1;
    clip.notes = { { 1, 0.0, 1.0, 60, 100 } };
    ASSERT_TRUE(track->SetClips({ clip }));
    const auto render = [&]() {
        TracktionSession session(m_runtime, TrackList::Get(project()), TracktionSession::Role::Render);
        TracktionRenderSource source(session, { 0.0, 2.0, 48000.0, 2, 512 });
        float peak = 0.0f;
        while (source.process(512) > 0) {
            peak = std::max(peak, source.buffer().getMagnitude(0, source.buffer().getNumSamples()));
        }
        return peak;
    };
    EXPECT_GT(render(), 0.001f);
    track->SetMute(true);
    EXPECT_FLOAT_EQ(render(), 0.0f);
}

TEST_F(MidiProjectTests, VstInstrumentReceivesVelocityAndSustainAndRestoresParameters)
{
    TracktionPluginMetaReader reader(PluginFormat::VST3);
    const auto metadata = reader.readMeta(AU_TRACKTION_TEST_INSTRUMENT_PATH);
    ASSERT_TRUE(metadata.ret);
    ASSERT_EQ(metadata.val.size(), 1u);
    const auto& meta = metadata.val[0];
    EXPECT_EQ(meta.attributeVal(effects::EFFECT_TYPE_ATTRIBUTE), u"Instrument");
    const auto converted = effects::utils::museToAuEffectMeta(AU_TRACKTION_TEST_INSTRUMENT_PATH, meta);
    const auto savedMeta = effects::utils::auToMuseEffectMeta(converted);
    EXPECT_EQ(savedMeta.attributeVal(TRACKTION_PLUGIN_DESCRIPTION_ATTRIBUTE), meta.attributeVal(TRACKTION_PLUGIN_DESCRIPTION_ATTRIBUTE));
    const auto description = meta.attributeVal(TRACKTION_PLUGIN_DESCRIPTION_ATTRIBUTE).toStdString();
    auto track = addTrack();
    track->SetInstrument(wxString::FromUTF8(description), {});
    MidiClipData clip;
    clip.id = 1;
    clip.notes = { { 1, 0.25, 0.5, 60, 80 } };
    clip.controllers = { { 0.5, 64, 127 }, { 1.5, 64, 0 } };
    ASSERT_TRUE(track->SetClips({ clip }));
    const auto render = [&]() {
        TracktionSession session(m_runtime, TrackList::Get(project()), TracktionSession::Role::Render);
        TracktionRenderSource source(session, { 0.0, 1.5, 48000.0, 2, 512 });
        std::vector<float> samples;
        while (const int count = source.process(512)) {
            const auto* data = source.buffer().getReadPointer(0);
            samples.insert(samples.end(), data, data + count);
        }
        return samples;
    };
    const auto original = render();
    ASSERT_GT(original.size(), 50000u);
    EXPECT_FLOAT_EQ(original[2000], 0.0f);
    EXPECT_GT(original[10000], 0.01f);
    EXPECT_NEAR(original[10000], original[28000], 1e-6f);
    EXPECT_FLOAT_EQ(original[50000], 0.0f);

    TracktionInstrumentEditorFactory factory;
    const auto editor = factory.create(description, {});
    ASSERT_TRUE(editor.ret);
    std::string gainId;
    for (const auto& parameter : editor.val->parameters()) {
        if (parameter.name == "Gain") {
            gainId = parameter.id;
        }
    }
    ASSERT_FALSE(gainId.empty());
    ASSERT_TRUE(editor.val->setParameter(gainId, 0.25));
    const auto state = editor.val->state();
    track->SetInstrument(wxString::FromUTF8(description), wxString::FromUTF8(state));
    const auto restored = factory.create(description, state);
    ASSERT_TRUE(restored.ret);
    bool gainRestored = false;
    for (const auto& parameter : restored.val->parameters()) {
        if (parameter.id == gainId) {
            EXPECT_NEAR(parameter.value, 0.25, 1e-6);
            gainRestored = true;
        }
    }
    EXPECT_TRUE(gainRestored);
    const auto adjusted = render();
    ASSERT_EQ(adjusted.size(), original.size());
    EXPECT_NEAR(adjusted[10000], original[10000] * 0.5f, 1e-6f);
    EXPECT_NEAR(adjusted[28000], original[28000] * 0.5f, 1e-6f);
}

TEST_F(MidiProjectTests, ExportMixerIncludesSelectedMidiAndHonorsSolo)
{
    auto track = addTrack();
    MidiClipData clip;
    clip.id = 1;
    clip.notes = { { 1, 0.0, 1.0, 60, 100 } };
    ASSERT_TRUE(track->SetClips({ clip }));
    auto other = addTrack();
    ASSERT_TRUE(other->SetClips({ clip }));
    other->SetSolo(true);
    track->SetSelected(true);
    EXPECT_TRUE(ExportUtils::FindExportMidiTracks(TrackList::Get(project()), true).empty());
    other->SetSolo(false);
    AudioRenderSource::Factory::Scope hook([this](const AudacityProject& project, const AudioRenderSource::Tracks& tracks,
                                                  const AudioRenderSource::Options& options) {
        EXPECT_EQ(options.midiTracks.size(), 1u);
        return std::make_unique<TracktionMixSource>(m_runtime, project, tracks, options);
    });
    auto mixer = ExportPluginHelpers::CreateMixer(project(), true, 0.0, 2.0, 2, 512, false, 48000.0, floatSample, nullptr);
    size_t count = 0;
    float peak = 0.0f;
    while (const auto frames = mixer->Process()) {
        const auto* samples = reinterpret_cast<const float*>(mixer->GetBuffer(0));
        for (size_t i = 0; i < frames; ++i) {
            peak = std::max(peak, std::abs(samples[i]));
        }
        count += frames;
    }
    EXPECT_EQ(count, 96000u);
    EXPECT_GT(peak, 0.001f);
}

TEST_F(MidiProjectTests, CapturingInstrumentStateDoesNotRequestAnotherCapture)
{
    TracktionPluginMetaReader reader(PluginFormat::VST3);
    const auto metadata = reader.readMeta(AU_TRACKTION_TEST_INSTRUMENT_PATH);
    ASSERT_TRUE(metadata.ret);
    auto track = addTrack();
    track->SetInstrument(wxString::FromUTF8(metadata.val[0].attributeVal(TRACKTION_PLUGIN_DESCRIPTION_ATTRIBUTE).toStdString()), {});
    TracktionSession session(m_runtime, TrackList::Get(project()), TracktionSession::Role::Render);
    auto* native = session.instrumentTrack(*track);
    ASSERT_NE(native, nullptr);
    const auto editor = createLiveInstrumentEditor(session.edit(), *native);
    editor->takeChanges();
    const auto initial = editor->state();
    ASSERT_FALSE(initial.empty());
    EXPECT_FALSE(editor->takeChanges());

    auto* plugin = dynamic_cast<tracktion::ExternalPlugin*>(native->pluginList[0]);
    ASSERT_NE(plugin, nullptr);
    auto* processor = plugin->getAudioPluginInstance();
    ASSERT_NE(processor, nullptr);
    juce::AudioProcessorParameter* gain = nullptr;
    for (auto* parameter : processor->getParameters()) {
        if (parameter->getName(64) == "Gain") {
            gain = parameter;
        }
    }
    ASSERT_NE(gain, nullptr);
    gain->setValueNotifyingHost(0.25f);
    EXPECT_TRUE(editor->takeChanges());
    const auto changed = editor->state();
    EXPECT_NE(changed, initial);
    EXPECT_FALSE(editor->takeChanges());
    EXPECT_EQ(editor->state(), changed);
    EXPECT_FALSE(editor->takeChanges());

    gain->setValueNotifyingHost(0.75f);
    EXPECT_TRUE(editor->takeChanges());
}

TEST_F(MidiProjectTests, InstalledInstrumentStateCaptureDoesNotRequestAnotherCapture)
{
    const auto path = qEnvironmentVariable("AU_TRACKTION_TEST_INSTRUMENT_PLUGIN");
    if (path.isEmpty()) {
        GTEST_SKIP() << "Set AU_TRACKTION_TEST_INSTRUMENT_PLUGIN to an installed VST3 instrument.";
    }
    TracktionPluginMetaReader reader(PluginFormat::VST3);
    const auto metadata = reader.readMeta(path.toStdString());
    ASSERT_TRUE(metadata.ret);
    ASSERT_FALSE(metadata.val.empty());
    auto track = addTrack();
    track->SetInstrument(wxString::FromUTF8(metadata.val[0].attributeVal(TRACKTION_PLUGIN_DESCRIPTION_ATTRIBUTE).toStdString()), {});
    TracktionSession session(m_runtime, TrackList::Get(project()), TracktionSession::Role::Render);
    auto* native = session.instrumentTrack(*track);
    ASSERT_NE(native, nullptr);
    const auto editor = createLiveInstrumentEditor(session.edit(), *native);
    TracktionRenderSource source(session, { 0.0, 3.0, 48000.0, 2, 512 });
    const auto pump = [] {
        QEventLoop events;
        QTimer::singleShot(100, &events, &QEventLoop::quit);
        events.exec();
    };
    source.process(512);
    pump();
    editor->takeChanges();
    for (int capture = 0; capture < 3; ++capture) {
        SCOPED_TRACE(capture);
        ASSERT_FALSE(editor->state().empty());
        pump();
        EXPECT_FALSE(editor->takeChanges());
    }
}

TEST_F(MidiProjectTests, LiveKeyboardNotesAndSustainReachTheInstrument)
{
    TracktionPluginMetaReader reader(PluginFormat::VST3);
    const auto metadata = reader.readMeta(AU_TRACKTION_TEST_INSTRUMENT_PATH);
    ASSERT_TRUE(metadata.ret);
    auto track = addTrack();
    track->SetInstrument(wxString::FromUTF8(metadata.val[0].attributeVal(TRACKTION_PLUGIN_DESCRIPTION_ATTRIBUTE).toStdString()), {});
    MidiClipData clip;
    clip.id = 1;
    ASSERT_TRUE(track->SetClips({ clip }));
    TracktionSession session(m_runtime, TrackList::Get(project()), TracktionSession::Role::Render);
    auto* native = session.instrumentTrack(*track);
    ASSERT_NE(native, nullptr);
    const auto editor = createLiveInstrumentEditor(session.edit(), *native);
    TracktionRenderSource source(session, { 0.0, 3.0, 48000.0, 2, 512 });
    const auto peak = [&]() {
        float level = 0.0f;
        for (int block = 0; block < 8; ++block) {
            source.process(512);
            level = source.buffer().getMagnitude(0, source.buffer().getNumSamples());
        }
        return level;
    };
    EXPECT_FLOAT_EQ(peak(), 0.0f);
    editor->sendMidi(0x90, 60, 100);
    const float velocity100 = peak();
    editor->takeChanges();
    peak();
    EXPECT_FALSE(editor->takeChanges());
    EXPECT_GT(velocity100, 0.01f);
    editor->sendMidi(0xb0, 64, 127);
    editor->sendMidi(0x80, 60, 0);
    EXPECT_NEAR(peak(), velocity100, 1e-6f);
    editor->sendMidi(0xb0, 64, 0);
    EXPECT_FLOAT_EQ(peak(), 0.0f);
    editor->sendMidi(0x90, 60, 50);
    EXPECT_NEAR(peak(), velocity100 * 0.5f, 1e-6f);
    editor->releaseNotes();
    EXPECT_FLOAT_EQ(peak(), 0.0f);
    editor->close();
    EXPECT_FALSE(editor->active());
    editor->sendMidi(0x90, 60, 100);
    EXPECT_TRUE(editor->parameters().empty());
}

TEST_F(MidiProjectTests, ClosingTheInstrumentEditorKeepsClipPlaybackActive)
{
    TracktionPluginMetaReader reader(PluginFormat::VST3);
    const auto metadata = reader.readMeta(AU_TRACKTION_TEST_INSTRUMENT_PATH);
    ASSERT_TRUE(metadata.ret);
    auto track = addTrack();
    track->SetInstrument(wxString::FromUTF8(metadata.val[0].attributeVal(TRACKTION_PLUGIN_DESCRIPTION_ATTRIBUTE).toStdString()), {});
    MidiClipData clip;
    clip.id = 1;
    clip.length = 8;
    clip.notes = { { 1, 0.0, 6.0, 60, 100 } };
    ASSERT_TRUE(track->SetClips({ clip }));
    TracktionSession session(m_runtime, TrackList::Get(project()), TracktionSession::Role::Render);
    auto* native = session.instrumentTrack(*track);
    const auto editor = createLiveInstrumentEditor(session.edit(), *native);
    TracktionRenderSource source(session, { 0.0, 3.0, 48000.0, 2, 512 });
    source.process(512);
    source.process(512);
    const float before = source.buffer().getMagnitude(0, source.buffer().getNumSamples());
    ASSERT_GT(before, 0.01f);
    auto* external = dynamic_cast<tracktion::ExternalPlugin*>(native->pluginList[0]);
    ASSERT_NE(external, nullptr);
    auto* gain = external->getAudioPluginInstance()->getParameters()[0];
    editor->takeChanges();
    gain->beginChangeGesture();
    gain->setValueNotifyingHost(0.25f);
    EXPECT_FALSE(editor->takeChanges());
    gain->endChangeGesture();
    EXPECT_TRUE(editor->takeChanges());
    EXPECT_FALSE(editor->state().empty());
    editor->close();
    for (int block = 0; block < 8; ++block) {
        source.process(512);
    }
    EXPECT_NEAR(source.buffer().getMagnitude(0, source.buffer().getNumSamples()), before * 0.5f, 1e-6f);
}

TEST_F(MidiProjectTests, BusRoutingSurvivesRealProjectSaveReopenAndUndo)
{
    const auto midi = addTrack();
    const auto bus = WaveTrackFactory::Get(project()).Create(2, floatSample, 48000);
    TrackRouting::Get(*bus).busId = "stable-bus-id";
    bus->SetName("Bus");
    TrackList::Get(project()).Add(bus);
    auto& history = ProjectHistory::Get(project());
    auto& undo = UndoManager::Get(project());
    history.PushState(TranslatableString::untranslatable("Before routing"), TranslatableString::untranslatable("Routing"));
    TrackRouting::Get(*midi).output = "stable-bus-id";
    TrackRouting::Get(*midi).sends = { { "stable-bus-id", 0.4321, true } };
    history.PushState(TranslatableString::untranslatable("After routing"), TranslatableString::untranslatable("Routing"));
    undo.Undo([&](const UndoStackElem& element) { history.PopState(element.state); });
    EXPECT_TRUE(TrackRouting::Get(**TrackList::Get(project()).Any<MidiTrack>().begin()).output.empty());
    undo.Redo([&](const UndoStackElem& element) { history.PopState(element.state); });
    ASSERT_TRUE(m_project->save(m_path));
    m_project->clearSavedState();
    m_project->close();
    m_project = std::make_shared<au3::Au3ProjectAccessor>(muse::modularity::globalCtx());
    ASSERT_TRUE(m_project->load(m_path, false));
    const auto waves = TrackList::Get(project()).Any<WaveTrack>();
    const auto midis = TrackList::Get(project()).Any<MidiTrack>();
    ASSERT_EQ(waves.size(), 1u);
    ASSERT_EQ(midis.size(), 1u);
    EXPECT_EQ(TrackRouting::Get(**waves.begin()).busId, "stable-bus-id");
    const auto& route = TrackRouting::Get(**midis.begin());
    EXPECT_EQ(route.output, "stable-bus-id");
    ASSERT_EQ(route.sends.size(), 1u);
    EXPECT_DOUBLE_EQ(route.sends[0].gain, 0.4321);
    EXPECT_TRUE(route.sends[0].preFader);
}

TEST_F(MidiProjectTests, AutomationSurvivesRealProjectSaveAndReopenForAudioAndMidi)
{
    const auto midi = addTrack();
    ASSERT_TRUE(midi->SetClips({ testClip() }));
    const auto wave = WaveTrackFactory::Get(project()).Create(1, floatSample, 48000);
    TrackList::Get(project()).Add(wave);
    ASSERT_TRUE(TrackAutomation::Get(*midi).SetLane({ "instrument/cutoff", true, { { 0.0, 0.2, 0.0 }, { 2.0, 0.8, 0.0 } } }));
    ASSERT_TRUE(TrackAutomation::Get(*wave).SetLane({ "volume", false, { { 0.0, 0.5, 0.0 } } }));
    const auto midiData = TrackAutomation::Get(*midi).Lanes();
    const auto waveData = TrackAutomation::Get(*wave).Lanes();
    ASSERT_TRUE(m_project->save(m_path));
    m_project->clearSavedState();
    m_project->close();
    m_project = std::make_shared<au3::Au3ProjectAccessor>(muse::modularity::globalCtx());
    ASSERT_TRUE(m_project->load(m_path, false));
    const auto midiTracks = TrackList::Get(project()).Any<MidiTrack>();
    const auto waveTracks = TrackList::Get(project()).Any<WaveTrack>();
    ASSERT_EQ(midiTracks.size(), 1u);
    ASSERT_EQ(waveTracks.size(), 1u);
    EXPECT_EQ(TrackAutomation::Get(**midiTracks.begin()).Lanes(), midiData);
    EXPECT_EQ(TrackAutomation::Get(**waveTracks.begin()).Lanes(), waveData);
}

TEST_F(MidiProjectTests, InstrumentAutomationChangesTheRenderedPluginParameter)
{
    TracktionPluginMetaReader reader(PluginFormat::VST3);
    const auto metadata = reader.readMeta(AU_TRACKTION_TEST_INSTRUMENT_PATH);
    ASSERT_TRUE(metadata.ret);
    const auto description = metadata.val[0].attributeVal(TRACKTION_PLUGIN_DESCRIPTION_ATTRIBUTE).toStdString();
    TracktionInstrumentEditorFactory factory;
    const auto editor = factory.create(description, {});
    ASSERT_TRUE(editor.ret);
    std::string gainId;
    for (const auto& parameter : editor.val->parameters()) {
        if (parameter.name == "Gain") {
            gainId = parameter.id;
        }
    }
    ASSERT_FALSE(gainId.empty());
    const auto track = addTrack();
    track->SetInstrument(wxString::FromUTF8(description), {});
    MidiClipData clip;
    clip.id = 1;
    clip.length = 8;
    clip.notes = { { 1, 0.0, 7.0, 60, 127 } };
    ASSERT_TRUE(track->SetClips({ clip }));
    ASSERT_TRUE(TrackAutomation::Get(*track).SetLane({ "instrument/" + gainId, true,
                                                       { { 0.0, 0.25, 0.0 }, { 2.0, 0.75, 0.0 } } }));
    TracktionSession session(m_runtime, TrackList::Get(project()), TracktionSession::Role::Render);
    TracktionRenderSource source(session, { 0.0, 2.0, 48000.0, 2, 512 });
    std::vector<float> samples;
    while (const int count = source.process(512)) {
        const auto* data = source.buffer().getReadPointer(0);
        samples.insert(samples.end(), data, data + count);
    }
    ASSERT_GT(samples.size(), 72000u);
    EXPECT_GT(samples[72000], samples[24000] * 1.5f);
}

namespace {
class InputTestEditor final : public audio::IInstrumentEditor
{
public:
    std::vector<audio::InstrumentParameter> parameters() const override { return {}; }
    bool setParameter(const std::string&, double) override { return false; }
    std::string state() const override { ++stateReads; return savedState; }
    bool takeChanges() override { return std::exchange(dirty, false); }
    bool active() const override { return m_active; }
    void close() override { m_active = false; }
    std::vector<audio::InstrumentMidiMessage> takeMidiMessages() override
    {
        return std::exchange(messages, {});
    }

    std::vector<audio::InstrumentMidiMessage> messages;
    bool m_active = true;
    bool dirty = false;
    mutable int stateReads = 0;
    std::string savedState;
};

class InputTestPlayback final : public audio::IInstrumentPlayback
{
public:
    muse::RetVal<std::shared_ptr<audio::IInstrumentEditor> > openInstrument(AudacityProject&, MidiTrack&) override
    {
        editor = std::make_shared<InputTestEditor>();
        return muse::RetVal<std::shared_ptr<audio::IInstrumentEditor> >::make_ok(editor);
    }

    void closeInstrument(const std::shared_ptr<audio::IInstrumentEditor>& instance) override { instance->close(); }
    std::shared_ptr<InputTestEditor> editor;
};
}

class PianoRollTests : public MidiProjectTests
{
protected:
    void SetUp() override
    {
        MidiProjectTests::SetUp();
        m_context = testutils::makeTestContext();
        m_globalContext = std::make_shared<testing::NiceMock<context::GlobalContextMock> >();
        m_playbackState = std::make_shared<testing::NiceMock<context::PlaybackStateMock> >();
        m_projectMock = std::make_shared<testing::NiceMock<project::AudacityProjectMock> >();
        m_trackeditProject = std::make_shared<testing::NiceMock<trackedit::TrackeditProjectMock> >();
        m_audio = std::make_shared<testing::NiceMock<audio::AudioEngineMock> >();
        m_known = std::make_shared<testing::NiceMock<muse::audioplugins::KnownAudioPluginsRegisterMock> >();
        ON_CALL(*m_projectMock, au3ProjectPtr()).WillByDefault(testing::Return(m_project->au3ProjectPtr()));
        ON_CALL(*m_globalContext, currentProject()).WillByDefault(testing::Return(m_projectMock));
        ON_CALL(*m_globalContext, playbackState()).WillByDefault(testing::Return(m_playbackState));
        ON_CALL(*m_globalContext, currentTrackeditProject()).WillByDefault(testing::Return(m_trackeditProject));
        muse::modularity::ioc(m_context)->registerExport<context::IGlobalContext>("midi-tests", m_globalContext);
        m_history = std::make_shared<trackedit::Au3ProjectHistory>(m_context);
        muse::modularity::ioc(m_context)->registerExport<trackedit::IProjectHistory>("midi-tests", m_history);
        muse::modularity::globalIoc()->registerExport<audio::IAudioEngine>("midi-tests", m_audio);
        muse::modularity::globalIoc()->registerExport<muse::audioplugins::IKnownAudioPluginsRegister>("midi-tests", m_known);
        m_inputs = std::make_shared<InputTestPlayback>();
        muse::modularity::globalIoc()->registerExport<audio::IInstrumentPlayback>("midi-tests", m_inputs);
        muse::modularity::ioc(m_context)->registerExport<trackedit::ITracksViewRequestsService>("midi-tests",
                                                                                                new trackedit::TracksViewRequestsService(
                                                                                                    m_context));
        m_history->init();
        auto track = addTrack();
        m_model = std::make_unique<trackedit::PianoRollModel>(m_context);
        m_model->setTrackId(QVariant::fromValue<long>(track->GetId()));
        m_model->init();
        ASSERT_GT(m_model->addClip(0.0, 4.0), 0);
    }

    void TearDown() override
    {
        m_model.reset();
        muse::modularity::ioc(m_context)->unregister<trackedit::IProjectHistory>("midi-tests");
        muse::modularity::ioc(m_context)->unregister<context::IGlobalContext>("midi-tests");
        m_history.reset();
        muse::modularity::ioc(m_context)->unregister<trackedit::ITracksViewRequestsService>("midi-tests");
        muse::modularity::globalIoc()->unregister<audio::IInstrumentPlayback>("midi-tests");
        m_inputs.reset();
        m_context.reset();
        muse::modularity::globalIoc()->unregister<audio::IAudioEngine>("midi-tests");
        muse::modularity::globalIoc()->unregister<muse::audioplugins::IKnownAudioPluginsRegister>("midi-tests");
        MidiProjectTests::TearDown();
    }

    muse::modularity::ContextPtr m_context;
    std::shared_ptr<testing::NiceMock<context::GlobalContextMock> > m_globalContext;
    std::shared_ptr<testing::NiceMock<context::PlaybackStateMock> > m_playbackState;
    std::shared_ptr<testing::NiceMock<project::AudacityProjectMock> > m_projectMock;
    std::shared_ptr<testing::NiceMock<trackedit::TrackeditProjectMock> > m_trackeditProject;
    std::shared_ptr<testing::NiceMock<audio::AudioEngineMock> > m_audio;
    std::shared_ptr<testing::NiceMock<muse::audioplugins::KnownAudioPluginsRegisterMock> > m_known;
    std::shared_ptr<trackedit::Au3ProjectHistory> m_history;
    std::shared_ptr<InputTestPlayback> m_inputs;
    std::unique_ptr<trackedit::PianoRollModel> m_model;
};

TEST(InstrumentViewTests, RemovingTheViewFromItsWindowDoesNotCallTheDestroyedDerivedObject)
{
    QQuickWindow window;
    auto view = std::make_unique<trackedit::InstrumentView>(window.contentItem());
    EXPECT_EQ(view->window(), &window);
    QPointer<trackedit::InstrumentView> reference(view.get());
    view.reset();
    EXPECT_TRUE(reference.isNull());
}

TEST_F(PianoRollTests, PlaybackCursorUsesTheSelectedClipOffsetWithoutReloadingNotes)
{
    double seconds = 3.0;
    muse::async::Channel<muse::secs_t> positionChanged;
    ON_CALL(*m_playbackState, playbackPosition()).WillByDefault([&] { return muse::secs_t(seconds); });
    ON_CALL(*m_playbackState, playbackPositionChanged()).WillByDefault(testing::Return(positionChanged));
    m_model->initPanel();
    const auto clipId = m_model->clipId();
    m_model->moveClip(clipId, 2.0);
    EXPECT_DOUBLE_EQ(m_model->playbackBeat(), 2.0);

    int cursorChanges = 0;
    int documentChanges = 0;
    QObject::connect(m_model.get(), &trackedit::PianoRollModel::playbackPositionChanged, m_model.get(), [&] { ++cursorChanges; });
    QObject::connect(m_model.get(), &trackedit::PianoRollModel::dataChanged, m_model.get(), [&] { ++documentChanges; });
    seconds = 2.5;
    positionChanged.send(muse::secs_t(seconds));
    EXPECT_DOUBLE_EQ(m_model->playbackBeat(), 1.0);
    EXPECT_EQ(cursorChanges, 1);
    EXPECT_EQ(documentChanges, 0);
    seconds = 1.5;
    positionChanged.send(muse::secs_t(seconds));
    EXPECT_LT(m_model->playbackBeat(), 0.0);
    seconds = 5.0;
    positionChanged.send(muse::secs_t(seconds));
    EXPECT_GT(m_model->playbackBeat(), m_model->clipLength());
    EXPECT_EQ(documentChanges, 0);

    m_model->undo();
    EXPECT_DOUBLE_EQ(m_model->playbackBeat(), 10.0);
    EXPECT_GT(cursorChanges, 3);
    auto* track = *TrackList::Get(project()).Any<MidiTrack>().begin();
    track->SetTempo(90.0);
    EXPECT_DOUBLE_EQ(m_model->playbackBeat(), 7.5);
    m_model->removeClip(clipId);
    EXPECT_DOUBLE_EQ(m_model->playbackBeat(), -1.0);
}

TEST_F(PianoRollTests, NoteEditsSurviveTrackReplacementDuringUndoRedo)
{
    m_model->addNote(0.25, 0.5, 64, 83);
    ASSERT_EQ(m_model->notes().size(), 1);
    const auto id = m_model->notes()[0].toMap().value("id").toLongLong();
    m_model->editNote(id, 1.0, 1.5, 67, 110);
    ASSERT_EQ(m_model->notes().size(), 1);
    EXPECT_EQ(m_model->notes()[0].toMap().value("pitch").toInt(), 67);
    m_model->undo();
    EXPECT_EQ(m_model->notes()[0].toMap().value("pitch").toInt(), 64);
    EXPECT_EQ(m_model->notes()[0].toMap().value("velocity").toInt(), 83);
    m_model->undo();
    EXPECT_TRUE(m_model->notes().empty());
    m_model->redo();
    ASSERT_EQ(m_model->notes().size(), 1);
    m_model->editNote(id, 2.0, 0.5, 72, 90);
    EXPECT_EQ(m_model->notes()[0].toMap().value("pitch").toInt(), 72);
    m_model->removeNote(id);
    EXPECT_TRUE(m_model->notes().empty());
}

TEST_F(PianoRollTests, EditingDoesNotStopAnotherProjectsAudioStream)
{
    ON_CALL(*m_audio, isBusy()).WillByDefault(testing::Return(true));
    ON_CALL(*m_audio, canStopAudioStream(testing::_)).WillByDefault(testing::Return(false));
    EXPECT_CALL(*m_audio, stopStream()).Times(0);
    const auto historyIndex = m_history->currentStateIndex();
    m_model->addNote(0.0, 1.0, 60, 100);
    EXPECT_TRUE(m_model->notes().empty());
    EXPECT_EQ(m_history->currentStateIndex(), historyIndex);
    EXPECT_FALSE(m_model->error().isEmpty());
}

TEST_F(PianoRollTests, RecordingPreventsDocumentEdits)
{
    ON_CALL(*m_globalContext, isRecording()).WillByDefault(testing::Return(true));
    EXPECT_FALSE(m_model->editable());
    const auto historyIndex = m_history->currentStateIndex();
    m_model->addNote(0.0, 1.0, 60, 100);
    m_model->removeClip(m_model->clipId());
    EXPECT_TRUE(m_model->notes().empty());
    EXPECT_EQ(m_model->clips().size(), 1);
    EXPECT_EQ(m_history->currentStateIndex(), historyIndex);
}
TEST_F(PianoRollTests, OpenEditorRequestsThePanelInItsOwnContext)
{
    trackedit::PianoRollModel panel(m_context);
    panel.initPanel();
    int requests = 0;
    QObject::connect(&panel, &trackedit::PianoRollModel::editorRequested, &panel, [&] { ++requests; });
    m_model->openEditor(m_model->clipId());
    EXPECT_EQ(requests, 1);
    EXPECT_EQ(panel.trackId(), m_model->trackId());
    EXPECT_EQ(panel.clipId(), m_model->clipId());
}

TEST_F(PianoRollTests, KeyboardTakeUsesOneUndoStepAndKeepsExistingNotes)
{
    m_model->addNote(2.0, 0.5, 72, 90);
    const auto historyIndex = m_history->currentStateIndex();
    m_model->startInputRecording();
    ASSERT_TRUE(m_model->inputRecording());
    m_model->noteOn(60, 83);
    m_model->noteOn(64, 101);
    EXPECT_EQ(m_model->heldNotes().size(), 2);
    m_model->noteOff(60);
    m_model->stopInputRecording();
    EXPECT_FALSE(m_model->inputRecording());
    EXPECT_TRUE(m_model->heldNotes().empty());
    ASSERT_EQ(m_model->notes().size(), 3);
    EXPECT_EQ(m_history->currentStateIndex(), historyIndex + 1);
    EXPECT_EQ(m_model->notes()[1].toMap().value("pitch").toInt(), 60);
    EXPECT_EQ(m_model->notes()[1].toMap().value("velocity").toInt(), 83);
    m_model->undo();
    ASSERT_EQ(m_model->notes().size(), 1);
    EXPECT_EQ(m_model->notes()[0].toMap().value("pitch").toInt(), 72);
    m_model->redo();
    EXPECT_EQ(m_model->notes().size(), 3);
}

TEST_F(PianoRollTests, MidiInputTakePreservesVelocityPedalAndExtendsTheClip)
{
    m_model->startInputRecording();
    ASSERT_TRUE(m_model->inputRecording());
    const double time = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
    m_inputs->editor->messages = { { 0x92, 60, 87, time }, { 0xb2, 64, 127, time + 0.25 },
        { 0x82, 60, 0, time + 3.0 }, { 0xb2, 64, 0, time + 3.5 } };
    QEventLoop loop;
    QTimer::singleShot(30, &loop, &QEventLoop::quit);
    loop.exec();
    m_model->stopInputRecording();
    ASSERT_EQ(m_model->notes().size(), 1);
    const auto note = m_model->notes()[0].toMap();
    EXPECT_EQ(note.value("velocity").toInt(), 87);
    EXPECT_NEAR(note.value("length").toDouble(), 6.0, 0.02);
    EXPECT_GE(m_model->clipLength(), 7.0);
    ASSERT_EQ(m_model->controllers().size(), 2);
    EXPECT_EQ(m_model->controllers()[0].toMap().value("value").toInt(), 127);
    EXPECT_EQ(m_model->controllers()[1].toMap().value("value").toInt(), 0);
}

TEST_F(PianoRollTests, ClosingThePanelFinishesHeldNotesAndTheTake)
{
    m_model->startInputRecording();
    m_model->noteOn(64, 97);
    const auto editor = m_inputs->editor;
    m_model->disableInput();
    EXPECT_FALSE(editor->active());
    EXPECT_FALSE(m_model->inputRecording());
    ASSERT_EQ(m_model->notes().size(), 1);
    EXPECT_GT(m_model->notes()[0].toMap().value("length").toDouble(), 0.0);
}
TEST_F(PianoRollTests, TransportChangesReconnectTheInstrumentInput)
{
    ASSERT_TRUE(m_model->enableInput());
    const auto previous = m_inputs->editor;
    previous->close();
    QEventLoop loop;
    QTimer::singleShot(30, &loop, &QEventLoop::quit);
    loop.exec();
    ASSERT_NE(m_inputs->editor, previous);
    EXPECT_TRUE(m_inputs->editor->active());
    m_model->disableInput();
    EXPECT_FALSE(m_inputs->editor->active());
}
TEST_F(PianoRollTests, AutomationEditsUseAudacityUndoAndKeepNotes)
{
    au::automation::TrackAutomationModel model(m_context);
    model.setTrackId(m_model->trackId().toLongLong());
    model.init();
    model.setPoint(-1, 0.0, 0.25);
    model.setPoint(-1, 2.0, 0.75);
    ASSERT_EQ(model.points().size(), 2);
    const auto index = m_history->currentStateIndex();
    model.setPoint(1, 3.0, 0.5);
    EXPECT_EQ(m_history->currentStateIndex(), index + 1);
    EXPECT_DOUBLE_EQ(model.points()[1].toMap().value("time").toDouble(), 3.0);
    m_history->undo();
    ASSERT_EQ(model.points().size(), 2);
    EXPECT_DOUBLE_EQ(model.points()[1].toMap().value("time").toDouble(), 2.0);
    m_history->redo();
    EXPECT_DOUBLE_EQ(model.points()[1].toMap().value("time").toDouble(), 3.0);
    EXPECT_TRUE(m_model->notes().empty());
    model.setLaneEnabled(false);
    EXPECT_FALSE(model.laneEnabled());
    m_history->undo();
    EXPECT_TRUE(model.laneEnabled());
}

TEST_F(PianoRollTests, AutomationDoesNotStopAnotherProjectsStreamOrEditDuringRecording)
{
    au::automation::TrackAutomationModel model(m_context);
    model.setTrackId(m_model->trackId().toLongLong());
    model.init();
    audio::AudioStreamDescriptor stream;
    stream.ownerProject = nullptr;
    ON_CALL(*m_audio, currentStream()).WillByDefault(testing::Return(stream));
    EXPECT_CALL(*m_audio, stopStream()).Times(0);
    model.setPoint(-1, 0.0, 0.25);
    EXPECT_TRUE(model.points().empty());
    EXPECT_FALSE(model.error().isEmpty());
    ON_CALL(*m_audio, currentStream()).WillByDefault(testing::Return(std::nullopt));
    ON_CALL(*m_globalContext, isRecording()).WillByDefault(testing::Return(true));
    model.setPoint(-1, 0.0, 0.25);
    EXPECT_TRUE(model.points().empty());
}
TEST_F(PianoRollTests, SelectingAnAutomationPointDoesNotStopPlaybackOrAddHistory)
{
    au::automation::TrackAutomationModel model(m_context);
    model.setTrackId(m_model->trackId().toLongLong());
    model.init();
    model.setPoint(-1, 0.0, 0.5);
    const auto index = m_history->currentStateIndex();
    audio::AudioStreamDescriptor stream;
    stream.ownerProject = &project();
    ON_CALL(*m_audio, currentStream()).WillByDefault(testing::Return(stream));
    EXPECT_CALL(*m_audio, stopStream()).Times(0);
    model.setPoint(0, 0.0, 0.5);
    EXPECT_EQ(m_history->currentStateIndex(), index);
}
TEST_F(PianoRollTests, RoutingControlsUseAudacityUndoAndRejectFeedback)
{
    trackedit::TrackRoutingModel model(m_context);
    model.setTrackId(m_model->trackId().toLongLong());
    model.init();
    ASSERT_EQ(model.buses().size(), 1);
    model.addBus();
    ASSERT_EQ(model.buses().size(), 2);
    model.setOutput(1);
    EXPECT_EQ(model.outputIndex(), 1);
    model.setSend(1, true, 0.375, true);
    ASSERT_EQ(model.sends().size(), 1);
    EXPECT_DOUBLE_EQ(model.sends()[0].toMap().value("gain").toDouble(), 0.375);
    m_history->undo();
    EXPECT_FALSE(model.sends()[0].toMap().value("enabled").toBool());
    m_history->redo();
    EXPECT_TRUE(model.sends()[0].toMap().value("enabled").toBool());
    const auto firstBus = *TrackList::Get(project()).Any<WaveTrack>().begin();
    model.setTrackId(firstBus->GetId());
    model.addBus();
    model.setOutput(1);
    const auto lastBus = *TrackList::Get(project()).Any<WaveTrack>().rbegin();
    model.setTrackId(lastBus->GetId());
    const auto state = m_history->currentStateIndex();
    model.setSend(1, true, 1.0, false);
    EXPECT_FALSE(model.error().isEmpty());
    EXPECT_EQ(m_history->currentStateIndex(), state);
    EXPECT_TRUE(TrackRouting::Validate(TrackList::Get(project())));
}

TEST_F(PianoRollTests, RoutingControlsDoNotStopAnotherProjectsStream)
{
    trackedit::TrackRoutingModel model(m_context);
    model.setTrackId(m_model->trackId().toLongLong());
    model.init();
    audio::AudioStreamDescriptor stream;
    stream.ownerProject = nullptr;
    ON_CALL(*m_audio, currentStream()).WillByDefault(testing::Return(stream));
    EXPECT_CALL(*m_audio, stopStream()).Times(0);
    model.addBus();
    EXPECT_EQ(TrackList::Get(project()).Any<WaveTrack>().size(), 0u);
    EXPECT_FALSE(model.error().isEmpty());
}

namespace {
bool waitForFreeze(trackedit::TrackFreezeModel& model)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
    while (model.busy() && std::chrono::steady_clock::now() < deadline) {
        QCoreApplication::processEvents();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return !model.busy();
}
}

TEST_F(PianoRollTests, InstrumentFreezeUsesNativeAudioAndRestoresSourceAfterSaveReopen)
{
    TracktionPluginMetaReader reader(PluginFormat::VST3);
    const auto metadata = reader.readMeta(AU_TRACKTION_TEST_INSTRUMENT_PATH);
    ASSERT_TRUE(metadata.ret);
    const auto description = metadata.val[0].attributeVal(TRACKTION_PLUGIN_DESCRIPTION_ATTRIBUTE).toStdString();
    auto* midi = *TrackList::Get(project()).Any<MidiTrack>().begin();
    midi->SetInstrument(wxString::FromUTF8(description), {});
    m_model->addNote(0.0, 2.0, 64, 100);
    midi->SetGain(0.5);
    midi->SetPan(0.25);
    ASSERT_TRUE(TrackAutomation::Get(*midi).SetLane({ "volume", true, { { 0.0, 0.25, 0.0 } } }));
    const auto notes = midi->GetClips()[0].notes;
    const auto automation = TrackAutomation::Get(*midi).Lanes();
    AudioRenderSource::Factory::Scope hook([this](const AudacityProject& project, const AudioRenderSource::Tracks& tracks,
                                                  const AudioRenderSource::Options& options) {
        EXPECT_FALSE(options.trackRouting);
        return std::make_unique<TracktionMixSource>(m_runtime, project, tracks, options);
    });
    trackedit::TrackFreezeModel model(m_context);
    model.setTrackId(midi->GetId());
    model.init();
    ASSERT_TRUE(model.canFreeze());
    model.freeze(0.0);
    ASSERT_TRUE(waitForFreeze(model));
    ASSERT_TRUE(model.error().isEmpty()) << model.error().toStdString();
    ASSERT_TRUE(model.frozen());
    EXPECT_TRUE(TrackList::Get(project()).Any<MidiTrack>().empty());
    auto* wave = *TrackList::Get(project()).Any<WaveTrack>().begin();
    ASSERT_EQ(wave->Intervals().size(), 1u);
    EXPECT_EQ(TrackAutomation::Get(*wave).Lanes(), automation);
    EXPECT_DOUBLE_EQ(wave->GetVolume(), 0.5);
    AudioRenderSource::Options options;
    options.stopTime = 0.5;
    options.sampleRate = 48000;
    TracktionMixSource frozenAudio(m_runtime, project(), { wave->SharedPointer<WaveTrack>() }, options);
    ASSERT_GT(frozenAudio.Process(), 0u);
    EXPECT_GT(reinterpret_cast<const float*>(frozenAudio.GetBuffer(0))[256], 0.01f);
    m_history->undo();
    EXPECT_FALSE(model.frozen());
    EXPECT_EQ(TrackList::Get(project()).Any<MidiTrack>().size(), 1u);
    m_history->redo();
    EXPECT_TRUE(model.frozen());
    ASSERT_TRUE(m_project->save(m_path));
    m_project->clearSavedState();
    m_project->close();
    m_project = std::make_shared<au3::Au3ProjectAccessor>(muse::modularity::globalCtx());
    ASSERT_TRUE(m_project->load(m_path, false));
    ON_CALL(*m_projectMock, au3ProjectPtr()).WillByDefault(testing::Return(m_project->au3ProjectPtr()));
    wave = *TrackList::Get(project()).Any<WaveTrack>().begin();
    model.setTrackId(wave->GetId());
    ASSERT_TRUE(model.frozen());
    wave->SetVolume(0.75);
    model.unfreeze();
    ASSERT_TRUE(model.error().isEmpty()) << model.error().toStdString();
    ASSERT_EQ(TrackList::Get(project()).Any<MidiTrack>().size(), 1u);
    midi = *TrackList::Get(project()).Any<MidiTrack>().begin();
    ASSERT_EQ(midi->GetClips()[0].notes.size(), notes.size());
    EXPECT_EQ(midi->GetClips()[0].notes[0].pitch, 64);
    EXPECT_EQ(midi->GetClips()[0].notes[0].velocity, 100);
    EXPECT_EQ(midi->GetInstrumentDescription().utf8_string(), description);
    EXPECT_DOUBLE_EQ(midi->GetGain(), 0.75);
    EXPECT_EQ(TrackAutomation::Get(*midi).Lanes(), automation);
}

TEST_F(PianoRollTests, AudioFreezeKeepsOriginalSampleBlocksAfterSaveReopen)
{
    auto source = WaveTrackFactory::Get(project()).Create(2, floatSample, 48000);
    const auto clip = source->CreateClip(0.0, "Original");
    const std::vector<float> left(4800, 0.125f), right(4800, -0.25f);
    constSamplePtr input[] { reinterpret_cast<constSamplePtr>(left.data()), reinterpret_cast<constSamplePtr>(right.data()) };
    clip->Append(input, floatSample, left.size(), 1, floatSample);
    clip->Flush();
    source->InsertInterval(clip, true);
    TrackList::Get(project()).Add(source);
    source->SetVolume(0.5);
    WaveTrackUtilities::SampleBlockIDSet originalIds;
    WaveTrackUtilities::InspectBlocks(TrackList::Get(project()), {}, &originalIds);
    AudioRenderSource::Factory::Scope hook([this](const AudacityProject& project, const AudioRenderSource::Tracks& tracks,
                                                  const AudioRenderSource::Options& options) {
        return std::make_unique<TracktionMixSource>(m_runtime, project, tracks, options);
    });
    trackedit::TrackFreezeModel model(m_context);
    model.init();
    model.setTrackId(source->GetId());
    model.freeze(0.0);
    ASSERT_TRUE(waitForFreeze(model));
    ASSERT_TRUE(model.frozen()) << model.error().toStdString();
    WaveTrackUtilities::SampleBlockIDSet frozenIds;
    WaveTrackUtilities::InspectBlocks(TrackList::Get(project()), {}, &frozenIds);
    for (const auto id : originalIds) {
        EXPECT_TRUE(frozenIds.contains(id));
    }
    source.reset();
    ASSERT_TRUE(m_project->save(m_path));
    m_project->clearSavedState();
    m_project->close();
    m_project = std::make_shared<au3::Au3ProjectAccessor>(muse::modularity::globalCtx());
    ASSERT_TRUE(m_project->load(m_path, false));
    ON_CALL(*m_projectMock, au3ProjectPtr()).WillByDefault(testing::Return(m_project->au3ProjectPtr()));
    auto* frozen = *TrackList::Get(project()).Any<WaveTrack>().begin();
    model.setTrackId(frozen->GetId());
    ASSERT_TRUE(model.frozen());
    ASSERT_FALSE(TrackFreeze::Get(*frozen).blocks.empty());
    model.unfreeze();
    ASSERT_TRUE(model.error().isEmpty()) << model.error().toStdString();
    auto* restored = *TrackList::Get(project()).Any<WaveTrack>().begin();
    EXPECT_EQ(restored->NChannels(), 2u);
    EXPECT_TRUE(TrackFreeze::Get(*restored).sourceXml.empty());
    ASSERT_EQ(restored->Intervals().size(), 1u);
    AudacityAudioSource audio(**restored->Intervals().begin());
    juce::AudioBuffer<float> samples(2, 32);
    ASSERT_TRUE(audio.read(0, samples.getWritePointer(0), 100, 32));
    ASSERT_TRUE(audio.read(1, samples.getWritePointer(1), 100, 32));
    EXPECT_FLOAT_EQ(samples.getSample(0, 0), 0.125f);
    EXPECT_FLOAT_EQ(samples.getSample(1, 0), -0.25f);
    EXPECT_DOUBLE_EQ(restored->GetVolume(), 0.5);
}

TEST_F(PianoRollTests, FreezeCancellationAndStreamOwnershipKeepTheDocumentUnchanged)
{
    m_model->addNote(0.0, 1.0, 60, 100);
    AudioRenderSource::Factory::Scope hook([this](const AudacityProject& project, const AudioRenderSource::Tracks& tracks,
                                                  const AudioRenderSource::Options& options) {
        return std::make_unique<TracktionMixSource>(m_runtime, project, tracks, options);
    });
    trackedit::TrackFreezeModel model(m_context);
    model.init();
    model.setTrackId(m_model->trackId().toLongLong());
    const auto state = m_history->currentStateIndex();
    audio::AudioStreamDescriptor stream;
    stream.ownerProject = nullptr;
    ON_CALL(*m_audio, currentStream()).WillByDefault(testing::Return(stream));
    EXPECT_CALL(*m_audio, stopStream()).Times(0);
    model.freeze(0.0);
    EXPECT_FALSE(model.busy());
    EXPECT_FALSE(model.error().isEmpty());
    ON_CALL(*m_audio, currentStream()).WillByDefault(testing::Return(std::nullopt));
    model.freeze(0.0);
    model.cancel();
    ASSERT_TRUE(waitForFreeze(model));
    EXPECT_FALSE(model.frozen());
    EXPECT_EQ(TrackList::Get(project()).Any<MidiTrack>().size(), 1u);
    EXPECT_EQ(m_history->currentStateIndex(), state);
}

TEST_F(PianoRollTests, FreezeSavesPendingInstrumentSettingsBeforeClosingLiveInput)
{
    m_model->addNote(0.0, 1.0, 60, 100);
    ASSERT_TRUE(m_model->enableInput());
    ASSERT_TRUE(m_inputs->editor);
    const std::string pending = "<PLUGIN type=\"4osc\" pendingFreezeTest=\"1\"/>";
    m_inputs->editor->savedState = pending;
    m_inputs->editor->dirty = true;
    AudioRenderSource::Factory::Scope hook([this](const AudacityProject& project, const AudioRenderSource::Tracks& tracks,
                                                  const AudioRenderSource::Options& options) {
        return std::make_unique<TracktionMixSource>(m_runtime, project, tracks, options);
    });
    trackedit::TrackFreezeModel model(m_context);
    model.init();
    model.setTrackId(m_model->trackId().toLongLong());
    model.freeze(0.0);
    ASSERT_TRUE(waitForFreeze(model));
    ASSERT_TRUE(model.frozen()) << model.error().toStdString();
    model.unfreeze();
    ASSERT_EQ(TrackList::Get(project()).Any<MidiTrack>().size(), 1u);
    EXPECT_EQ((*TrackList::Get(project()).Any<MidiTrack>().begin())->GetInstrumentState().utf8_string(), pending);
}
TEST_F(PianoRollTests, FrozenSourceCopiesKeepSamplesWhenTheStorageFactoryChanges)
{
    auto source = WaveTrackFactory::Get(project()).Create(1, floatSample, 48000);
    auto clip = source->CreateClip(0.0, "Original source");
    const std::vector<float> samples(480, 0.375f);
    constSamplePtr input[] { reinterpret_cast<constSamplePtr>(samples.data()) };
    clip->Append(input, floatSample, samples.size(), 1, floatSample);
    clip->Flush();
    source->InsertInterval(clip, true);
    XMLStringWriter xml;
    xml.StartTag("frozen-source");
    source->WriteXML(xml);
    xml.EndTag("frozen-source");
    auto frozen = WaveTrackFactory::Get(project()).Create(2, floatSample, 48000);
    auto& data = TrackFreeze::Get(*frozen);
    data.sourceXml = xml;
    for (const auto& block : *(*clip->Channels().begin())->GetSequenceBlockArray()) {
        data.blocks.push_back(block.sb);
    }
    ASSERT_FALSE(data.blocks.empty());
    const auto originalId = data.blocks.front()->GetBlockID();
    const auto factory = SampleBlockFactory::New(project());
    auto copy = WaveTrackUtilities::EmptyCopy(*frozen, WaveTrackUtilities::RealtimeEffectsCopy::Deep, factory);
    ASSERT_FALSE(TrackFreeze::Get(*copy).blocks.empty());
    EXPECT_NE(TrackFreeze::Get(*copy).blocks.front()->GetBlockID(), originalId);
    source.reset();
    clip.reset();
    frozen.reset();
    TrackList::Get(project()).Add(copy);
    trackedit::TrackFreezeModel model(m_context);
    model.init();
    model.setTrackId(copy->GetId());
    model.unfreeze();
    ASSERT_TRUE(model.error().isEmpty()) << model.error().toStdString();
    const auto* restored = *TrackList::Get(project()).Any<WaveTrack>().begin();
    ASSERT_EQ(restored->Intervals().size(), 1u);
    AudacityAudioSource audio(**restored->Intervals().begin());
    float sample = 0;
    ASSERT_TRUE(audio.read(0, &sample, 100, 1));
    EXPECT_FLOAT_EQ(sample, 0.375f);
}
TEST_F(PianoRollTests, InstrumentEditorRequestsAWindowAndKeepsLiveInputWhenTheWindowCloses)
{
    int requests = 0;
    QObject::connect(m_model.get(), &trackedit::PianoRollModel::instrumentEditorRequested, [&] { ++requests; });
    m_model->showInstrumentEditor();
    ASSERT_EQ(requests, 1);
    ASSERT_TRUE(m_inputs->editor);
    const auto editor = m_inputs->editor;
    m_model->showInstrumentEditor();
    EXPECT_EQ(requests, 2);
    EXPECT_EQ(m_inputs->editor, editor);
    const std::string state = "<PLUGIN type=\"4osc\" closedEditorTest=\"1\"/>";
    editor->savedState = state;
    m_model->hideInstrumentEditor();
    EXPECT_TRUE(editor->active());
    const auto* midi = *TrackList::Get(project()).Any<MidiTrack>().begin();
    EXPECT_EQ(midi->GetInstrumentState().utf8_string(), state);
    int closeRequests = 0;
    QObject::connect(m_model.get(), &trackedit::PianoRollModel::instrumentEditorCloseRequested, [&] { ++closeRequests; });
    m_history->undo();
    EXPECT_EQ(closeRequests, 1);
    EXPECT_FALSE(editor->active());
    EXPECT_TRUE((*TrackList::Get(project()).Any<MidiTrack>().begin())->GetInstrumentState().empty());
}

TEST_F(PianoRollTests, ContinuousInstrumentChangesCaptureOneStateAfterTheAdjustmentStops)
{
    ASSERT_TRUE(m_model->enableInput());
    const auto editor = m_inputs->editor;
    const auto initialIndex = m_history->currentStateIndex();
    const auto pump = [](int milliseconds) {
        QEventLoop events;
        QTimer::singleShot(milliseconds, &events, &QEventLoop::quit);
        events.exec();
    };
    for (int update = 0; update < 6; ++update) {
        editor->savedState = "<PLUGIN type=\"4osc\" adjustment=\"" + std::to_string(update) + "\"/>";
        editor->dirty = true;
        pump(60);
        EXPECT_EQ(editor->stateReads, 0);
        EXPECT_EQ(m_history->currentStateIndex(), initialIndex);
    }
    pump(350);
    EXPECT_EQ(editor->stateReads, 1);
    EXPECT_EQ(m_history->currentStateIndex(), initialIndex + 1);
    const auto finalState = editor->savedState;
    EXPECT_EQ((*TrackList::Get(project()).Any<MidiTrack>().begin())->GetInstrumentState().utf8_string(), finalState);
    pump(350);
    EXPECT_EQ(editor->stateReads, 1);
    EXPECT_EQ(m_history->currentStateIndex(), initialIndex + 1);
    m_history->undo();
    EXPECT_TRUE((*TrackList::Get(project()).Any<MidiTrack>().begin())->GetInstrumentState().empty());
    m_history->redo();
    EXPECT_EQ((*TrackList::Get(project()).Any<MidiTrack>().begin())->GetInstrumentState().utf8_string(), finalState);
}

TEST_F(PianoRollTests, ClosingInputCommitsPendingInstrumentChangesWithoutWaiting)
{
    ASSERT_TRUE(m_model->enableInput());
    const auto editor = m_inputs->editor;
    editor->savedState = "<PLUGIN type=\"4osc\" pendingClose=\"1\"/>";
    editor->dirty = true;
    QEventLoop events;
    QTimer::singleShot(40, &events, &QEventLoop::quit);
    events.exec();
    m_model->disableInput();
    EXPECT_EQ(editor->stateReads, 1);
    EXPECT_EQ((*TrackList::Get(project()).Any<MidiTrack>().begin())->GetInstrumentState().utf8_string(), editor->savedState);
    EXPECT_FALSE(editor->active());
}

TEST_F(PianoRollTests, UndoCommitsThePendingInstrumentAdjustmentBeforeRestoringThePreviousState)
{
    ASSERT_TRUE(m_model->enableInput());
    const auto editor = m_inputs->editor;
    const auto initialIndex = m_history->currentStateIndex();
    editor->savedState = "<PLUGIN type=\"4osc\" pendingUndo=\"1\"/>";
    editor->dirty = true;
    m_history->undo();
    EXPECT_EQ(editor->stateReads, 1);
    EXPECT_EQ(m_history->currentStateIndex(), initialIndex);
    EXPECT_TRUE((*TrackList::Get(project()).Any<MidiTrack>().begin())->GetInstrumentState().empty());
    m_history->redo();
    EXPECT_EQ((*TrackList::Get(project()).Any<MidiTrack>().begin())->GetInstrumentState().utf8_string(), editor->savedState);
}

TEST_F(PianoRollTests, SaveCommitsPendingInstrumentChangesAndKeepsTheEditorOpen)
{
    muse::async::Notification aboutSave;
    ON_CALL(*m_projectMock, aboutSaveBegin()).WillByDefault(testing::Return(aboutSave));
    ASSERT_TRUE(m_model->enableInput());
    const auto editor = m_inputs->editor;
    editor->savedState = "<PLUGIN type=\"4osc\" pendingSave=\"1\"/>";
    editor->dirty = true;
    aboutSave.notify();
    EXPECT_EQ(editor->stateReads, 1);
    EXPECT_TRUE(editor->active());
    EXPECT_EQ((*TrackList::Get(project()).Any<MidiTrack>().begin())->GetInstrumentState().utf8_string(), editor->savedState);
    ASSERT_TRUE(m_project->save(m_path));
    m_model->disableInput();
    m_project->clearSavedState();
    m_project->close();
    ASSERT_TRUE(m_project->load(m_path, false));
    EXPECT_EQ((*TrackList::Get(project()).Any<MidiTrack>().begin())->GetInstrumentState().utf8_string(), editor->savedState);
}

TEST_F(PianoRollTests, LiveInstrumentParametersDoNotStopTheAudioStream)
{
    ASSERT_TRUE(m_model->enableInput());
    audio::AudioStreamDescriptor stream;
    stream.ownerProject = &project();
    ON_CALL(*m_audio, currentStream()).WillByDefault(testing::Return(stream));
    ON_CALL(*m_audio, isBusy()).WillByDefault(testing::Return(true));
    ON_CALL(*m_audio, canStopAudioStream(testing::_)).WillByDefault(testing::Return(true));
    EXPECT_CALL(*m_audio, stopStream()).Times(0);
    m_model->setParameter("parameter", 0.5);
    EXPECT_TRUE(m_inputs->editor->active());
}
}
