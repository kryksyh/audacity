/*
* Audacity: A Digital Audio Editor
*/

// Regression test for: master-track realtime effects lost on save/reload.
//
// The master (project-wide) realtime-effect list is serialized in
// ProjectFileIO::WriteXML from a snapshot (SavedMasterEffectList), not the live
// list. Au3ProjectAccessor::load() takes that snapshot (empty) right after load;
// if the snapshot is not refreshed before SaveProject, master effects added since
// load are never written, and reopening loses them. This test reproduces the
// load -> add -> save -> reload round trip and asserts the effect survives.

#include <gtest/gtest.h>
#include <gmock/gmock.h>
#include <QTemporaryDir>

#include "global/io/fileinfo.h"
#include "global/types/ret.h"

#include "project/internal/audacityproject.h"

#include "project/tests/mocks/trackeditprojectcreatormock.h"
#include "project/tests/mocks/projectviewstatecreatormock.h"
#include "project/tests/mocks/dummyeffectinstancefactory.h"
#include "project/tests/mocks/cloudprojectsprovidermock.h"
#include "trackedit/tests/mocks/clipboardmock.h"
#include "trackedit/internal/tracksviewrequestsservice.h"
#include "context/tests/mocks/globalcontextmock.h"
#include "au3-midi-track/MidiTrack.h"
#include "au3-track/Track.h"
#include "au3-project-history/UndoManager.h"
#include "au3-utility/MemoryX.h"

#include "testtools.h"
#include "testing/testcontext.h"

// au3 realtime-effects (symbols come transitively from au3wrap's public link of
// au3-realtime-effects / au3-components).
#include "au3-project/Project.h"
#include "au3-realtime-effects/RealtimeEffectList.h"
#include "au3-realtime-effects/RealtimeEffectState.h"

namespace au::project {
class Project_RealtimeEffectsPersistenceTests : public ::testing::Test
{
protected:
    muse::modularity::ContextPtr m_testCtx;
    std::unique_ptr<Audacity4Project> m_currentProject;
    std::shared_ptr<au::project::TrackeditProjectCreatorMock> m_trackeditProjectCreator;
    std::shared_ptr<au::projectscene::ProjectViewStateCreatorMock> m_projectViewStateCreator;
    std::shared_ptr<au::trackedit::ClipboardMock> m_clipboard;
    std::shared_ptr<au::au3cloud::CloudProjectsProviderMock> m_cloudProjectsProvider;
    std::shared_ptr<au::context::GlobalContextMock> m_liveContext;
    std::shared_ptr<au::trackedit::TracksViewRequestsService> m_viewRequests;

    void bindLiveInstrumentRequests()
    {
        m_liveContext = std::make_shared<::testing::NiceMock<au::context::GlobalContextMock> >();
        m_viewRequests = std::make_shared<au::trackedit::TracksViewRequestsService>(m_testCtx);
        ON_CALL(*m_liveContext, currentProject()).WillByDefault([this] {
            return IAudacityProjectPtr(m_currentProject.get(), [](IAudacityProject*) {});
        });
        m_currentProject->globalContext.set(m_liveContext);
        m_currentProject->viewRequests.set(m_viewRequests);
    }

    static muse::Ret saveWithoutThumbnail(const std::unique_ptr<Audacity4Project>& project, const muse::io::path_t& path)
    {
        return project->doSave(path, false, false);
    }

    void SetUp() override
    {
        m_testCtx = au::testutils::makeTestContext();
        m_clipboard = std::make_shared<::testing::NiceMock<au::trackedit::ClipboardMock> >();
        m_cloudProjectsProvider = std::make_shared<::testing::NiceMock<au::au3cloud::CloudProjectsProviderMock> >();
        m_currentProject = makeProject();
    }

    void TearDown() override
    {
        if (m_currentProject) {
            m_currentProject.reset();
        }
    }

    std::unique_ptr<Audacity4Project> makeProject()
    {
        auto project = std::make_unique<Audacity4Project>(m_testCtx);
        project->trackeditProjectCreator.set(m_trackeditProjectCreator);
        project->viewStateCreator.set(m_projectViewStateCreator);
        project->clipboard.set(m_clipboard);
        project->cloudProjectsProvider.set(m_cloudProjectsProvider);
        return project;
    }

    static ::AudacityProject& au3Project(const std::unique_ptr<Audacity4Project>& project)
    {
        return *reinterpret_cast<::AudacityProject*>(project->m_au3Project->au3ProjectPtr());
    }

