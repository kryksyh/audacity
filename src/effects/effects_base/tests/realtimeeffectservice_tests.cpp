/*
* Audacity: A Digital Audio Editor
*/

#include <gtest/gtest.h>
#include <gmock/gmock.h>

#include "effects/effects_base/internal/realtimeeffectservice.h"
#include "effects/effects_base/internal/realtimeeffectserviceutils.h"
#include "effects/effects_base/internal/effectsprovider.h"

#include "context/tests/mocks/globalcontextmock.h"
#include "project/tests/mocks/audacityprojectmock.h"
#include "project/tests/mocks/dummyeffectinstancefactory.h"
#include "trackedit/tests/mocks/projecthistorymock.h"

#include "testing/testcontext.h"

#include "au3-project/Project.h"
#include "au3-track/Track.h"
#include "au3-wave-track/WaveTrack.h"
#include "au3-midi-track/MidiTrack.h"
#include "au3-project-history/ProjectHistory.h"
#include "au3-project-history/UndoManager.h"
#include "au3-realtime-effects/RealtimeEffectList.h"
#include "au3-realtime-effects/RealtimeEffectState.h"
#include "au3-xml/XMLFileReader.h"

using namespace ::testing;

namespace au::effects {
namespace {
class ValueEffect final : public project::DummyEffectInstanceFactory
{
public:
    EffectSettings MakeSettings() const override { return EffectSettings::Make<double>(); }
    bool SaveSettings(const EffectSettings& settings, CommandParameters& parameters) const override
    {
        return parameters.Write("Value", *settings.cast<double>());
    }

    bool LoadSettings(const CommandParameters& parameters, EffectSettings& settings) const override
    {
        return parameters.Read("Value", settings.cast<double>());
    }
};

class CachedMetadataProvider final : public EffectsProvider
{
public:
    EffectMeta meta(const EffectId& id) const override
    {
        EffectMeta result;
        result.id = id;
        result.state = state;
        return result;
    }

    muse::audioplugins::AudioPluginState state = muse::audioplugins::AudioPluginState::Validated;
};
}
class Effects_RealtimeEffectServiceTests : public ::testing::Test, public muse::async::Asyncable
{
protected:
    void SetUp() override
    {
        m_ctx = testutils::makeTestContext();

        m_globalContext = std::make_shared<NiceMock<context::GlobalContextMock> >();
        muse::modularity::ioc(m_ctx)->registerExport<context::IGlobalContext>("utests", m_globalContext);

        m_au3Project = ::AudacityProject::Create();
        m_project = std::make_shared<NiceMock<project::AudacityProjectMock> >();

        ON_CALL(*m_project, au3ProjectPtr())
        .WillByDefault(Return(reinterpret_cast<uintptr_t>(m_au3Project.get())));
        ON_CALL(*m_globalContext, currentProject())
        .WillByDefault(Return(m_project));
        ON_CALL(*m_globalContext, currentProjectChanged())
        .WillByDefault(Return(m_currentProjectChanged));

        m_service = std::make_shared<RealtimeEffectService>(m_ctx);
    }

    void TearDown() override
    {
        m_service.reset();
        muse::modularity::ioc(m_ctx)->unregister<context::IGlobalContext>("utests");
    }

