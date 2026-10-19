/*
 * Audacity: A Digital Audio Editor
 */
#include <cfloat>

#include <gtest/gtest.h>
#include <gmock/gmock.h>

#include "../internal/au3/au3record.h"

#include "actions/tests/mocks/actionsdispatchermock.h"
#include "context/tests/mocks/globalcontextmock.h"
#include "context/tests/mocks/playbackstatemock.h"
#include "project/tests/mocks/audacityprojectmock.h"
#include "project/tests/testtools.h"
#include "trackedit/tests/mocks/selectioncontrollermock.h"
#include "trackedit/tests/mocks/trackeditprojectmock.h"

#include "au3audio/tests/mocks/audioenginemock.h"
#include "audio/tests/mocks/audiodrivercontrollermock.h"
#include "mocks/recordconfigurationmock.h"
#include "testing/testcontext.h"

#include "au3wrap/internal/au3project.h"
#include "au3wrap/au3types.h"

#include "au3-track/PendingTracks.h"
#include "au3-wave-track/WaveTrack.h"
#include "au3-wave-track/WaveClip.h"
#include "au3-wave-track-paint/waveform/WaveDataCache.h"

using ::testing::_;
using ::testing::DoAll;
using ::testing::InSequence;
using ::testing::Invoke;
using ::testing::NiceMock;
using ::testing::Return;
using ::testing::SaveArg;
using ::testing::Truly;

using namespace au::au3;

namespace au::record {
constexpr static double TEST_SAMPLE_RATE = 44100.0;

namespace {
class RecordingClipChanges : public WaveClipListener
{
public:
    void MarkChanged() noexcept override { ++changes; }
    void Invalidate() override {}
    std::unique_ptr<WaveClipListener> Clone() const override { return std::make_unique<RecordingClipChanges>(); }
    int changes = 0;
};

const WaveClip::Attachments::RegisteredFactory recordingChangesKey {
    [](WaveClip&) { return std::make_unique<RecordingClipChanges>(); }
};
}

class Au3RecordTests : public ::testing::Test
{
public:
    void SetUp() override
    {
        m_context = testutils::makeTestContext();
        m_record = std::make_shared<Au3Record>(m_context);

        m_globalContext = std::make_shared<NiceMock<context::GlobalContextMock> >();
        m_dispatcher = std::make_shared<NiceMock<muse::actions::ActionsDispatcherMock> >();
        m_selectionController = std::make_shared<NiceMock<trackedit::SelectionControllerMock> >();
        m_playbackState = std::make_shared<NiceMock<context::PlaybackStateMock> >();
        m_audioEngine = std::make_shared<NiceMock<audio::AudioEngineMock> >();
        m_recordConfiguration = std::make_shared<NiceMock<RecordConfigurationMock> >();
        m_audioDriverController = std::make_shared<NiceMock<audio::AudioDriverControllerMock> >();
        m_trackeditProject = std::make_shared<NiceMock<trackedit::TrackeditProjectMock> >();
        m_currentProject = std::make_shared<NiceMock<project::AudacityProjectMock> >();
        muse::modularity::ioc(m_context)->registerExport<context::IGlobalContext>("utests", m_globalContext);

        m_record->globalContext.set(m_globalContext);
        m_record->dispatcher.set(m_dispatcher);
        m_record->selectionController.set(m_selectionController);
        m_record->audioEngine.set(m_audioEngine);
        m_record->recordConfiguration.set(m_recordConfiguration);
        m_record->audioDriverController.set(m_audioDriverController);

        ON_CALL(*m_globalContext, currentProject())
        .WillByDefault(Return(m_currentProject));
        ON_CALL(*m_globalContext, playbackState())
        .WillByDefault(Return(m_playbackState));
        ON_CALL(*m_globalContext, currentTrackeditProject())
        .WillByDefault(Return(m_trackeditProject));

        ON_CALL(*m_audioEngine, isBusy())
        .WillByDefault(Invoke([this]() { return m_engineBusy; }));
        ON_CALL(*m_audioEngine, isCapturing())
        .WillByDefault(Return(false));
        ON_CALL(*m_audioEngine, stopStream())
        .WillByDefault(Invoke([this]() { m_engineBusy = false; }));
        ON_CALL(*m_audioEngine, recordingClipChanged())
        .WillByDefault(Return(m_recordingClipChanged));
        ON_CALL(*m_audioEngine, startStream(_, _, _, _, _, _))
        .WillByDefault(Return(1));

        ON_CALL(*m_audioDriverController, inputDevices())
        .WillByDefault(Return(std::vector<std::string> { "Test Input Device" }));
        ON_CALL(*m_audioDriverController, inputChannelsAvailable())
        .WillByDefault(Return(2));

        audio::AudioConfiguration audioConfiguration;
        audioConfiguration.inputChannels = 2;
        ON_CALL(*m_audioDriverController, configuration())
        .WillByDefault(Return(audioConfiguration));

        ON_CALL(*m_recordConfiguration, leadInTimeDuration())
        .WillByDefault(Return(2.0));
        ON_CALL(*m_recordConfiguration, crossfadeDuration())
        .WillByDefault(Return(0.0));

        initTestProject();
    }