    // Save through Au3ProjectAccessor::save() directly (the method under test),
    // bypassing the facade's thumbnail creation. Friendship is granted to this
    // fixture, not to the TEST_F-generated subclass, so the access must live here.
    static bool saveDirect(const std::unique_ptr<Audacity4Project>& project, const muse::io::path_t& path)
    {
        return project->m_au3Project->save(path);
    }
};

TEST_F(Project_RealtimeEffectsPersistenceTests, SaveFlushesPendingInstrumentStateBeforeSerializingTheProject)
{
    QTemporaryDir temporary;
    const auto source = std::string(au_project_tests_DATA_ROOT) + "/data/empty.aup4";
    const auto path = temporary.filePath("instrument.aup4").toStdString();
    ASSERT_TRUE(testtools::copyFile(source, path));
    ASSERT_TRUE(m_currentProject->load(muse::io::path_t(path), false, "").success());
    const auto closeProject = finally([&] { m_currentProject->close(); });
    bindLiveInstrumentRequests();
    auto track = MidiTrack::Create();
    TrackList::Get(au3Project(m_currentProject)).Add(track);
    muse::async::Asyncable receiver;
    int flushes = 0;
    m_viewRequests->flushInstrumentChangesRequested().onNotify(&receiver, [&] {
        ++flushes;
        track->SetInstrument("builtin:sine", "Latest controller preset");
    });
    ASSERT_TRUE(saveWithoutThumbnail(m_currentProject, muse::io::path_t(path)));
    EXPECT_EQ(flushes, 1);
    m_currentProject->close();
    ASSERT_TRUE(m_currentProject->load(muse::io::path_t(path), false, "").success());
    const auto midi = TrackList::Get(au3Project(m_currentProject)).Any<MidiTrack>();
    ASSERT_FALSE(midi.empty());
    EXPECT_EQ((*midi.begin())->GetInstrumentState().utf8_string(), "Latest controller preset");
}

TEST_F(Project_RealtimeEffectsPersistenceTests, ClosePromptChecksPendingInstrumentChangesBeforeReportingTheProjectClean)
{
    QTemporaryDir temporary;
    const auto source = std::string(au_project_tests_DATA_ROOT) + "/data/empty.aup4";
    const auto path = temporary.filePath("instrument.aup4").toStdString();
    ASSERT_TRUE(testtools::copyFile(source, path));
    ASSERT_TRUE(m_currentProject->load(muse::io::path_t(path), false, "").success());
    const auto closeProject = finally([&] { m_currentProject->close(); });
    auto& undo = UndoManager::Get(au3Project(m_currentProject));
    undo.PushState(::TranslatableString::untranslatable(QStringLiteral("Initial")), {});
    undo.StateSaved();
    EXPECT_FALSE(m_currentProject->hasUnsavedChanges());
    bindLiveInstrumentRequests();
    muse::async::Asyncable receiver;
    m_viewRequests->flushInstrumentChangesRequested().onNotify(&receiver, [&] {
        UndoManager::Get(au3Project(m_currentProject)).MarkUnsaved();
    });
    EXPECT_TRUE(m_currentProject->hasUnsavedChanges());
}

TEST_F(Project_RealtimeEffectsPersistenceTests, MasterEffectList_SurvivesSaveReload)
{
    const std::string srcPath
        = (muse::String::fromUtf8(au_project_tests_DATA_ROOT) + "/data/empty.aup4").toStdString();
    const std::string dstPath
        = (muse::String::fromUtf8(au_project_tests_DATA_ROOT) + "/data/master_effect_roundtrip.aup4").toStdString();

    // Writable working copy of the (empty) project.
    testtools::removeProjectIfExists(dstPath);
    ASSERT_TRUE(testtools::copyFile(srcPath, dstPath));

    const muse::io::path_t projectPath(dstPath);
    const PluginID kFakeId = wxString::FromUTF8("au-test:fake-master-effect");

    // Resolve the fake plugin id for the whole test, so AddState succeeds and the
    // state survives reload's SetID() -> GetEffect() re-resolution.
    RealtimeEffectState::EffectFactory::Scope effectFactoryScope {
        [](const PluginID&) -> const EffectInstanceFactory* { return &dummyFactory(); }
    };

    // 1) Load the project. Au3ProjectAccessor::load() takes the (empty) master snapshot.
    ASSERT_TRUE(m_currentProject->load(projectPath, false, "").success());

    // 2) Add a master realtime effect to the LIVE list (the snapshot is now stale).
    {
        auto& live = RealtimeEffectList::Get(au3Project(m_currentProject));
        ASSERT_EQ(live.GetStatesCount(), 0u);

        auto state = RealtimeEffectState::make_shared(kFakeId);
        ASSERT_TRUE(live.AddState(state));
        ASSERT_EQ(live.GetStatesCount(), 1u);
    }

    // 3) Save through the path that had the #9790 bug (Au3ProjectAccessor::save ->
    //    SaveProject -> WriteXML serializing a stale master-effect snapshot),
    //    bypassing the facade's thumbnail creation.
    ASSERT_TRUE(saveDirect(m_currentProject, projectPath));

    // 4) Close and reload into a fresh project instance.
    m_currentProject->close();
    m_currentProject = makeProject();
    ASSERT_TRUE(m_currentProject->load(projectPath, false, "").success());

    // 5) The master effect must have round-tripped.
    auto& reloaded = RealtimeEffectList::Get(au3Project(m_currentProject));
    EXPECT_EQ(reloaded.GetStatesCount(), 1u);
    if (reloaded.GetStatesCount() == 1u) {
        EXPECT_EQ(reloaded.GetStateAt(0)->GetID().ToStdString(), kFakeId.ToStdString());
    }

    m_currentProject->close();

    testtools::removeProjectIfExists(dstPath);
}

TEST_F(Project_RealtimeEffectsPersistenceTests, RealtimeEffectState_LoadNormalizesWindowsEffectIdPathSeparators)
{
    const std::string aup3Id
        = "Effect_VST3_Soap Audio_Soap Voice Cleaner_C:\\Program Files\\Common Files\\VST3\\Soap Voice Cleaner.vst3;"
          "ABCDEF019182FAEB536F6170536F6170";
    const std::string aup4Id
        = "Effect_VST3_Soap Audio_Soap Voice Cleaner_C:/Program Files/Common Files/VST3/Soap Voice Cleaner.vst3;"
          "ABCDEF019182FAEB536F6170536F6170";

    RealtimeEffectState::EffectFactory::Scope effectFactoryScope {
        [](const PluginID&) -> const EffectInstanceFactory* { return &dummyFactory(); }
    };

    RealtimeEffectState state { {} };
    const AttributesList attrs {
        { "id", XMLAttributeValueView { std::string_view { aup3Id } } }
    };

    ASSERT_TRUE(state.HandleXMLTag(RealtimeEffectState::XMLTag(), attrs));
    EXPECT_EQ(state.GetID().ToStdString(), aup4Id);
}

TEST_F(Project_RealtimeEffectsPersistenceTests, RealtimeEffectState_LoadStoresResolvedEffectId)
{
    const PluginID loadedId = wxString::FromUTF8(
        "Effect_VST3_Soap Audio_Soap Voice Cleaner_C:\\Program Files\\Common Files\\VST3\\Soap Voice Cleaner.vst3;"
        "ABCDEF019182FAEB536F6170536F6170");
    const PluginID normalizedId = wxString::FromUTF8(
        "Effect_VST3_Soap Audio_Soap Voice Cleaner_C:/Program Files/Common Files/VST3/Soap Voice Cleaner.vst3;"
        "ABCDEF019182FAEB536F6170536F6170");
    const PluginID resolvedId = wxString::FromUTF8(
        "Effect_VST3_Soap Audio_Soap Voice Cleaner_D:/Audio/VST3/Soap Voice Cleaner.vst3;"
        "ABCDEF019182FAEB536F6170536F6170");

    RealtimeEffectState::EffectIdResolver::Scope resolverScope {
        [&](const PluginID& id) -> PluginID { return id == normalizedId ? resolvedId : PluginID {}; }
    };
    RealtimeEffectState::EffectFactory::Scope effectFactoryScope {
        [](const PluginID&) -> const EffectInstanceFactory* { return &dummyFactory(); }
    };

    RealtimeEffectState state { {} };
    const std::string loadedIdString = loadedId.ToStdString();
    const AttributesList attrs {
        { "id", XMLAttributeValueView { std::string_view { loadedIdString } } }
    };

    ASSERT_TRUE(state.HandleXMLTag(RealtimeEffectState::XMLTag(), attrs));
    EXPECT_EQ(state.GetID(), resolvedId);
}
}