    muse::modularity::ContextPtr m_ctx;
    std::shared_ptr<NiceMock<context::GlobalContextMock> > m_globalContext;
    std::shared_ptr<NiceMock<project::AudacityProjectMock> > m_project;
    std::shared_ptr<::AudacityProject> m_au3Project;
    muse::async::Notification m_currentProjectChanged;
    std::shared_ptr<RealtimeEffectService> m_service;
};

TEST_F(Effects_RealtimeEffectServiceTests, EffectStackGoneOnTrackDeletionWhileStatesStillAlive)
{
    RealtimeEffectState::EffectFactory::Scope factoryScope {
        [](const PluginID&) -> const EffectInstanceFactory* { return &project::dummyFactory(); }
    };

    // A track with one realtime effect, as after loading a project.
    auto& trackList = ::TrackList::Get(*m_au3Project);
    auto waveTrack = ::WaveTrackFactory::Get(*m_au3Project).Create();
    trackList.Add(waveTrack);
    const TrackId trackId = waveTrack->GetId();

    auto state = RealtimeEffectState::make_shared(wxString::FromUTF8("au-test:fake-effect"));
    const std::weak_ptr<RealtimeEffectState> weakState = state;
    ASSERT_TRUE(RealtimeEffectList::Get(*waveTrack).AddState(state));

    // The track (via its effect list) must be the sole owner of the state.
    state.reset();
    waveTrack.reset();

    m_service->init();

    ASSERT_TRUE(m_service->effectStack(trackId).has_value());
    ASSERT_EQ(m_service->effectStack(trackId)->size(), 1u);
    ASSERT_TRUE(m_service->effectStack(IRealtimeEffectService::masterTrackId).has_value());

    // UI models re-read the stack from within the stack-changed notification.
    bool notified = false;
    bool stackGoneDuringNotification = false;
    bool stateAliveDuringNotification = false;
    m_service->realtimeEffectStackChanged().onReceive(this, [&](TrackId id) {
        if (id != trackId) {
            return;
        }
        notified = true;
        stackGoneDuringNotification = !m_service->effectStack(id).has_value();
        stateAliveDuringNotification = !weakState.expired();
    });

    trackList.Clear();

    EXPECT_TRUE(notified);
    EXPECT_TRUE(stackGoneDuringNotification);
    EXPECT_TRUE(stateAliveDuringNotification);

    // After the clear the states are destroyed, and the track's stack stays gone.
    EXPECT_TRUE(weakState.expired());
    EXPECT_FALSE(m_service->effectStack(trackId).has_value());

    // The master list belongs to the project and remains registered.
    EXPECT_TRUE(m_service->effectStack(IRealtimeEffectService::masterTrackId).has_value());
}

TEST_F(Effects_RealtimeEffectServiceTests, MidiEffectStacksNotifyForLoadedAndNewTracksAndResolveTheirOwners)
{
    RealtimeEffectState::EffectFactory::Scope factoryScope {
        [](const PluginID&) -> const EffectInstanceFactory* { return &project::dummyFactory(); }
    };
    auto& tracks = ::TrackList::Get(*m_au3Project);
    const auto loaded = MidiTrack::Create();
    tracks.Add(loaded);
    auto first = RealtimeEffectState::make_shared("test:midi-first");
    ASSERT_TRUE(RealtimeEffectList::Get(*loaded).AddState(first));
    m_service->init();
    std::vector<std::pair<TrackId, RealtimeEffectStatePtr> > additions;
    m_service->realtimeEffectAdded().onReceive(this, [&](TrackId id, RealtimeEffectStatePtr state) {
        additions.emplace_back(id, std::move(state));
    });
    const auto second = RealtimeEffectState::make_shared("test:midi-second");
    ASSERT_TRUE(RealtimeEffectList::Get(*loaded).AddState(second));
    ASSERT_EQ(additions.size(), 1u);
    EXPECT_EQ(additions.front().first, loaded->GetId());
    EXPECT_EQ(additions.front().second, second);
    EXPECT_EQ(utils::trackId(m_project, second), loaded->GetId());
    EXPECT_EQ(utils::effectIndex(m_project, second), 1);

    const auto created = MidiTrack::Create();
    tracks.Add(created);
    const auto third = RealtimeEffectState::make_shared("test:midi-third");
    ASSERT_TRUE(RealtimeEffectList::Get(*created).AddState(third));
    ASSERT_EQ(additions.size(), 2u);
    EXPECT_EQ(additions.back().first, created->GetId());
    EXPECT_EQ(utils::trackId(m_project, third), created->GetId());
    EXPECT_EQ(utils::effectIndex(m_project, third), 0);

    m_currentProjectChanged.notify();
    const auto fourth = RealtimeEffectState::make_shared("test:midi-fourth");
    ASSERT_TRUE(RealtimeEffectList::Get(*created).AddState(fourth));
    ASSERT_EQ(additions.size(), 3u) << "Rebinding must neither miss nor duplicate list subscriptions";
    EXPECT_EQ(additions.back().second, fourth);
    bool removed = false;
    m_service->realtimeEffectRemoved().onReceive(this, [&](TrackId id, RealtimeEffectStatePtr state) {
        removed = id == created->GetId() && state == third;
    });
    RealtimeEffectList::Get(*created).RemoveState(third);
    EXPECT_TRUE(removed);
    EXPECT_FALSE(utils::trackId(m_project, third).has_value());
}

TEST_F(Effects_RealtimeEffectServiceTests, MidiEffectUndoRedoRestoresPendingManualSettingsAndActivation)
{
    ValueEffect effect;
    RealtimeEffectState::EffectFactory::Scope factoryScope([&](const PluginID&) { return &effect; });
    const auto track = MidiTrack::Create();
    TrackList::Get(*m_au3Project).Add(track);
    const auto state = RealtimeEffectState::make_shared("test:midi-value");
    ASSERT_TRUE(RealtimeEffectList::Get(*track).AddState(state));
    m_service->init();
    auto& undo = UndoManager::Get(*m_au3Project);
    const auto clear = finally([&] { undo.ClearStates(); });
    auto access = state->GetAccess();
    auto change = [&](double value, bool active) {
        access->ModifySettings([&](EffectSettings& settings) {
            *settings.cast<double>() = value;
            settings.extra.SetActive(active);
            return nullptr;
        });
    };
    change(0.25, true);
    EXPECT_DOUBLE_EQ(*state->GetSettings().cast<double>(), 0.0);
    undo.PushState(TranslatableString::untranslatable("First manual value"), {});
    change(0.75, false);
    undo.PushState(TranslatableString::untranslatable("Second manual value"), {});
    change(0.5, true);
    auto restore = [&](const UndoStackElem& element) { ProjectHistory::Get(*m_au3Project).PopState(element.state, false); };
    undo.Undo(restore);
    EXPECT_DOUBLE_EQ(*access->Get().cast<double>(), 0.25);
    EXPECT_TRUE(state->IsEnabled());
    undo.Redo(restore);
    EXPECT_DOUBLE_EQ(*access->Get().cast<double>(), 0.75);
    EXPECT_FALSE(state->IsEnabled());
}

TEST_F(Effects_RealtimeEffectServiceTests, EffectAutomationUndoRedoRestoresMidiWaveAndMasterLanesWithoutReplacingTheirStates)
{
    RealtimeEffectState::EffectFactory::Scope factoryScope {
        [](const PluginID&) -> const EffectInstanceFactory* { return &project::dummyFactory(); }
    };
    auto& tracks = TrackList::Get(*m_au3Project);
    const auto midi = MidiTrack::Create();
    tracks.Add(midi);
    const auto wave = WaveTrackFactory::Get(*m_au3Project).Create();
    tracks.Add(wave);
    std::vector<RealtimeEffectStatePtr> effects;
    for (auto* list : { &RealtimeEffectList::Get(*midi), &RealtimeEffectList::Get(*wave), &RealtimeEffectList::Get(*m_au3Project) }) {
        const auto state = RealtimeEffectState::make_shared("test:automated-insert");
        ASSERT_TRUE(list->AddState(state));
        effects.push_back(state);
    }
    m_service->init();
    auto& undo = UndoManager::Get(*m_au3Project);
    const auto clear = finally([&] { undo.ClearStates(); });
    undo.PushState(TranslatableString::untranslatable("No insert lanes"), {});
    for (const auto& state : effects) {
        ASSERT_TRUE(state->Automation().SetLane({ "Gain", true, { { 0.0, 0.25, 0.0 } } }));
    }
    undo.PushState(TranslatableString::untranslatable("First insert lanes"), {});
    for (const auto& state : effects) {
        ASSERT_TRUE(state->Automation().SetLane({ "Gain", false, { { 0.0, 0.75, 0.0 } } }));
    }
    undo.PushState(TranslatableString::untranslatable("Changed insert lanes"), {});
    auto restore = [&](const UndoStackElem& element) { ProjectHistory::Get(*m_au3Project).PopState(element.state, false); };
    auto expect = [&](double value, bool enabled) {
        for (const auto& state : effects) {
            const auto* lane = state->Automation().Find("Gain");
            ASSERT_NE(lane, nullptr);
            ASSERT_EQ(lane->points.size(), 1u);
            EXPECT_DOUBLE_EQ(lane->points.front().value, value);
            EXPECT_EQ(lane->enabled, enabled);
        }
    };
    undo.Undo(restore);
    expect(0.25, true);
    undo.Redo(restore);
    expect(0.75, false);
    undo.Undo(restore);
    undo.Undo(restore);
    for (const auto& state : effects) {
        EXPECT_TRUE(state->Automation().Lanes().empty());
    }
    undo.Redo(restore);
    expect(0.25, true);
    const auto restoredMidi = *TrackList::Get(*m_au3Project).Any<MidiTrack>().begin();
    const auto restoredWave = *TrackList::Get(*m_au3Project).Any<WaveTrack>().begin();
    EXPECT_EQ(RealtimeEffectList::Get(*restoredMidi).GetStateAt(0), effects[0]);
    EXPECT_EQ(RealtimeEffectList::Get(*restoredWave).GetStateAt(0), effects[1]);
    EXPECT_EQ(RealtimeEffectList::Get(*m_au3Project).GetStateAt(0), effects[2]);
}

TEST_F(Effects_RealtimeEffectServiceTests, UnavailableEffectBypassUsesDocumentStateAndRecordsOnlyActualChanges)
{
    auto provider = std::make_shared<EffectsProvider>();
    muse::modularity::globalIoc()->registerExport<IEffectsProvider>("missing-effect-test", provider);
    const auto history = std::make_shared<NiceMock<trackedit::ProjectHistoryMock> >();
    muse::modularity::ioc(m_ctx)->registerExport<trackedit::IProjectHistory>("missing-effect-test", history);
    const auto unregister = finally([&] {
        muse::modularity::ioc(m_ctx)->unregister<trackedit::IProjectHistory>("missing-effect-test");
        muse::modularity::globalIoc()->unregister<IEffectsProvider>("missing-effect-test");
    });
    RealtimeEffectState::EffectFactory::Scope factory([](const PluginID&) -> const EffectInstanceFactory* { return nullptr; });
    const auto state = RealtimeEffectState::make_shared("test:unavailable-effect");
    ASSERT_EQ(state->GetEffect(), nullptr);
    ASSERT_FALSE(m_service->isAvailable(state));
    EXPECT_TRUE(m_service->isActive(state));
    EXPECT_CALL(*history, modifyState(false)).Times(2);
    EXPECT_CALL(*history, markUnsaved()).Times(2);
    int notifications = 0;
    m_service->isActiveChanged().onReceive(this, [&](const RealtimeEffectStatePtr& changed) {
        EXPECT_EQ(changed, state);
        ++notifications;
    });
    m_service->setIsActive(state, false);
    EXPECT_FALSE(state->IsEnabled());
    EXPECT_FALSE(m_service->isActive(state));
    m_service->setIsActive(state, false);
    m_service->setIsActive(state, true);
    EXPECT_TRUE(state->IsEnabled());
    EXPECT_TRUE(m_service->isActive(state));
    m_service->setIsActive(state, true);
    EXPECT_EQ(notifications, 2);
    EXPECT_FALSE(m_service->isActive(nullptr));
    m_service->setIsActive(nullptr, false);
}

TEST_F(Effects_RealtimeEffectServiceTests, UndoRestoresOpaqueSettingsCapturedBeforeThePluginBecameAvailable)
{
    ValueEffect effect;
    bool available = false;
    RealtimeEffectState::EffectFactory::Scope factory([&](const PluginID&) -> const EffectInstanceFactory* {
        return available ? &effect : nullptr;
    });
    const auto track = MidiTrack::Create();
    TrackList::Get(*m_au3Project).Add(track);
    auto& effects = RealtimeEffectList::Get(*track);
    XMLFileReader reader;
    ASSERT_TRUE(reader.ParseString(&effects,
                                   "<effects active=\"1\"><effect active=\"1\" id=\"test:unavailable-value\"><parameters>"
                                   "<parameter name=\"Value\" value=\"0.75\"/></parameters><automation>"
                                   "<lane parameter=\"Value\" enabled=\"1\"><point time=\"0\" value=\"0.4\" curve=\"0\"/></lane>"
                                   "</automation></effect></effects>"));
    const auto state = effects.GetStateAt(0);
    ASSERT_EQ(state->GetEffect(), nullptr);
    m_service->init();
    auto& undo = UndoManager::Get(*m_au3Project);
    const auto clear = finally([&] { undo.ClearStates(); });
    undo.PushState(TranslatableString::untranslatable("Unavailable effect"), {});
    available = true;
    ASSERT_NE(state->GetEffect(), nullptr);
    const auto access = state->GetAccess();
    ASSERT_DOUBLE_EQ(*access->Get().cast<double>(), 0.75);
    access->ModifySettings([](EffectSettings& settings) { *settings.cast<double>() = 0.25; return nullptr; });
    access->Flush();
    state->SetActive(false);
    ASSERT_TRUE(state->Automation().SetLane({ "Value", false, { { 0.0, 0.9, 0.0 } } }));
    undo.PushState(TranslatableString::untranslatable("Available effect edited"), {});
    auto restore = [&](const UndoStackElem& element) { ProjectHistory::Get(*m_au3Project).PopState(element.state, false); };
    undo.Undo(restore);
    EXPECT_DOUBLE_EQ(*access->Get().cast<double>(), 0.75);
    EXPECT_TRUE(state->IsEnabled());
    ASSERT_NE(state->Automation().Find("Value"), nullptr);
    EXPECT_DOUBLE_EQ(state->Automation().Find("Value")->points.front().value, 0.4);
    undo.Redo(restore);
    EXPECT_DOUBLE_EQ(*access->Get().cast<double>(), 0.25);
    EXPECT_FALSE(state->IsEnabled());
    EXPECT_DOUBLE_EQ(state->Automation().Find("Value")->points.front().value, 0.9);
}

TEST_F(Effects_RealtimeEffectServiceTests, CachedValidatedMetadataDoesNotMakeAnUnresolvedPluginAvailable)
{
    const auto provider = std::make_shared<CachedMetadataProvider>();
    muse::modularity::globalIoc()->registerExport<IEffectsProvider>("cached-metadata-test", provider);
    const auto unregister = finally([&] { muse::modularity::globalIoc()->unregister<IEffectsProvider>("cached-metadata-test"); });
    bool available = false;
    int attempts = 0;
    RealtimeEffectState::EffectFactory::Scope factory([&](const PluginID&) -> const EffectInstanceFactory* {
        ++attempts;
        return available ? &project::dummyFactory() : nullptr;
    });
    const auto state = RealtimeEffectState::make_shared("test:cached-metadata");
    const auto before = attempts;
    EXPECT_FALSE(m_service->isAvailable(state));
    EXPECT_EQ(attempts, before) << "UI availability queries must not repeatedly try to load missing plugins";
    state->SetActive(false);
    EXPECT_EQ(attempts, before) << "Document bypass must not try to load an unresolved plugin";
    EXPECT_FALSE(state->IsEnabled());
    available = true;
    ASSERT_NE(state->GetEffect(), nullptr);
    EXPECT_FALSE(state->IsEnabled());
    EXPECT_TRUE(m_service->isAvailable(state));
    provider->state = muse::audioplugins::AudioPluginState::Error;
    EXPECT_FALSE(m_service->isAvailable(state));
}
}