    void TearDown() override
    {
        // Guard against a fatal failure during setup leaving the project
        // uninitialized; projectRef() would otherwise dereference a null pointer
        if (m_au3ProjectAccessor && m_au3ProjectAccessor->au3ProjectPtr()) {
            PendingTracks::Get(projectRef()).ClearPendingTracks();
            Au3TrackList::Get(projectRef()).Clear();

            m_au3ProjectAccessor->clearSavedState();
            m_au3ProjectAccessor->close();
        }

        testtools::removeProjectIfExists(m_workingProjectPath);
        m_record.reset();
        muse::modularity::ioc(m_context)->unregister<context::IGlobalContext>("utests");
    }

    void initTestProject()
    {
        m_au3ProjectAccessor = std::make_shared<Au3ProjectAccessor>(muse::modularity::globalCtx());

        // Load a working copy of the empty-project fixture shared with the trackedit tests
        const std::string source = std::string(record_tests_DATA_ROOT) + "/../../trackedit/tests/data/empty.aup4";
        m_workingProjectPath = std::string(record_tests_DATA_ROOT) + "/empty_working.aup4";
        testtools::removeProjectIfExists(m_workingProjectPath);
        ASSERT_TRUE(testtools::copyFile(source, m_workingProjectPath));

        constexpr auto discardAutosave = false;
        muse::Ret ret = m_au3ProjectAccessor->load(muse::io::path_t(m_workingProjectPath), discardAutosave);
        ASSERT_TRUE(ret);

        ON_CALL(*m_currentProject, au3ProjectPtr())
        .WillByDefault(Return(m_au3ProjectAccessor->au3ProjectPtr()));
    }

    Au3Project& projectRef() const
    {
        return *reinterpret_cast<Au3Project*>(m_au3ProjectAccessor->au3ProjectPtr());
    }

    Au3WaveTrack* addSelectedMonoTrack()
    {
        auto& trackFactory = Au3WaveTrackFactory::Get(projectRef());
        auto track = trackFactory.Create(sampleFormat::floatSample, TEST_SAMPLE_RATE);
        Au3TrackList::Get(projectRef()).Add(track, ::TrackList::DoAssignId::Yes,
                                            ::TrackList::EventPublicationSynchrony::Synchronous);
        track->SetSelected(true);
        return track.get();
    }

    std::shared_ptr<Au3Record> m_record;
    muse::modularity::ContextPtr m_context;

    std::shared_ptr<context::GlobalContextMock> m_globalContext;
    std::shared_ptr<muse::actions::ActionsDispatcherMock> m_dispatcher;
    std::shared_ptr<trackedit::SelectionControllerMock> m_selectionController;
    std::shared_ptr<context::PlaybackStateMock> m_playbackState;
    std::shared_ptr<audio::AudioEngineMock> m_audioEngine;
    std::shared_ptr<RecordConfigurationMock> m_recordConfiguration;
    std::shared_ptr<audio::AudioDriverControllerMock> m_audioDriverController;
    std::shared_ptr<trackedit::TrackeditProjectMock> m_trackeditProject;
    std::shared_ptr<project::AudacityProjectMock> m_currentProject;

    std::shared_ptr<Au3ProjectAccessor> m_au3ProjectAccessor;
    std::string m_workingProjectPath;

    muse::async::Channel<Au3TrackId, Au3ClipId> m_recordingClipChanged;
    bool m_engineBusy = false;
};

TEST_F(Au3RecordTests, LeadInRecordingStartsAtPlayheadPosition)
{
    //! [GIVEN] A selected track, the playhead at 8s and a stale selection start at 5s
    //! (e.g. left behind by a previous recording)
    addSelectedMonoTrack();
    ON_CALL(*m_playbackState, playbackPosition())
    .WillByDefault(Return(muse::secs_t(8.0)));
    ON_CALL(*m_selectionController, selectionStartTime())
    .WillByDefault(Return(trackedit::secs_t(5.0)));

    double startedAt = -1.0;
    audio::IAudioEngine::StartStreamOptions streamOptions;
    EXPECT_CALL(*m_audioEngine, startStream(_, _, _, _, _, _))
    .WillOnce(DoAll(SaveArg<1>(&startedAt), SaveArg<5>(&streamOptions), Return(1)));

    //! [THEN] The playhead is repositioned to the start of the lead-in (8s - 2s pre-roll)
    EXPECT_CALL(*m_dispatcher,
                dispatch(testing::Matcher<const muse::actions::ActionQuery&>(
                             Truly([](const muse::actions::ActionQuery& q) {
        return q.param("seekTime").toDouble() == 6.0;
    }))));

    //! [WHEN] Lead-in recording is initiated
    muse::Ret ret = m_record->leadInRecording();
    EXPECT_TRUE(ret);

    //! [THEN] The stream starts recording at the playhead position, not at the
    //! stale selection start
    EXPECT_DOUBLE_EQ(startedAt, 8.0);
    EXPECT_DOUBLE_EQ(streamOptions.leadInTime, 2.0);
}

TEST_F(Au3RecordTests, LeadInRecordingAtZeroPlayheadFails)
{
    //! [GIVEN] A selected track and the playhead at 0s — there is nothing to pre-roll
    addSelectedMonoTrack();
    ON_CALL(*m_playbackState, playbackPosition())
    .WillByDefault(Return(muse::secs_t(0.0)));

    EXPECT_CALL(*m_audioEngine, startStream(_, _, _, _, _, _))
    .Times(0);

    //! [WHEN] Lead-in recording is initiated
    muse::Ret ret = m_record->leadInRecording();

    //! [THEN] It fails and no stream is started
    EXPECT_FALSE(ret);
}

TEST_F(Au3RecordTests, RecordingToNewTrackUsesConfiguredTrackName)
{
    //! [GIVEN] No recordable track exists and a custom recording track name
    //! with the track number suffix is configured
    RecordingTrackNameOptions nameOptions;
    nameOptions.useCustomName = true;
    nameOptions.customName = "MyTake";
    nameOptions.addTrackNumber = true;
    ON_CALL(*m_recordConfiguration, recordingTrackNameOptions())
    .WillByDefault(Return(nameOptions));

    EXPECT_CALL(*m_audioEngine, startStream(_, _, _, _, _, _))
    .WillOnce(Return(1));

    //! [WHEN] Recording is initiated
    muse::Ret ret = m_record->start();
    EXPECT_TRUE(ret);

    //! [THEN] Recording goes to a new track named after the configured scheme
    const auto tracks = Au3TrackList::Get(projectRef()).Any<Au3WaveTrack>();
    ASSERT_EQ(tracks.size(), 1u);
    EXPECT_EQ((*tracks.begin())->NChannels(), 2u);
    EXPECT_EQ((*tracks.begin())->GetName(), wxString("MyTake_1"));
}

TEST_F(Au3RecordTests, RecordingWithBusyEngineStartsUnpausedAtPlayhead)
{
    //! [GIVEN] A selected track, the playhead at 8s and the engine still holding
    //! a (possibly paused) playback stream
    addSelectedMonoTrack();
    m_engineBusy = true;
    ON_CALL(*m_playbackState, playbackPosition())
    .WillByDefault(Return(muse::secs_t(8.0)));

    double startedAt = -1.0;

    //! [THEN] The old stream is stopped, the engine pause flag (which survives
    //! stream restarts) is cleared, and only then recording starts
    InSequence seq;
    EXPECT_CALL(*m_audioEngine, stopStream());
    EXPECT_CALL(*m_audioEngine, pauseStream(false));
    EXPECT_CALL(*m_audioEngine, startStream(_, _, _, _, _, _))
    .WillOnce(DoAll(SaveArg<1>(&startedAt), Return(1)));

    //! [WHEN] Recording is initiated
    muse::Ret ret = m_record->start();
    EXPECT_TRUE(ret);

    //! [THEN] Recording starts at the playhead position
    EXPECT_DOUBLE_EQ(startedAt, 8.0);
}

TEST_F(Au3RecordTests, FailedStartRemovesTheRecordingClipBeforeAnotherAttempt)
{
    auto* track = addSelectedMonoTrack();
    const std::vector<float> samples(44100, 0.25f);
    track->Append(0, reinterpret_cast<constSamplePtr>(samples.data()), floatSample, samples.size());
    track->Flush();
    const auto original = *track->Intervals().begin();
    ON_CALL(*m_playbackState, playbackPosition()).WillByDefault(Return(muse::secs_t(0.25)));
    EXPECT_CALL(*m_audioEngine, startStream(_, _, _, _, _, _))
    .WillOnce(Return(0))
    .WillOnce(Return(1));

    EXPECT_FALSE(m_record->start());
    EXPECT_TRUE(m_record->recordingClipKeys().empty());
    EXPECT_EQ(track->NIntervals(), 1u);
    EXPECT_TRUE(track->NoPlayRegionsOverlap());
    EXPECT_DOUBLE_EQ(original->GetPlayEndTime(), 1.0);

    EXPECT_TRUE(m_record->start());
    EXPECT_EQ(m_record->recordingClipKeys().size(), 1u);
}

TEST_F(Au3RecordTests, FailedStartRemovesANewRecordingTrack)
{
    EXPECT_CALL(*m_audioEngine, startStream(_, _, _, _, _, _)).WillOnce(Return(0));

    EXPECT_FALSE(m_record->start());
    EXPECT_TRUE(m_record->recordingClipKeys().empty());
    EXPECT_TRUE(Au3TrackList::Get(projectRef()).empty());
}

TEST_F(Au3RecordTests, FailedLeadInStartKeepsTheOriginalTrack)
{
    auto* track = addSelectedMonoTrack();
    ON_CALL(*m_playbackState, playbackPosition()).WillByDefault(Return(muse::secs_t(8.0)));
    EXPECT_CALL(*m_audioEngine, startStream(_, _, _, _, _, _)).WillOnce(Return(0));

    EXPECT_FALSE(m_record->leadInRecording());
    EXPECT_TRUE(m_record->recordingClipKeys().empty());
    EXPECT_EQ(Au3TrackList::Get(projectRef()).Any<Au3WaveTrack>().size(), 1u);
    EXPECT_EQ(track->NIntervals(), 0u);
}

TEST_F(Au3RecordTests, RecordingAppendsKeepCompletedWaveformCacheEntries)
{
    m_record->init();
    TransportSequences sequences;
    EXPECT_CALL(*m_audioEngine, startStream(_, _, _, _, _, _))
    .WillOnce(DoAll(SaveArg<0>(&sequences), Return(1)));
    ASSERT_TRUE(m_record->start());
    ASSERT_EQ(sequences.captureSequences.size(), 1u);
    const auto tracks = Au3TrackList::Get(projectRef()).Any<Au3WaveTrack>();
    ASSERT_EQ(tracks.size(), 1u);
    const auto track = *tracks.begin();
    ASSERT_EQ(track->NIntervals(), 1u);
    const auto visible = *track->Intervals().begin();
    auto& changes = visible->Attachments::Get<RecordingClipChanges>(recordingChangesKey);
    changes.changes = 0;
    const auto pending = sequences.captureSequences.front();
    const auto append = [&](float value, size_t count) {
        const std::vector<float> samples(count, value);
        for (size_t channel = 0; channel < pending->NChannels(); ++channel) {
            pending->Append(channel, reinterpret_cast<constSamplePtr>(samples.data()), floatSample, count, 1, floatSample);
        }
        PendingTracks::Get(projectRef()).UpdatePendingTracks();
    };
    append(0.25f, 300);
    const int initialChanges = changes.changes;
    WaveDataCache cache(*visible, 0);
    const auto first = cache.PerformLookup({ TEST_SAMPLE_RATE, 0 });
    ASSERT_NE(first, nullptr);
    ASSERT_TRUE(first->IsComplete);
    const auto firstUpdate = first->LastUpdate;
    const auto tail = cache.PerformLookup({ TEST_SAMPLE_RATE, 256 });
    ASSERT_NE(tail, nullptr);
    ASSERT_FALSE(tail->IsComplete);
    append(0.75f, 300);
    EXPECT_EQ(changes.changes, initialChanges);
    EXPECT_EQ(cache.PerformLookup({ TEST_SAMPLE_RATE, 0 }), first);
    EXPECT_EQ(first->LastUpdate, firstUpdate);
    const auto updatedTail = cache.PerformLookup({ TEST_SAMPLE_RATE, 256 });
    ASSERT_NE(updatedTail, nullptr);
    ASSERT_TRUE(updatedTail->IsComplete);
    EXPECT_FLOAT_EQ(updatedTail->Data[80].min, 0.75f);
    EXPECT_FLOAT_EQ(updatedTail->Data[80].max, 0.75f);
    EXPECT_DOUBLE_EQ(visible->GetPlayEndTime(), 600.0 / TEST_SAMPLE_RATE);
    pending->Flush();
    append(0.5f, 300);
    const auto nextTail = cache.PerformLookup({ TEST_SAMPLE_RATE, 512 });
    ASSERT_NE(nextTail, nullptr);
    ASSERT_TRUE(nextTail->IsComplete);
    EXPECT_FLOAT_EQ(nextTail->Data[100].min, 0.5f);
    EXPECT_FLOAT_EQ(nextTail->Data[100].max, 0.5f);
    EXPECT_EQ(changes.changes, initialChanges);
    EXPECT_EQ(cache.PerformLookup({ TEST_SAMPLE_RATE, 0 }), first);
    EXPECT_EQ(first->LastUpdate, firstUpdate);
}
}
