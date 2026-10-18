/*
* Audacity: A Digital Audio Editor
*/
#include <gtest/gtest.h>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QTemporaryDir>
#include <QFile>
#include <QFileInfo>
#include <QDir>
#include <sqlite3.h>
#include <cmath>
#include <chrono>
#include <functional>
#include <thread>
#include <tuple>

#include "../internal/nativeaudiorendersource.h"
#include "../internal/nativeinstrumentfactory.h"
#include "automation/view/trackautomationmodel.h"
#include "mocks/audioenginemock.h"
#include "trackedit/view/trackfreezemodel.h"
#include "trackedit/view/pianorollmodel.h"
#include "audioplugins/tests/mocks/knownaudiopluginsregistermock.h"
#include "trackedit/view/trackroutingmodel.h"
#include "trackedit/internal/au3/au3projecthistory.h"
#include "trackedit/internal/au3/mixrenderscenario.h"
#include "interactive/tests/mocks/interactivemock.h"
#include "trackedit/internal/tracksviewrequestsservice.h"
#include "trackedit/tests/mocks/trackeditprojectmock.h"
#include "context/tests/mocks/globalcontextmock.h"
#include "project/tests/mocks/audacityprojectmock.h"
#include "project/tests/testtools.h"
#include "au3wrap/internal/au3project.h"
#include "au3-midi-track/MidiTrack.h"
#include "au3-effects/MixAndRender.h"
#include "au3-basic-ui/BasicUI.h"
#include "au3-playable-track/TrackAutomation.h"
#include "au3-playable-track/TrackRouting.h"
#include "au3-project-rate/ProjectRate.h"
#include "au3-numeric-formats/ProjectTimeSignature.h"
#include "au3-project/Project.h"
#include "au3-project-file-io/ProjectFileIO.h"
#include "au3-project-file-io/DBConnection.h"
#include "au3-wave-track/TrackFreeze.h"
#include "au3-wave-track/Sequence.h"
#include "au3-wave-track/WaveClip.h"
#include "au3-wave-track/WaveTrack.h"
#include "au3-realtime-effects/RealtimeEffectList.h"
#include "au3-realtime-effects/RealtimeEffectState.h"
#include "au3-realtime-effects/SavedMasterEffectList.h"
#include "au3-project-history/UndoManager.h"
#include "au3-builtin-effects/BassTrebleBase.h"

using testing::NiceMock;
using testing::Return;

namespace au::au3audio {
namespace {
// Keep original services except the modal progress window. Rendering itself
// still uses real Audacity MixAndRender, sources, plugins and sample storage.
class RenderProgressUI final : public BasicUI::Services
{
    class GenericProgress final : public BasicUI::GenericProgressDialog
    {
    public:
        explicit GenericProgress(std::function<void()> onClose = {})
            : onClose(std::move(onClose)) {}
        ~GenericProgress() override
        {
            if (onClose) {
                onClose();
            }
        }

        BasicUI::ProgressResult Pulse() override { return BasicUI::ProgressResult::Success; }
    private:
        std::function<void()> onClose;
    };
    class Progress final : public BasicUI::ProgressDialog
    {
    public:
        BasicUI::ProgressResult Poll(unsigned long long, unsigned long long, const TranslatableString&) override
        {
            return BasicUI::ProgressResult::Success;
        }

        bool Cancelled() const override { return false; }
        void SetMessage(const TranslatableString&) override {}
        void SetDialogTitle(const TranslatableString&) override {}
        void Reinit() override {}
    };
public:
    explicit RenderProgressUI(bool captureErrors = false)
        : previous(BasicUI::Install(this)), captureErrors(captureErrors) {}
    ~RenderProgressUI() override { BasicUI::Install(previous); }
    void DoCallAfter(const BasicUI::Action& action) override { previous->DoCallAfter(action); }
    void DoYield() override { previous->DoYield(); }
    void DoShowErrorDialog(const BasicUI::WindowPlacement& p, const TranslatableString& t, const TranslatableString& m,
                           const ManualPageID& h, const BasicUI::ErrorDialogOptions& o) override
    {
        if (captureErrors) {
            errors.push_back(m.Translation().ToStdString());
        } else {
            previous->DoShowErrorDialog(p, t, m, h, o);
        }
    }

    BasicUI::MessageBoxResult DoMessageBox(const TranslatableString& m, BasicUI::MessageBoxOptions o) override
    {
        return previous->DoMessageBox(m, std::move(o));
    }

    std::unique_ptr<BasicUI::ProgressDialog> DoMakeProgress(const TranslatableString&, const TranslatableString&, unsigned,
                                                            const TranslatableString&) override { return std::make_unique<Progress>(); }
    std::unique_ptr<BasicUI::GenericProgressDialog> DoMakeGenericProgress(const BasicUI::WindowPlacement& p, const TranslatableString& t,
                                                                          const TranslatableString& m, int s) override
    {
        if (captureErrors) {
            return std::make_unique<GenericProgress>([this, t] {
                if (onGenericProgress) {
                    onGenericProgress(t);
                }
            });
        }
        return previous->DoMakeGenericProgress(p, t, m, s);
    }

    int DoMultiDialog(const TranslatableString& m, const TranslatableString& t, const TranslatableStrings& b,
                      const ManualPageID& h, const TranslatableString& box, bool log) override
    {
        return previous->DoMultiDialog(m, t, b, h, box, log);
    }

    bool DoOpenInDefaultBrowser(const wxString& url) override { return previous->DoOpenInDefaultBrowser(url); }
    std::unique_ptr<BasicUI::WindowPlacement> DoFindFocus() override { return previous->DoFindFocus(); }
    void DoSetFocus(const BasicUI::WindowPlacement& p) override { previous->DoSetFocus(p); }
    bool IsUsingRtlLayout() const override { return previous->IsUsingRtlLayout(); }
    bool IsUiThread() const override { return previous->IsUiThread(); }
    std::vector<std::string> errors;
    std::function<void(const TranslatableString&)> onGenericProgress;
private:
    BasicUI::Services* previous;
    const bool captureErrors;
};

// Copy a quiescent SQLite main file and its uncheckpointed journal while its
// writer remains open, reproducing a destination left by an interrupted app.
void makeJournalDestination(const QString& writerPath, const QString& destination)
{
    sqlite3* raw = nullptr;
    ASSERT_EQ(sqlite3_open(writerPath.toUtf8().constData(), &raw), SQLITE_OK);
    std::unique_ptr<sqlite3, decltype(& sqlite3_close)> writer(raw, sqlite3_close);
    ASSERT_EQ(sqlite3_exec(writer.get(),
                           "PRAGMA journal_mode=WAL; PRAGMA wal_autocheckpoint=0;"
                           "CREATE TABLE retired(content BLOB); INSERT INTO retired VALUES(zeroblob(32768));",
                           nullptr, nullptr, nullptr), SQLITE_OK);
    ASSERT_TRUE(QFile::copy(writerPath, destination));
    ASSERT_TRUE(QFile::copy(writerPath + "-wal", destination + "-wal"));
    ASSERT_GT(QFileInfo(destination + "-wal").size(), 32);
}

QByteArray readFile(const QString& path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }
    return file.readAll();
}

QByteArray readStoredDocument(sqlite3* database, const char* table)
{
    QByteArray result;
    const auto query = std::string("SELECT hex(dict)||':'||hex(doc) FROM ") + table + " WHERE id=1;";
    EXPECT_EQ(sqlite3_exec(database, query.c_str(), [](void* result, int, char** values, char**) {
        *static_cast<QByteArray*>(result) = values[0];
        return 0;
    }, &result, nullptr), SQLITE_OK);
    return result;
}

void copyDatabaseSnapshot(sqlite3* source, const QString& destination)
{
    sqlite3* raw = nullptr;
    ASSERT_EQ(sqlite3_open(destination.toUtf8().constData(), &raw), SQLITE_OK);
    std::unique_ptr<sqlite3, decltype(& sqlite3_close)> target(raw, sqlite3_close);
    auto* backup = sqlite3_backup_init(raw, "main", source, "main");
    ASSERT_NE(backup, nullptr);
    int rc;
    QElapsedTimer timeout;
    timeout.start();
    do {
        rc = sqlite3_backup_step(backup, -1);
        if (rc == SQLITE_BUSY || rc == SQLITE_LOCKED) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    } while ((rc == SQLITE_BUSY || rc == SQLITE_LOCKED) && timeout.elapsed() < 1000);
    const auto finish = sqlite3_backup_finish(backup);
    ASSERT_EQ(rc, SQLITE_DONE);
    ASSERT_EQ(finish, SQLITE_OK);
}

class ModelInputEditor final : public audio::IInstrumentEditor
{
public:
    std::vector<audio::InstrumentParameter> parameters() const override { return { { "gain", "Gain", "", gain } }; }
    bool setParameter(const std::string&, double) override { return false; }
    std::string state() const override { ++snapshots; return preset; }
    bool takeChanges() override { return std::exchange(changed, false); }
    bool active() const override { return opened; }
    void close() override { opened = false; ++closed; }
    std::vector<audio::InstrumentMidiInput> midiInputs() const override { return { { "keyboard", "MIDI test keyboard" } }; }
    bool setMidiInput(const std::string& id) override
    {
        ++inputConnections;
        if (!id.empty() && !midiAvailable) {
            return false;
        }
        input = id;
        return true;
    }

    void sendMidi(int status, int data1, int data2) override { sent.push_back({ status, data1, data2, 0.0 }); }
    void releaseNotes() override { ++released; }
    std::vector<audio::InstrumentMidiMessage> takeMidiMessages() override
    {
        auto result = std::move(received);
        received.clear();
        return result;
    }

    mutable int snapshots = 0;
    bool changed = false;
    std::string preset, input;
    bool opened = true;
    bool midiAvailable = true;
    double gain = 0.5;
    int closed = 0, released = 0, inputConnections = 0;
    std::vector<audio::InstrumentMidiMessage> sent, received;
};
class ModelInputHost final : public audio::IInstrumentPlayback
{
public:
    std::shared_ptr<audio::IInstrumentEditor> existingInstrument(AudacityProject&, const MidiTrack& track) const override
    {
        return boundTrack == &track && editor && editor->opened
               && editor->preset == track.GetInstrumentState().utf8_string() ? editor : nullptr;
    }

    muse::RetVal<std::shared_ptr<audio::IInstrumentEditor> > openInstrument(AudacityProject&, MidiTrack& track) override
    {
        editor = std::make_shared<ModelInputEditor>();
        editor->preset = track.GetInstrumentState().utf8_string();
        boundTrack = &track;
        return muse::RetVal<std::shared_ptr<audio::IInstrumentEditor> >::make_ok(editor);
    }

    void closeInstrument(const std::shared_ptr<audio::IInstrumentEditor>& instance) override { instance->close(); }
    MidiTrack* boundTrack = nullptr;
    std::shared_ptr<ModelInputEditor> editor;
};

class MetadataServices final : public audio::IInstrumentEditorFactory, public audio::IPluginAutomation
{
public:
    muse::RetVal<std::shared_ptr<audio::IInstrumentEditor> > create(const std::string& description, const std::string& state) const override
    {
        ++creations;
        if (fail) {
            return muse::make_ret(muse::Ret::Code::InternalError, "Metadata fixture is unavailable");
        }
        return NativeInstrumentFactory().create(description, state);
    }

    std::vector<audio::InstrumentParameter> parameters(RealtimeEffectState& effect) const override
    {
        ++effectQueries;
        if (nativeEffectMetadata) {
            return NativeInstrumentFactory().parameters(effect);
        }
        return { { "value", "Value", "", effectValue } };
    }

    mutable int creations = 0, effectQueries = 0;
    bool fail = false;
    bool nativeEffectMetadata = false;
    double effectValue = 0.25;
};

class MetadataRegistration final
{
public:
    MetadataRegistration()
    {
        muse::modularity::globalIoc()->registerExport<audio::IInstrumentEditorFactory>("metadata-tests", services);
        muse::modularity::globalIoc()->registerExport<audio::IPluginAutomation>("metadata-tests", services);
    }

    ~MetadataRegistration()
    {
        muse::modularity::globalIoc()->unregister<audio::IInstrumentEditorFactory>("metadata-tests");
        muse::modularity::globalIoc()->unregister<audio::IPluginAutomation>("metadata-tests");
    }

    std::shared_ptr<MetadataServices> services = std::make_shared<MetadataServices>();
};

class MetadataEffect final : public BassTrebleBase
{
public:
    std::shared_ptr<EffectInstance> MakeInstance() const override { return std::make_shared<Instance>(*this); }
};
}
class NativeEditModelTests : public testing::Test
{
protected:
    void SetUp() override
    {
        ASSERT_TRUE(directory.isValid());
        path = directory.filePath("freeze.aup4").toStdString();
        const auto fixture = std::string(au3audio_tests_DATA_ROOT) + "/../../trackedit/tests/data/empty.aup4";
        ASSERT_TRUE(testtools::copyFile(fixture, path.toStdString()));
        accessor = std::make_shared<au3::Au3ProjectAccessor>(muse::modularity::globalCtx());
        ASSERT_TRUE(accessor->load(path, false));
        ProjectRate::Get(project()).SetRate(48000.0);
        registerNativeAudioRenderer();
        ctx = std::make_shared<muse::modularity::Context>(421);
        context = std::make_shared<NiceMock<context::GlobalContextMock> >();
        document = std::make_shared<NiceMock<project::AudacityProjectMock> >();
        trackeditProject = std::make_shared<NiceMock<trackedit::TrackeditProjectMock> >();
        audio = std::make_shared<NiceMock<audio::AudioEngineMock> >();
        ON_CALL(*document, au3ProjectPtr()).WillByDefault([this] { return accessor->au3ProjectPtr(); });
        ON_CALL(*context, currentProject()).WillByDefault(Return(document));
        ON_CALL(*context, currentTrackeditProject()).WillByDefault(Return(trackeditProject));
        auto* ioc = muse::modularity::ioc(ctx);
        ioc->registerExport<context::IGlobalContext>("utests", context);
        history = std::make_shared<trackedit::Au3ProjectHistory>(ctx);
        requests = std::make_shared<trackedit::TracksViewRequestsService>(ctx);
        ioc->registerExport<trackedit::IProjectHistory>("utests", history);
        ioc->registerExport<trackedit::ITracksViewRequestsService>("utests", requests);
        muse::modularity::globalIoc()->registerExport<audio::IAudioEngine>("utests", audio);
        inputHost = std::make_shared<ModelInputHost>();
        plugins = std::make_shared<NiceMock<muse::audioplugins::KnownAudioPluginsRegisterMock> >();
        muse::modularity::globalIoc()->registerExport<audio::IInstrumentPlayback>("utests", inputHost);
        muse::modularity::globalIoc()->registerExport<muse::audioplugins::IKnownAudioPluginsRegister>("utests", plugins);
        history->init();
    }

    void TearDown() override
    {
        muse::modularity::globalIoc()->unregister<audio::IAudioEngine>("utests");
        muse::modularity::globalIoc()->unregister<audio::IInstrumentPlayback>("utests");
        muse::modularity::globalIoc()->unregister<muse::audioplugins::IKnownAudioPluginsRegister>("utests");
        history.reset();
        requests.reset();
        muse::modularity::removeIoC(ctx);
        accessor->clearSavedState();
        closeProject();
        accessor.reset();
    }

    AudacityProject& project() { return *reinterpret_cast<AudacityProject*>(accessor->au3ProjectPtr()); }
    std::shared_ptr<WaveTrack> wave()
    {
        auto track = WaveTrackFactory::Get(project()).Create(1, floatSample, 48000.0);
        auto clip = track->CreateClip(0.0, "Original audio");
        samples.resize(1536);
        for (size_t i = 0; i < samples.size(); ++i) {
            samples[i] = 0.4f * std::sin(i * 0.17);
        }
        constSamplePtr data[] { reinterpret_cast<constSamplePtr>(samples.data()) };
        clip->Append(data, floatSample, samples.size(), 1, floatSample);
        clip->Flush();
        track->InsertInterval(clip, true);
        TrackList::Get(project()).Add(track);
        track->SetVolume(0.7);
        track->SetPan(-0.25);
        TrackAutomation::Get(*track).SetLane({ "volume", true, { { 0.0, 0.65, 0.0 }, { 0.016, 0.8, 0.0 } } });
        return track;
    }

    std::vector<float> render(const PlayableTrack& track, bool routing = false)
    {
        AudioRenderSource::Options options;
        options.sampleRate = 48000.0;
        options.stopTime = track.GetEndTime();
        options.trackRouting = routing;
        options.bufferSize = 73;
        AudioRenderSource::Tracks waves;
        if (const auto* wave = dynamic_cast<const WaveTrack*>(&track)) {
            waves.push_back(std::static_pointer_cast<const WaveTrack>(wave->shared_from_this()));
        }
        if (const auto* midi = dynamic_cast<const MidiTrack*>(&track)) {
            options.midiTracks.push_back(std::static_pointer_cast<const MidiTrack>(midi->shared_from_this()));
        }
        auto source = AudioRenderSource::Create(project(), waves, options);
        std::vector<float> result;
        while (const auto count = source->Process()) {
            for (size_t i = 0; i < count; ++i) {
                for (int channel = 0; channel < 2; ++channel) {
                    result.push_back(reinterpret_cast<const float*>(source->GetBuffer(channel))[i]);
                }
            }
        }
        return result;
    }

    void finish(trackedit::TrackFreezeModel& model)
    {
        QElapsedTimer timeout;
        timeout.start();
        while (model.busy() && timeout.elapsed() < 5000) {
            QCoreApplication::processEvents();
        }
        ASSERT_FALSE(model.busy());
        ASSERT_TRUE(model.error().isEmpty()) << model.error().toStdString();
        ASSERT_TRUE(model.frozen());
    }

    void closeProject()
    {
        // Exercise real close and compaction; only the progress window is fake.
        RenderProgressUI progress(true);
        accessor->close();
        EXPECT_TRUE(progress.errors.empty()) << testing::PrintToString(progress.errors);
    }

    void reopen()
    {
        ASSERT_TRUE(accessor->save(path));
        // The application retains this saved snapshot through close so its
        // sample blocks are locked before undo states and tracks are purged.
        closeProject();
        accessor = std::make_shared<au3::Au3ProjectAccessor>(muse::modularity::globalCtx());
        ASSERT_TRUE(accessor->load(path, false));
    }

    void expectSamples(const std::vector<float>& expected, const std::vector<float>& actual)
    {
        ASSERT_EQ(expected.size(), actual.size());
        for (size_t i = 0; i < expected.size(); ++i) {
            EXPECT_NEAR(expected[i], actual[i], 2e-6) << i;
        }
    }

    QTemporaryDir directory;
    muse::io::path_t path;
    muse::modularity::ContextPtr ctx;
    std::shared_ptr<au3::Au3ProjectAccessor> accessor;
    std::shared_ptr<NiceMock<context::GlobalContextMock> > context;
    std::shared_ptr<NiceMock<project::AudacityProjectMock> > document;
    std::shared_ptr<NiceMock<trackedit::TrackeditProjectMock> > trackeditProject;
    std::shared_ptr<NiceMock<audio::AudioEngineMock> > audio;
    std::shared_ptr<trackedit::Au3ProjectHistory> history;
    std::shared_ptr<trackedit::TracksViewRequestsService> requests;
    std::vector<float> samples;
    std::shared_ptr<ModelInputHost> inputHost;
    std::shared_ptr<NiceMock<muse::audioplugins::KnownAudioPluginsRegisterMock> > plugins;
};

TEST_F(NativeEditModelTests, InstrumentMenuDistinguishesFormatsAndInstallationsWithoutChangingPluginIdentity)
{
    using namespace muse::audioplugins;
    auto plugin = [](const char* id, const char* type, const char* title) {
        AudioPluginInfo info;
        info.meta.id = id;
        info.meta.type = type;
        info.meta.vendor = "Synth vendor";
        info.meta.attributes[u"title"] = muse::String::fromUtf8(title);
        info.meta.attributes[u"type"] = u"Instrument";
        info.state = AudioPluginState::Validated;
        return info;
    };
    auto unavailable = plugin("broken", "VstPlugin", "Broken synth");
    unavailable.state = AudioPluginState::Error;
    auto effect = plugin("effect", "VstPlugin", "Audio effect");
    effect.meta.attributes[u"type"] = u"Effect";
    ON_CALL(*plugins, pluginInfoList(testing::_)).WillByDefault(Return(AudioPluginInfoList {
            plugin("au-synth", "AudioUnit", "Synth"), plugin("vst-synth", "VstPlugin", "Synth"),
            plugin("lv2-synth", "Lv2Plugin", "LV2 synth"), plugin("vst-copy-one", "VstPlugin", "Duplicate"),
            plugin("vst-copy-two", "VstPlugin", "Duplicate"), unavailable, effect
        }));
    trackedit::PianoRollModel model(ctx);
    model.init();
    const auto choices = model.instruments();
    ASSERT_EQ(choices.size(), 6);
    EXPECT_EQ(choices[1].toMap().value("title").toString(), "Synth (AU)");
    EXPECT_EQ(choices[2].toMap().value("title").toString(), "Synth (VST3)");
    EXPECT_EQ(choices[3].toMap().value("title").toString(), "LV2 synth");
    EXPECT_EQ(choices[4].toMap().value("title").toString(), "Duplicate (VST3) (Synth vendor) (vst-copy-one)");
    EXPECT_NE(choices[4].toMap().value("title"), choices[5].toMap().value("title"));
    EXPECT_EQ(choices[1].toMap().value("description").toString(), "au-synth");
    EXPECT_EQ(choices[2].toMap().value("description").toString(), "vst-synth");
}

TEST_F(NativeEditModelTests, WaveFreezeUnfreezeRetainsSamplesAcrossUndoAndRealSaveReopen)
{
    auto original = wave();
    const auto expected = render(*original);
    const auto id = original->GetId();
    history->pushHistoryState("Audio", "Audio");
    trackedit::TrackFreezeModel model(ctx);
    model.init();
    model.setTrackId(static_cast<int64_t>(id));
    model.freeze(0.0);
    finish(model);
    auto frozen = *TrackList::Get(project()).Any<WaveTrack>().begin();
    expectSamples(expected, render(*frozen));
    ASSERT_FALSE(TrackFreeze::Get(*frozen).blocks.empty());
    history->undo();
    EXPECT_FALSE(model.frozen());
    expectSamples(expected, render(**TrackList::Get(project()).Any<WaveTrack>().begin()));
    history->redo();
    EXPECT_TRUE(model.frozen());
    // Release every external reference to the original samples before closing.
    original.reset();
    reopen();
    frozen = *TrackList::Get(project()).Any<WaveTrack>().begin();
    model.setTrackId(static_cast<int64_t>(frozen->GetId()));
    ASSERT_TRUE(model.frozen());
    ASSERT_FALSE(TrackFreeze::Get(*frozen).blocks.empty());
    expectSamples(expected, render(*frozen));
    model.unfreeze();
    ASSERT_TRUE(model.error().isEmpty()) << model.error().toStdString();
    ASSERT_FALSE(model.frozen());
    auto* restored = *TrackList::Get(project()).Any<WaveTrack>().begin();
    ASSERT_EQ(restored->NChannels(), 1u);
    expectSamples(expected, render(*restored));
    const auto sequence = restored->GetSortedClipByIndex(0)->GetSequence(0);
    std::vector<float> actual(samples.size());
    ASSERT_TRUE(sequence->Get(reinterpret_cast<samplePtr>(actual.data()), floatSample, 0, actual.size(), true));
    EXPECT_EQ(actual, samples);
}

TEST_F(NativeEditModelTests, MidiFreezeReopenUnfreezeRestoresNotesPresetAndAutomation)
{
    auto original = MidiTrack::Create();
    original->SetInstrument({}, "{\"0\":0.25}");
    original->SetGain(0.6);
    original->SetPan(0.2);
    ASSERT_TRUE(original->SetClips({ { 2, "Phrase", 0.0, 0.08, 1, { { 5, 0.01, 0.05, 67, 96 } }, {} } }));
    TrackAutomation::Get(*original).SetLane({ "pan", true, { { 0.0, 0.4, 0.0 } } });
    TrackList::Get(project()).Add(original);
    const auto clips = original->GetClips();
    const auto preset = original->GetInstrumentState();
    const auto expected = render(*original);
    ASSERT_TRUE(std::any_of(expected.begin(), expected.end(), [](float sample) { return std::abs(sample) > 1e-3; }));
    trackedit::TrackFreezeModel model(ctx);
    model.init();
    model.setTrackId(static_cast<int64_t>(original->GetId()));
    model.freeze(0.0);
    finish(model);
    expectSamples(expected, render(**TrackList::Get(project()).Any<WaveTrack>().begin()));
    original.reset();
    reopen();
    auto* frozen = *TrackList::Get(project()).Any<WaveTrack>().begin();
    model.setTrackId(static_cast<int64_t>(frozen->GetId()));
    model.unfreeze();
    ASSERT_TRUE(model.error().isEmpty()) << model.error().toStdString();
    const auto tracks = TrackList::Get(project()).Any<MidiTrack>();
    ASSERT_EQ(tracks.size(), 1u);
    auto* restored = *tracks.begin();
    EXPECT_EQ(restored->GetInstrumentState(), preset);
    ASSERT_EQ(restored->GetClips().size(), clips.size());
    const auto& clip = restored->GetClips().front();
    EXPECT_EQ(clip.id, clips.front().id);
    EXPECT_EQ(clip.name, clips.front().name);
    EXPECT_DOUBLE_EQ(clip.start, clips.front().start);
    EXPECT_DOUBLE_EQ(clip.length, clips.front().length);
    EXPECT_EQ(clip.channel, clips.front().channel);
    ASSERT_EQ(clip.notes.size(), 1u);
    EXPECT_EQ(clip.notes.front().id, 5);
    EXPECT_EQ(clip.notes.front().pitch, 67);
    EXPECT_EQ(clip.notes.front().velocity, 96);
    EXPECT_DOUBLE_EQ(clip.notes.front().start, 0.01);
    EXPECT_DOUBLE_EQ(clip.notes.front().length, 0.05);
    ASSERT_NE(TrackAutomation::Get(*restored).Find("pan"), nullptr);
    expectSamples(expected, render(*restored));
}

TEST_F(NativeEditModelTests, FreezeCancellationAndSourceEditsNeverReplaceTheOriginal)
{
    auto source = wave();
    history->pushHistoryState("Source", "Source");
    const auto state = history->currentStateIndex();
    trackedit::TrackFreezeModel model(ctx);
    model.init();
    model.setTrackId(static_cast<int64_t>(source->GetId()));
    model.freeze(0.2);
    ASSERT_TRUE(model.busy());
    model.cancel();
    QCoreApplication::processEvents();
    EXPECT_FALSE(model.busy());
    EXPECT_FALSE(model.frozen());
    EXPECT_EQ(TrackList::Get(project()).FindById(source->GetId()), source.get());
    EXPECT_EQ(history->currentStateIndex(), state);

    model.freeze(0.0);
    ASSERT_TRUE(model.busy());
    source->SetName("Edited while freeze was running");
    QElapsedTimer timeout;
    timeout.start();
    while (model.busy() && timeout.elapsed() < 5000) {
        QCoreApplication::processEvents();
    }
    ASSERT_FALSE(model.busy());
    EXPECT_FALSE(model.error().isEmpty());
    EXPECT_FALSE(model.frozen());
    EXPECT_EQ(TrackList::Get(project()).FindById(source->GetId()), source.get());
    EXPECT_EQ(source->GetName(), "Edited while freeze was running");
    EXPECT_EQ(history->currentStateIndex(), state);
}

TEST_F(NativeEditModelTests, MidiFreezeIncludesAudibleReleaseTailAndUnfreezeRestoresTheOriginalBounds)
{
    auto source = MidiTrack::Create();
    source->SetInstrument({}, "{\"1\":0,\"2\":0.05}");
    ASSERT_TRUE(source->SetClips({ { 1, "Short note", 0.0, 0.08, 1, { { 1, 0.0, 0.05, 60, 100 } }, {} } }));
    TrackList::Get(project()).Add(source);
    const double end = source->GetEndTime();
    trackedit::TrackFreezeModel model(ctx);
    model.init();
    model.setTrackId(static_cast<int64_t>(source->GetId()));
    model.freeze(0.125);
    finish(model);
    const auto* frozen = *TrackList::Get(project()).Any<WaveTrack>().begin();
    ASSERT_NE(frozen, nullptr);
    EXPECT_NEAR(frozen->GetEndTime(), end + 0.125, 1.0 / 48000);
    const auto samples = render(*frozen);
    const auto tailStart = static_cast<size_t>(std::ceil(end * 48000)) * 2;
    ASSERT_LT(tailStart, samples.size());
    EXPECT_TRUE(std::any_of(samples.begin() + tailStart, samples.end(), [](float sample) { return std::abs(sample) > 1e-3; }));
    model.unfreeze();
    ASSERT_TRUE(model.error().isEmpty()) << model.error().toStdString();
    const auto restored = TrackList::Get(project()).Any<MidiTrack>();
    ASSERT_EQ(restored.size(), 1u);
    EXPECT_DOUBLE_EQ((*restored.begin())->GetEndTime(), end);
}

TEST_F(NativeEditModelTests, LegacyUiMidiFreezeWithDuplicateGainUnfreezesAfterSaveAndReopen)
{
    auto original = MidiTrack::Create();
    original->SetGain(0.6);
    original->SetInstrument({}, "{\"0\":0.25}");
    ASSERT_TRUE(original->SetClips({ { 2, "Phrase", 0.0, 0.08, 1, { { 5, 0.01, 0.05, 67, 96 } }, {} } }));
    TrackList::Get(project()).Add(original);
    const auto expected = render(*original);
    trackedit::TrackFreezeModel model(ctx);
    model.init();
    model.setTrackId(static_cast<int64_t>(original->GetId()));
    model.freeze(0.0);
    finish(model);
    auto* frozen = *TrackList::Get(project()).Any<WaveTrack>().begin();
    auto& source = TrackFreeze::Get(*frozen).sourceXml;
    source.Replace("midigain=", "gain=");
    source.Replace("<miditrack ", "<miditrack gain=\"20\" ");
    original.reset();
    reopen();
    frozen = *TrackList::Get(project()).Any<WaveTrack>().begin();
    model.setTrackId(static_cast<int64_t>(frozen->GetId()));
    model.unfreeze();
    ASSERT_TRUE(model.error().isEmpty()) << model.error().toStdString();
    const auto tracks = TrackList::Get(project()).Any<MidiTrack>();
    ASSERT_EQ(tracks.size(), 1u);
    const auto* restored = *tracks.begin();
    EXPECT_NEAR(restored->GetGain(), 0.6, 1e-7);
    EXPECT_EQ(restored->GetInstrumentState(), "{\"0\":0.25}");
    ASSERT_EQ(restored->GetClips().size(), 1u);
    ASSERT_EQ(restored->GetClips().front().notes.size(), 1u);
    EXPECT_EQ(restored->GetClips().front().notes.front().pitch, 67);
    expectSamples(expected, render(*restored));
}

TEST_F(NativeEditModelTests, FrozenSourceCanMoveToAnotherProjectWithoutKeepingTheOldDatabaseAlive)
{
    auto original = wave();
    const auto expected = render(*original);
    trackedit::TrackFreezeModel model(ctx);
    model.setTrackId(static_cast<int64_t>(original->GetId()));
    model.freeze(0.0);
    finish(model);
    auto* frozen = *TrackList::Get(project()).Any<WaveTrack>().begin();
    const auto otherPath = directory.filePath("pasted.aup4").toStdString();
    const auto fixture = std::string(au3audio_tests_DATA_ROOT) + "/../../trackedit/tests/data/empty.aup4";
    ASSERT_TRUE(testtools::copyFile(fixture, otherPath));
    auto next = std::make_shared<au3::Au3ProjectAccessor>(muse::modularity::globalCtx());
    ASSERT_TRUE(next->load(otherPath, false));
    auto& other = *reinterpret_cast<AudacityProject*>(next->au3ProjectPtr());
    ProjectRate::Get(other).SetRate(48000.0);
    auto copied = frozen->PasteInto(other, TrackList::Get(other));
    ASSERT_NE(copied, nullptr);
    original.reset();
    closeProject();
    accessor = std::move(next);
    path = otherPath;
    copied.reset();
    reopen();
    auto* pasted = *TrackList::Get(project()).Any<WaveTrack>().begin();
    model.setTrackId(static_cast<int64_t>(pasted->GetId()));
    ASSERT_TRUE(model.frozen());
    model.unfreeze();
    ASSERT_TRUE(model.error().isEmpty()) << model.error().toStdString();
    ASSERT_FALSE(model.frozen());
    expectSamples(expected, render(**TrackList::Get(project()).Any<WaveTrack>().begin()));
}

TEST_F(NativeEditModelTests, RoutingEditsStopOwnMonitoringAndRejectFeedbackWithoutChangingHistory)
{
    auto source = wave();
    trackedit::TrackRoutingModel routing(ctx);
    routing.init();
    routing.setTrackId(static_cast<int64_t>(source->GetId()));
    std::optional<audio::AudioStreamDescriptor> stream { { audio::AudioStreamKind::Monitoring, &project(), 48000.0 } };
    ON_CALL(*audio, currentStream()).WillByDefault([&] { return stream; });
    ON_CALL(*audio, canStopAudioStream(testing::_)).WillByDefault(Return(true));
    EXPECT_CALL(*audio, stopStream()).Times(1).WillOnce([&] { stream.reset(); });
    routing.addBus();
    routing.addBus();
    ASSERT_EQ(routing.buses().size(), 3);
    routing.setOutput(1);
    routing.setSend(2, true, 0.35, true);
    const auto route = TrackRouting::Get(*source);
    EXPECT_FALSE(route.output.empty());
    ASSERT_EQ(route.sends.size(), 1u);
    EXPECT_DOUBLE_EQ(route.sends.front().gain, 0.35);
    EXPECT_TRUE(route.sends.front().preFader);
    auto buses = TrackList::Get(project()).Any<WaveTrack>();
    WaveTrack* first = nullptr;
    WaveTrack* second = nullptr;
    for (auto* bus : buses) {
        const auto& id = TrackRouting::Get(*bus).busId;
        if (id == route.output) {
            first = bus;
        }
        if (id == route.sends.front().bus) {
            second = bus;
        }
    }
    ASSERT_NE(first, nullptr);
    ASSERT_NE(second, nullptr);
    routing.setTrackId(static_cast<int64_t>(first->GetId()));
    routing.setOutput(1); // Bus 1 -> Bus 2 (self is excluded from the menu).
    routing.setTrackId(static_cast<int64_t>(second->GetId()));
    const auto historyBeforeCycle = history->currentStateIndex();
    routing.setOutput(1); // Bus 2 -> Bus 1 would close the loop.
    EXPECT_FALSE(routing.error().isEmpty());
    EXPECT_TRUE(TrackRouting::Get(*second).output.empty());
    EXPECT_EQ(history->currentStateIndex(), historyBeforeCycle);
    history->undo();
    first = nullptr;
    for (auto* bus : TrackList::Get(project()).Any<WaveTrack>()) {
        if (TrackRouting::Get(*bus).busId == route.output) {
            first = bus;
        }
    }
    ASSERT_NE(first, nullptr);
    EXPECT_TRUE(TrackRouting::Get(*first).output.empty());
    history->redo();
    source.reset();
    reopen();
    const auto* reopenedSource = *TrackList::Get(project()).Any<WaveTrack>().begin();
    const auto& reopenedRoute = TrackRouting::Get(*reopenedSource);
    EXPECT_EQ(reopenedRoute.output, route.output);
    EXPECT_EQ(reopenedRoute.sends, route.sends);
    EXPECT_TRUE(TrackRouting::Validate(TrackList::Get(project())));
}

class NativeSaveDestinationTests : public NativeEditModelTests, public testing::WithParamInterface<bool>
{
};

TEST_F(NativeEditModelTests, SaveAsCompactsTheOriginalUsingItsSavedDocumentAndKeepsLiveStateInTheDestination)
{
    RenderProgressUI ui(true);
    const auto priorTempo = ProjectTimeSignature::Get(project()).GetTempo();
    const auto priorMeter = ProjectTimeSignature::Get(project()).GetUpperTimeSignature();
    const auto restorePreferences = finally([&] {
        ProjectTimeSignature::Get(project()).SetTempo(priorTempo);
        ProjectTimeSignature::Get(project()).SetUpperTimeSignature(priorMeter);
    });
    static MetadataEffect effect;
    RealtimeEffectState::EffectFactory::Scope factory { [](const PluginID&) { return &effect; } };
    ProjectTimeSignature::Get(project()).SetTempo(120.0);
    ProjectTimeSignature::Get(project()).SetUpperTimeSignature(4);
    auto source = wave();
    auto& master = RealtimeEffectList::Get(project());
    ASSERT_TRUE(master.AddState(RealtimeEffectState::make_shared("saved-master")));
    history->pushHistoryState("Saved document", "Saved");
    ASSERT_TRUE(accessor->save(path));
    ASSERT_EQ(SavedMasterEffectList::Get(project()).List().GetStatesCount(), 1u);

    // Leave free database pages so Save As actually replaces the source with
    // its compacted copy, rather than keeping the equally sized original.
    {
        auto discarded = WaveTrackFactory::Get(project()).Create(1, floatSample, 48000.0);
        auto clip = discarded->CreateClip(0.0, "Discarded audio");
        std::vector<float> waste(1024 * 1024, 0.25f);
        constSamplePtr data[] { reinterpret_cast<constSamplePtr>(waste.data()) };
        clip->Append(data, floatSample, waste.size(), 1, floatSample);
        clip->Flush();
        discarded->InsertInterval(clip, true);
    }
    auto* database = ProjectFileIO::Get(project()).GetConnection().DB();
    QElapsedTimer checkpointWait;
    checkpointWait.start();
    int checkpointResult;
    do {
        checkpointResult = sqlite3_wal_checkpoint_v2(database, nullptr, SQLITE_CHECKPOINT_FULL, nullptr, nullptr);
        if (checkpointResult == SQLITE_BUSY) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    } while (checkpointResult == SQLITE_BUSY && checkpointWait.elapsed() < 5000);
    ASSERT_EQ(checkpointResult, SQLITE_OK);
    const auto beforeSize = QFileInfo(path.toQString()).size();
    ASSERT_GT(beforeSize, 1024 * 1024);

    master.GetStateAt(0)->SetActive(false);
    ProjectTimeSignature::Get(project()).SetTempo(150.0);
    ProjectTimeSignature::Get(project()).SetUpperTimeSignature(7);
    ASSERT_TRUE(master.AddState(RealtimeEffectState::make_shared("new-master")));
    history->pushHistoryState("Unsaved master effect", "Effect");
    const auto destination = directory.filePath("new-master.aup4");
    ASSERT_TRUE(accessor->save(muse::io::path_t(destination)));
    ASSERT_LT(QFileInfo(path.toQString()).size(), beforeSize) << "The source compaction path was not exercised";
    EXPECT_EQ(master.GetStatesCount(), 2u);
    source.reset();
    for (const auto& [file, expectedCount] : std::vector<std::pair<muse::io::path_t, size_t> > {
            { path, 1u }, { muse::io::path_t(destination), 2u } }) {
        closeProject();
        accessor = std::make_shared<au3::Au3ProjectAccessor>(muse::modularity::globalCtx());
        ASSERT_TRUE(accessor->load(file, false));
        const auto& loaded = RealtimeEffectList::Get(project());
        EXPECT_EQ(loaded.GetStatesCount(), expectedCount) << file.toStdString();
        EXPECT_DOUBLE_EQ(ProjectTimeSignature::Get(project()).GetTempo(), expectedCount == 1 ? 120.0 : 150.0);
        EXPECT_EQ(ProjectTimeSignature::Get(project()).GetUpperTimeSignature(), expectedCount == 1 ? 4 : 7);
        ASSERT_GE(loaded.GetStatesCount(), 1u);
        EXPECT_EQ(loaded.GetStateAt(0)->GetID().ToStdString(), "saved-master");
        EXPECT_EQ(loaded.GetStateAt(0)->IsEnabled(), expectedCount == 1);
    }
    EXPECT_TRUE(ui.errors.empty());
}

TEST_F(NativeEditModelTests, FailedSaveAsKeepsTheLastSavedMasterSnapshotAndTheUnsavedLiveEffectsSeparate)
{
    RenderProgressUI ui(true);
    static MetadataEffect effect;
    RealtimeEffectState::EffectFactory::Scope factory { [](const PluginID&) { return &effect; } };
    const auto source = wave();
    auto& live = RealtimeEffectList::Get(project());
    ASSERT_TRUE(live.AddState(RealtimeEffectState::make_shared("saved-master")));
    history->pushHistoryState("Saved document", "Saved");
    ASSERT_TRUE(accessor->save(path));
    auto& saved = SavedMasterEffectList::Get(project());
    const auto* previous = &saved.List();
    ASSERT_EQ(previous->GetStatesCount(), 1u);
    ASSERT_TRUE(live.AddState(RealtimeEffectState::make_shared("new-master")));
    history->pushHistoryState("Unsaved master effect", "Effect");

    auto* database = ProjectFileIO::Get(project()).GetConnection().DB();
    ASSERT_EQ(sqlite3_set_authorizer(database, [](void*, int action, const char*, const char*, const char* schema, const char*) {
        return action == SQLITE_CREATE_TABLE && schema && std::string_view(schema) == "outbound" ? SQLITE_DENY : SQLITE_OK;
    }, nullptr), SQLITE_OK);
    const auto clearAuthorizer = finally([&] { sqlite3_set_authorizer(database, nullptr, nullptr); });
    EXPECT_FALSE(accessor->save(muse::io::path_t(directory.filePath("failed.aup4"))));
    EXPECT_EQ(accessor->getFileName(), path);
    EXPECT_EQ(ProjectFileIO::Get(project()).GetConnection().DB(), database);
    EXPECT_TRUE(accessor->hasUnsavedChanges());
    EXPECT_EQ(&saved.List(), previous);
    EXPECT_EQ(saved.List().GetStatesCount(), 1u);
    EXPECT_EQ(live.GetStatesCount(), 2u);
}

class NativeSaveFinalizationTests : public NativeEditModelTests, public testing::WithParamInterface<std::tuple<bool, bool> >
{
};

TEST_P(NativeSaveFinalizationTests, AFinalWriteFailureRetainsSourceOwnershipRecoveryDataAndTheOldDestination)
{
    const auto [overwrite, failAutosaveDelete] = GetParam();
    RenderProgressUI ui(true);
    static MetadataEffect effect;
    RealtimeEffectState::EffectFactory::Scope factory { [](const PluginID&) { return &effect; } };
    auto source = wave();
    auto midi = MidiTrack::Create();
    ASSERT_TRUE(midi->SetClips({ { 1, "Saved MIDI", 0, 4, 1, { { 1, 0, 1, 60, 95 } }, {} } }));
    TrackList::Get(project()).Add(midi);
    auto& master = RealtimeEffectList::Get(project());
    ASSERT_TRUE(master.AddState(RealtimeEffectState::make_shared("saved-master")));
    history->pushHistoryState("Saved document", "Saved");
    ASSERT_TRUE(accessor->save(path));
    const auto* savedMaster = &SavedMasterEffectList::Get(project()).List();
    auto clips = midi->GetClips();
    clips.front().notes.push_back({ 2, 1.5, 0.5, 67, 110 });
    ASSERT_TRUE(midi->SetClips(std::move(clips)));
    source->SetVolume(0.5);
    ASSERT_TRUE(master.AddState(RealtimeEffectState::make_shared("unsaved-master")));
    history->pushHistoryState("Unsaved notes and effects", "MIDI");
    const auto expectedAudio = render(*source), expectedMidi = render(*midi);
    auto& io = ProjectFileIO::Get(project());
    ASSERT_TRUE(io.AutoSave());
    const auto readRecovery = [&] {
        wxString result;
        EXPECT_EQ(sqlite3_exec(io.GetConnection().DB(), "SELECT hex(dict)||':'||hex(doc) FROM autosave WHERE id=1;",
                               [](void* result, int, char** values, char**) {
            *static_cast<wxString*>(result) = wxString::FromUTF8(values[0]);
            return 0;
        }, &result, nullptr), SQLITE_OK);
        return result;
    };
    const auto recovery = readRecovery();
    ASSERT_FALSE(recovery.empty());
    auto* originalConnection = io.GetConnection().DB();
    const auto destination = directory.filePath("final-write-failure.aup4");
    QByteArray oldDatabase, oldJournal;
    if (overwrite) {
        ASSERT_NO_FATAL_FAILURE(makeJournalDestination(directory.filePath("old-writer.sqlite"), destination));
        oldDatabase = readFile(destination);
        oldJournal = readFile(destination + "-wal");
        ASSERT_FALSE(oldDatabase.isEmpty());
        ASSERT_FALSE(oldJournal.isEmpty());
    }
    bool injected = false;
    ui.onGenericProgress = [&](const TranslatableString& title) {
        if (injected || title.debugStr() != "Syncing") {
            return;
        }
        sqlite3* raw = nullptr;
        ASSERT_EQ(sqlite3_open_v2(destination.toUtf8().constData(), &raw, SQLITE_OPEN_READWRITE, nullptr), SQLITE_OK);
        std::unique_ptr<sqlite3, decltype(& sqlite3_close)> connection(raw, sqlite3_close);
        ASSERT_EQ(sqlite3_busy_timeout(raw, 1000), SQLITE_OK);
        // CopyTo has finished. Fail a real destination statement during the
        // final save, after its new connection has begun opening.
        const auto* sql = failAutosaveDelete
                          ? "INSERT INTO autosave SELECT * FROM project;"
                            "CREATE TRIGGER reject_final_save BEFORE DELETE ON autosave BEGIN SELECT RAISE(ABORT,'injected save failure'); END;"
                          :
                          "CREATE TRIGGER reject_final_save BEFORE INSERT ON project BEGIN SELECT RAISE(ABORT,'injected save failure'); END;";
        ASSERT_EQ(sqlite3_exec(raw, sql, nullptr, nullptr, nullptr), SQLITE_OK) << sqlite3_errmsg(raw);
        injected = true;
    };
    EXPECT_FALSE(accessor->save(muse::io::path_t(destination)));
    ui.onGenericProgress = {};
    ASSERT_TRUE(injected) << "The destination finalization failure was not exercised";
    EXPECT_FALSE(ui.errors.empty());
    EXPECT_TRUE(io.GetLibraryError().Translation().Contains("injected save failure"));
    ASSERT_EQ(accessor->getFileName(), path);
    ASSERT_EQ(io.GetConnection().DB(), originalConnection);
    EXPECT_TRUE(io.IsModified());
    EXPECT_TRUE(accessor->hasUnsavedChanges());
    EXPECT_EQ(&SavedMasterEffectList::Get(project()).List(), savedMaster);
    EXPECT_EQ(SavedMasterEffectList::Get(project()).List().GetStatesCount(), 1u);
    EXPECT_EQ(master.GetStatesCount(), 2u);
    EXPECT_EQ(readRecovery(), recovery);
    EXPECT_EQ(render(*source), expectedAudio);
    EXPECT_EQ(render(*midi), expectedMidi);
    if (overwrite) {
        EXPECT_EQ(readFile(destination), oldDatabase);
        EXPECT_EQ(readFile(destination + "-wal"), oldJournal);
    } else {
        EXPECT_FALSE(QFileInfo::exists(destination));
        EXPECT_FALSE(QFileInfo::exists(destination + "-wal"));
    }
    EXPECT_TRUE(QDir(directory.path()).entryList({ "*.bak*" }, QDir::Files).empty());
    const auto retry = muse::io::path_t(directory.filePath("retry-after-final-write-failure.aup4"));
    ASSERT_TRUE(accessor->save(retry));
    EXPECT_EQ(accessor->getFileName(), retry);
    source.reset();
    midi.reset();
    for (const auto& [file, noteCount, effects] : std::vector<std::tuple<muse::io::path_t, size_t, size_t> > {
            { path, 1, 1 }, { retry, 2, 2 } }) {
        closeProject();
        accessor = std::make_shared<au3::Au3ProjectAccessor>(muse::modularity::globalCtx());
        ASSERT_TRUE(accessor->load(file, false));
        const auto tracks = TrackList::Get(project()).Any<MidiTrack>();
        ASSERT_FALSE(tracks.empty());
        ASSERT_EQ((*tracks.begin())->GetClips().size(), 1u);
        EXPECT_EQ((*tracks.begin())->GetClips().front().notes.size(), noteCount);
        EXPECT_EQ(RealtimeEffectList::Get(project()).GetStatesCount(), effects);
    }
}

INSTANTIATE_TEST_SUITE_P(NewAndExistingDestinations, NativeSaveFinalizationTests,
                         testing::Combine(testing::Bool(), testing::Bool()));

TEST_P(NativeSaveFinalizationTests, AFailedFirstSaveRestoresTheTemporaryFileAndRetainsItsRecoveryDocument)
{
    const auto [overwrite, failAutosaveDelete] = GetParam();
    RenderProgressUI ui(true);
    auto& io = ProjectFileIO::Get(project());
    ASSERT_EQ(sqlite3_exec(io.GetConnection().DB(), "DELETE FROM project; DELETE FROM autosave;", nullptr, nullptr, nullptr), SQLITE_OK);
    io.MarkTemporary();
    accessor->clearSavedState();
    auto source = wave();
    auto midi = MidiTrack::Create();
    ASSERT_TRUE(midi->SetClips({ { 1, "Unsaved MIDI", 0, 4, 1, { { 1, 0, 1, 60, 95 } }, {} } }));
    TrackList::Get(project()).Add(midi);
    history->pushHistoryState("Unsaved notes and audio", "MIDI");
    ASSERT_TRUE(io.AutoSave());
    ASSERT_TRUE(io.IsTemporary());
    const auto recovery = readStoredDocument(io.GetConnection().DB(), "autosave");
    ASSERT_FALSE(recovery.isEmpty());
    const auto expectedAudio = render(*source), expectedMidi = render(*midi);
    const auto destination = directory.filePath("failed-first-save.aup4");
    QByteArray oldDatabase, oldJournal;
    if (overwrite) {
        ASSERT_NO_FATAL_FAILURE(makeJournalDestination(directory.filePath("old-first-save.sqlite"), destination));
        oldDatabase = readFile(destination);
        oldJournal = readFile(destination + "-wal");
    }
    const auto* sql = failAutosaveDelete
                      ?
                      "CREATE TRIGGER reject_final_save BEFORE DELETE ON autosave BEGIN SELECT RAISE(ABORT,'injected first save failure'); END;"
                      :
                      "CREATE TRIGGER reject_final_save BEFORE INSERT ON project BEGIN SELECT RAISE(ABORT,'injected first save failure'); END;";
    ASSERT_EQ(sqlite3_exec(io.GetConnection().DB(), sql, nullptr, nullptr, nullptr), SQLITE_OK);
    EXPECT_FALSE(accessor->save(muse::io::path_t(destination)));
    EXPECT_TRUE(io.GetLibraryError().Translation().Contains("injected first save failure"));
    ASSERT_EQ(accessor->getFileName(), path);
    EXPECT_TRUE(QFileInfo::exists(path.toQString()));
    EXPECT_EQ(QFileInfo(QString::fromUtf8(sqlite3_db_filename(io.GetConnection().DB(), "main"))).canonicalFilePath(),
              QFileInfo(path.toQString()).canonicalFilePath());
    EXPECT_TRUE(io.IsTemporary());
    EXPECT_TRUE(io.IsModified());
    EXPECT_TRUE(accessor->hasUnsavedChanges());
    EXPECT_TRUE(readStoredDocument(io.GetConnection().DB(), "project").isEmpty());
    EXPECT_EQ(readStoredDocument(io.GetConnection().DB(), "autosave"), recovery);
    EXPECT_EQ(render(*source), expectedAudio);
    EXPECT_EQ(render(*midi), expectedMidi);
    if (overwrite) {
        EXPECT_EQ(readFile(destination), oldDatabase);
        EXPECT_EQ(readFile(destination + "-wal"), oldJournal);
    } else {
        EXPECT_FALSE(QFileInfo::exists(destination));
        EXPECT_FALSE(QFileInfo::exists(destination + "-wal"));
    }
    ASSERT_EQ(sqlite3_exec(io.GetConnection().DB(), "DROP TRIGGER reject_final_save;", nullptr, nullptr, nullptr), SQLITE_OK);
    const auto retry = muse::io::path_t(directory.filePath("successful-first-save.aup4"));
    ASSERT_TRUE(accessor->save(retry));
    EXPECT_EQ(accessor->getFileName(), retry);
    EXPECT_FALSE(io.IsTemporary());
    EXPECT_FALSE(accessor->hasUnsavedChanges());
    EXPECT_FALSE(QFileInfo::exists(path.toQString())) << "First Save must retain its rename optimization";
    source.reset();
    midi.reset();
    closeProject();
    accessor = std::make_shared<au3::Au3ProjectAccessor>(muse::modularity::globalCtx());
    ASSERT_TRUE(accessor->load(retry, false));
    const auto waves = TrackList::Get(project()).Any<WaveTrack>();
    const auto instruments = TrackList::Get(project()).Any<MidiTrack>();
    ASSERT_FALSE(waves.empty());
    ASSERT_FALSE(instruments.empty());
    EXPECT_EQ(render(**waves.begin()), expectedAudio);
    EXPECT_EQ(render(**instruments.begin()), expectedMidi);
}

class NativeSaveStatementTests : public NativeEditModelTests, public testing::WithParamInterface<bool>
{
};

TEST_P(NativeSaveStatementTests, FailedSaveDoesNotCommitTheDocumentOrJournalBeforeDeletingRecoveryData)
{
    RenderProgressUI ui(true);
    auto source = wave();
    auto midi = MidiTrack::Create();
    ASSERT_TRUE(midi->SetClips({ { 1, "Saved MIDI", 0, 4, 1, { { 1, 0, 1, 60, 95 } }, {} } }));
    TrackList::Get(project()).Add(midi);
    history->pushHistoryState("Saved document", "Saved");
    ASSERT_TRUE(accessor->save(path));
    auto& io = ProjectFileIO::Get(project());
    auto* database = io.GetConnection().DB();
    const auto saved = readStoredDocument(database, "project");
    auto clips = midi->GetClips();
    clips.front().notes.push_back({ 2, 1.5, 0.5, 67, 110 });
    ASSERT_TRUE(midi->SetClips(std::move(clips)));
    source->SetVolume(0.5);
    history->pushHistoryState("Unsaved notes and audio", "MIDI");
    ASSERT_TRUE(io.AutoSave());
    const auto recovery = readStoredDocument(database, "autosave");
    const auto expectedAudio = render(*source), expectedMidi = render(*midi);
    int journalBefore = 0;
    const auto journalCount = [&] {
        int count = 0;
        EXPECT_EQ(sqlite3_exec(database, "SELECT COUNT(*) FROM project_history;", [](void* out, int, char** values, char**) {
            *static_cast<int*>(out) = std::stoi(values[0]);
            return 0;
        }, &count, nullptr), SQLITE_OK);
        return count;
    };
    journalBefore = journalCount();
    const auto* sql = GetParam()
                      ?
                      "CREATE TRIGGER reject_final_save BEFORE DELETE ON autosave BEGIN SELECT RAISE(ABORT,'injected ordinary save failure'); END;"
                      :
                      "CREATE TRIGGER reject_final_save BEFORE INSERT ON project BEGIN SELECT RAISE(ABORT,'injected ordinary save failure'); END;";
    ASSERT_EQ(sqlite3_exec(database, sql, nullptr, nullptr, nullptr), SQLITE_OK);
    EXPECT_FALSE(accessor->save(path));
    EXPECT_TRUE(io.GetLibraryError().Translation().Contains("injected ordinary save failure"));
    EXPECT_EQ(accessor->getFileName(), path);
    ASSERT_EQ(io.GetConnection().DB(), database);
    EXPECT_EQ(readStoredDocument(database, "project"), saved);
    EXPECT_EQ(readStoredDocument(database, "autosave"), recovery);
    EXPECT_EQ(journalCount(), journalBefore);
    EXPECT_TRUE(io.IsModified());
    EXPECT_TRUE(accessor->hasUnsavedChanges());
    ASSERT_EQ(sqlite3_exec(database, "DROP TRIGGER reject_final_save;", nullptr, nullptr, nullptr), SQLITE_OK);
    ASSERT_TRUE(accessor->save(path));
    EXPECT_NE(readStoredDocument(database, "project"), saved);
    EXPECT_TRUE(readStoredDocument(database, "autosave").isEmpty());
    source.reset();
    midi.reset();
    closeProject();
    accessor = std::make_shared<au3::Au3ProjectAccessor>(muse::modularity::globalCtx());
    ASSERT_TRUE(accessor->load(path, false));
    const auto waves = TrackList::Get(project()).Any<WaveTrack>();
    const auto instruments = TrackList::Get(project()).Any<MidiTrack>();
    ASSERT_FALSE(waves.empty());
    ASSERT_FALSE(instruments.empty());
    EXPECT_EQ(render(**waves.begin()), expectedAudio);
    EXPECT_EQ(render(**instruments.begin()), expectedMidi);
}

INSTANTIATE_TEST_SUITE_P(DocumentAndAutosaveStatements, NativeSaveStatementTests, testing::Bool());

class NativePendingEffectSaveTests : public NativeEditModelTests, public testing::WithParamInterface<std::tuple<bool, bool> >
{
};

TEST_P(NativePendingEffectSaveTests, SaveAndRecoveryKeepPendingManualSettingsSeparateFromAutomation)
{
    const auto [master, recovery] = GetParam();
    RenderProgressUI ui(true);
    static MetadataEffect effect;
    RealtimeEffectState::EffectFactory::Scope factory([](const PluginID&) { return &effect; });
    auto source = wave();
    const auto state = RealtimeEffectState::make_shared("tone-controls");
    auto& effects = master ? RealtimeEffectList::Get(project()) : RealtimeEffectList::Get(*source);
    ASSERT_TRUE(effects.AddState(state));
    ASSERT_TRUE(state->Automation().SetLane({ "Gain", true, { { 0.0, 0.9, 0.0 } } }));
    history->pushHistoryState("Effect", "Effect");
    ASSERT_TRUE(accessor->save(path));
    state->GetAccess()->ModifySettings([](EffectSettings& settings) {
        settings.cast<BassTrebleSettings>()->mGain = -6.0;
        settings.cast<BassTrebleSettings>()->mBass = 15.0;
        settings.extra.SetActive(false);
        return nullptr;
    });
    EXPECT_DOUBLE_EQ(state->GetSettings().cast<BassTrebleSettings>()->mGain, 0.0);
    history->pushHistoryState("Manual settings", "Effect");
    auto destination = path;
    if (recovery) {
        ASSERT_TRUE(ProjectFileIO::Get(project()).AutoSave());
        destination = muse::io::path_t(directory.filePath("pending-recovery.aup4"));
        copyDatabaseSnapshot(ProjectFileIO::Get(project()).GetConnection().DB(), destination.toQString());
    } else {
        ASSERT_TRUE(accessor->save(path));
    }
    source.reset();
    closeProject();
    accessor = std::make_shared<au3::Au3ProjectAccessor>(muse::modularity::globalCtx());
    ASSERT_TRUE(accessor->load(destination, false));
    EXPECT_EQ(ProjectFileIO::Get(project()).IsRecovered(), recovery);
    const auto& restoredEffects = master ? RealtimeEffectList::Get(project())
                                  : RealtimeEffectList::Get(**TrackList::Get(project()).Any<WaveTrack>().begin());
    ASSERT_EQ(restoredEffects.GetStatesCount(), 1u);
    const auto restored = restoredEffects.GetStateAt(0);
    EXPECT_DOUBLE_EQ(restored->GetSettings().cast<BassTrebleSettings>()->mGain, -6.0);
    EXPECT_DOUBLE_EQ(restored->GetSettings().cast<BassTrebleSettings>()->mBass, 15.0);
    EXPECT_FALSE(restored->IsEnabled());
    ASSERT_NE(restored->Automation().Find("Gain"), nullptr);
    EXPECT_DOUBLE_EQ(restored->Automation().Find("Gain")->points.front().value, 0.9);
}

INSTANTIATE_TEST_SUITE_P(TrackAndMasterSaveAndRecovery, NativePendingEffectSaveTests,
                         testing::Combine(testing::Bool(), testing::Bool()));

TEST_F(NativeEditModelTests, SavingWithAnUnavailableEffectKeepsItsSettingsForTheNextAvailableOpen)
{
    RenderProgressUI ui(true);
    static MetadataEffect effect;
    bool available = true;
    RealtimeEffectState::EffectFactory::Scope factory([&](const PluginID&) -> const EffectInstanceFactory* {
        return available ? &effect : nullptr;
    });
    auto source = wave();
    auto state = RealtimeEffectState::make_shared("temporarily-unavailable-tone");
    ASSERT_TRUE(RealtimeEffectList::Get(*source).AddState(state));
    ASSERT_TRUE(state->Automation().SetLane({ "Gain", true, { { 0.0, 0.9, 0.0 } } }));
    state->GetAccess()->ModifySettings([](EffectSettings& settings) {
        settings.cast<BassTrebleSettings>()->mGain = -6.0;
        settings.cast<BassTrebleSettings>()->mBass = 15.0;
        return nullptr;
    });
    state->GetAccess()->Flush();
    history->pushHistoryState("Configured effect", "Effect");
    source.reset();
    state.reset();
    reopen();
    available = false;
    // First save/reopen while missing, then save and close it once more. Neither
    // document history copies nor the compacted file may replace it with defaults.
    for (int pass = 0; pass < 3; ++pass) {
        reopen();
    }
    auto* loaded = *TrackList::Get(project()).Any<WaveTrack>().begin();
    auto missing = RealtimeEffectList::Get(*loaded).GetStateAt(0);
    ASSERT_EQ(missing->GetEffect(), nullptr);
    ASSERT_NE(missing->Automation().Find("Gain"), nullptr);
    missing.reset();
    available = true;
    reopen();
    loaded = *TrackList::Get(project()).Any<WaveTrack>().begin();
    const auto restored = RealtimeEffectList::Get(*loaded).GetStateAt(0);
    ASSERT_NE(restored->GetEffect(), nullptr);
    EXPECT_DOUBLE_EQ(restored->GetSettings().cast<BassTrebleSettings>()->mGain, -6.0);
    EXPECT_DOUBLE_EQ(restored->GetSettings().cast<BassTrebleSettings>()->mBass, 15.0);
    ASSERT_NE(restored->Automation().Find("Gain"), nullptr);
    EXPECT_DOUBLE_EQ(restored->Automation().Find("Gain")->points.front().value, 0.9);
}

class NativeRecoveredSaveTests : public NativeEditModelTests, public testing::WithParamInterface<bool>
{
};

TEST_P(NativeRecoveredSaveTests, DiscardRetainsTheSavedDocumentWhileSaveAsRetainsTheRecoveredStartingSnapshot)
{
    RenderProgressUI ui(true);
    const auto prior = ProjectTimeSignature::Get(project()).GetTempo();
    const auto priorUpper = ProjectTimeSignature::Get(project()).GetUpperTimeSignature();
    const auto priorLower = ProjectTimeSignature::Get(project()).GetLowerTimeSignature();
    const auto restorePreferences = finally([&] {
        auto& signature = ProjectTimeSignature::Get(project());
        signature.SetTempo(prior);
        signature.SetUpperTimeSignature(priorUpper);
        signature.SetLowerTimeSignature(priorLower);
    });
    static MetadataEffect effect;
    RealtimeEffectState::EffectFactory::Scope factory { [](const PluginID&) { return &effect; } };
    auto& signature = ProjectTimeSignature::Get(project());
    signature.SetTempo(120);
    signature.SetUpperTimeSignature(4);
    signature.SetLowerTimeSignature(4);
    auto source = wave();
    auto midi = MidiTrack::Create();
    ASSERT_TRUE(midi->SetClips({ { 1, "Saved MIDI", 0, 4, 1, { { 1, 0, 1, 60, 95 } }, {} } }));
    TrackList::Get(project()).Add(midi);
    auto& effects = RealtimeEffectList::Get(project());
    ASSERT_TRUE(effects.AddState(RealtimeEffectState::make_shared("saved-effect")));
    history->pushHistoryState("Saved state", "Saved");
    ASSERT_TRUE(accessor->save(path));
    const auto savedAudio = render(*source), savedMidi = render(*midi);
    auto clips = midi->GetClips();
    clips.front().notes.push_back({ 2, 1.5, 0.5, 64, 100 });
    ASSERT_TRUE(midi->SetClips(std::move(clips)));
    source->SetVolume(0.5);
    signature.SetUpperTimeSignature(7);
    signature.SetLowerTimeSignature(8);
    ASSERT_TRUE(effects.AddState(RealtimeEffectState::make_shared("recovered-effect")));
    history->pushHistoryState("Recovery snapshot", "MIDI");
    // Construct a valid crash snapshot with the current master-effect list.
    SavedMasterEffectList::Get(project()).UpdateCopy();
    ASSERT_TRUE(ProjectFileIO::Get(project()).AutoSave());
    const auto recoveredAudio = render(*source), recoveredMidi = render(*midi);
    const auto recoveredPath = muse::io::path_t(directory.filePath("recovered-source.aup4"));
    ASSERT_NO_FATAL_FAILURE(copyDatabaseSnapshot(ProjectFileIO::Get(project()).GetConnection().DB(), recoveredPath.toQString()));
    source.reset();
    midi.reset();
    closeProject();
    accessor = std::make_shared<au3::Au3ProjectAccessor>(muse::modularity::globalCtx());
    ASSERT_TRUE(accessor->load(recoveredPath, false));
    ASSERT_TRUE(accessor->isRecovered());
    ASSERT_TRUE(accessor->hasUnsavedChanges());
    ASSERT_FALSE(accessor->isTemporary());
    const auto loadedWaves = TrackList::Get(project()).Any<WaveTrack>();
    const auto loadedMidi = TrackList::Get(project()).Any<MidiTrack>();
    ASSERT_FALSE(loadedWaves.empty());
    ASSERT_FALSE(loadedMidi.empty());
    source = std::static_pointer_cast<WaveTrack>((*loadedWaves.begin())->shared_from_this());
    midi = std::static_pointer_cast<MidiTrack>((*loadedMidi.begin())->shared_from_this());
    EXPECT_EQ(render(*source), recoveredAudio);
    EXPECT_EQ(render(*midi), recoveredMidi);
    ASSERT_EQ(RealtimeEffectList::Get(project()).GetStatesCount(), 2u);
    clips = midi->GetClips();
    clips.front().notes.push_back({ 3, 2.5, 0.5, 67, 110 });
    ASSERT_TRUE(midi->SetClips(std::move(clips)));
    source->SetVolume(0.25);
    ProjectTimeSignature::Get(project()).SetUpperTimeSignature(3);
    ProjectTimeSignature::Get(project()).SetLowerTimeSignature(4);
    ASSERT_TRUE(RealtimeEffectList::Get(project()).AddState(RealtimeEffectState::make_shared("session-effect")));
    history->pushHistoryState("Edits after recovery", "MIDI");
    const auto currentAudio = render(*source), currentMidi = render(*midi);
    const auto destination = muse::io::path_t(directory.filePath("saved-after-recovery.aup4"));
    if (GetParam()) {
        ASSERT_TRUE(accessor->save(destination));
        EXPECT_FALSE(accessor->isRecovered());
        EXPECT_FALSE(accessor->hasUnsavedChanges());
        EXPECT_EQ(accessor->getFileName(), destination);
    }
    source.reset();
    midi.reset();
    closeProject();
    accessor = std::make_shared<au3::Au3ProjectAccessor>(muse::modularity::globalCtx());
    ASSERT_TRUE(accessor->load(recoveredPath, false));
    EXPECT_FALSE(accessor->isRecovered());
    EXPECT_FALSE(accessor->hasAutosaveData());
    EXPECT_EQ(RealtimeEffectList::Get(project()).GetStatesCount(), GetParam() ? 2u : 1u);
    EXPECT_EQ(ProjectTimeSignature::Get(project()).GetUpperTimeSignature(), GetParam() ? 7 : 4);
    EXPECT_EQ(ProjectTimeSignature::Get(project()).GetLowerTimeSignature(), GetParam() ? 8 : 4);
    EXPECT_EQ(render(**TrackList::Get(project()).Any<WaveTrack>().begin()), GetParam() ? recoveredAudio : savedAudio);
    EXPECT_EQ(render(**TrackList::Get(project()).Any<MidiTrack>().begin()), GetParam() ? recoveredMidi : savedMidi);
    if (GetParam()) {
        closeProject();
        accessor = std::make_shared<au3::Au3ProjectAccessor>(muse::modularity::globalCtx());
        ASSERT_TRUE(accessor->load(destination, false));
        EXPECT_FALSE(accessor->isRecovered());
        EXPECT_EQ(RealtimeEffectList::Get(project()).GetStatesCount(), 3u);
        EXPECT_EQ(ProjectTimeSignature::Get(project()).GetUpperTimeSignature(), 3);
        EXPECT_EQ(ProjectTimeSignature::Get(project()).GetLowerTimeSignature(), 4);
        EXPECT_EQ(render(**TrackList::Get(project()).Any<WaveTrack>().begin()), currentAudio);
        EXPECT_EQ(render(**TrackList::Get(project()).Any<MidiTrack>().begin()), currentMidi);
    }
}

INSTANTIATE_TEST_SUITE_P(DiscardOrSaveAs, NativeRecoveredSaveTests, testing::Bool());

class NativeDiscardSavedDocumentTests : public NativeEditModelTests, public testing::WithParamInterface<bool>
{
};

TEST_P(NativeDiscardSavedDocumentTests, DiscardOnCloseRestoresSavedProjectAttributesAndTrackAndMasterEffects)
{
    RenderProgressUI ui(true);
    const auto priorTempo = ProjectTimeSignature::Get(project()).GetTempo();
    const auto priorMeter = ProjectTimeSignature::Get(project()).GetUpperTimeSignature();
    const auto restorePreferences = finally([&] {
        ProjectTimeSignature::Get(project()).SetTempo(priorTempo);
        ProjectTimeSignature::Get(project()).SetUpperTimeSignature(priorMeter);
    });
    static MetadataEffect effect;
    RealtimeEffectState::EffectFactory::Scope factory { [](const PluginID&) { return &effect; } };
    ProjectTimeSignature::Get(project()).SetTempo(120.0);
    ProjectTimeSignature::Get(project()).SetUpperTimeSignature(4);
    auto source = wave();
    ASSERT_TRUE(RealtimeEffectList::Get(*source).AddState(RealtimeEffectState::make_shared("saved-track")));
    auto& master = RealtimeEffectList::Get(project());
    ASSERT_TRUE(master.AddState(RealtimeEffectState::make_shared("saved-master")));
    history->pushHistoryState("Saved state", "Saved");
    ASSERT_TRUE(accessor->save(path));
    const auto expected = samples;
    const auto expectedVolume = static_cast<double>(static_cast<float>(source->GetVolume()));
    // The shared empty fixture contains retired blocks. Remove those first so
    // the parameter controls whether close actually needs to compact.
    auto& io = ProjectFileIO::Get(project());
    io.Compact({ &TrackList::Get(project()) }, true);
    ASSERT_EQ(io.GetTotalUsage(), io.GetCurrentUsage({ &TrackList::Get(project()) }));

    ProjectTimeSignature::Get(project()).SetTempo(150.0);
    ProjectTimeSignature::Get(project()).SetUpperTimeSignature(7);
    master.GetStateAt(0)->SetActive(false);
    ASSERT_TRUE(master.AddState(RealtimeEffectState::make_shared("unsaved-master")));
    RealtimeEffectList::Get(*source).GetStateAt(0)->SetActive(false);
    source->SetVolume(0.3);
    if (GetParam()) {
        auto extra = WaveTrackFactory::Get(project()).Create(1, floatSample, 48000.0);
        auto clip = extra->CreateClip(0.0, "Unsaved audio");
        std::vector<float> data(1024 * 1024, 0.25f);
        constSamplePtr buffers[] { reinterpret_cast<constSamplePtr>(data.data()) };
        clip->Append(buffers, floatSample, data.size(), 1, floatSample);
        clip->Flush();
        extra->InsertInterval(clip, true);
        TrackList::Get(project()).Add(extra);
    }
    history->pushHistoryState("Unsaved edits", "Edit");
    ASSERT_TRUE(accessor->hasUnsavedChanges());
    source.reset();
    // Do not clearSavedState(): exercise the actual close/compaction policy.
    closeProject();
    EXPECT_EQ(io.WasCompacted(), GetParam());
    accessor = std::make_shared<au3::Au3ProjectAccessor>(muse::modularity::globalCtx());
    ASSERT_TRUE(accessor->load(path, false));
    EXPECT_DOUBLE_EQ(ProjectTimeSignature::Get(project()).GetTempo(), 120.0);
    EXPECT_EQ(ProjectTimeSignature::Get(project()).GetUpperTimeSignature(), 4);
    const auto tracks = TrackList::Get(project()).Any<WaveTrack>();
    ASSERT_EQ(tracks.size(), 1u);
    const auto* restored = *tracks.begin();
    EXPECT_DOUBLE_EQ(restored->GetVolume(), expectedVolume);
    const auto& trackEffects = RealtimeEffectList::Get(*restored);
    ASSERT_EQ(trackEffects.GetStatesCount(), 1u);
    EXPECT_TRUE(trackEffects.GetStateAt(0)->IsEnabled());
    const auto& masterEffects = RealtimeEffectList::Get(project());
    ASSERT_EQ(masterEffects.GetStatesCount(), 1u);
    EXPECT_TRUE(masterEffects.GetStateAt(0)->IsEnabled());
    std::vector<float> actual(expected.size());
    ASSERT_TRUE(restored->GetChannel(0)->GetFloats(actual.data(), 0, actual.size()));
    EXPECT_EQ(actual, expected);
    EXPECT_TRUE(ui.errors.empty());
}

INSTANTIATE_TEST_SUITE_P(CompactedAndUncompacted, NativeDiscardSavedDocumentTests, testing::Bool());

TEST_P(NativeSaveDestinationTests, SaveCopyRetainsSourceConnectionAndSavePointAndCopiesFrozenMidiRoutingAndMasterEffects)
{
    RenderProgressUI ui(true);
    auto original = wave();
    history->pushHistoryState("Audio", "Audio");
    trackedit::TrackFreezeModel freeze(ctx);
    freeze.init();
    freeze.setTrackId(static_cast<int64_t>(original->GetId()));
    freeze.freeze(0.0);
    finish(freeze);
    original.reset();
    auto* frozen = *TrackList::Get(project()).Any<WaveTrack>().begin();
    const auto frozenXml = TrackFreeze::Get(*frozen).sourceXml;
    ASSERT_FALSE(frozenXml.empty());
    std::vector<float> frozenSamples(samples.size());
    ASSERT_TRUE(frozen->GetChannel(0)->GetFloats(frozenSamples.data(), 0, frozenSamples.size()));

    auto midi = MidiTrack::Create();
    midi->SetInstrument({}, "{\"0\":0.25}");
    ASSERT_TRUE(midi->SetClips({ { 1, "Phrase", 0.0, 4.0, 1, { { 1, 0.5, 1.0, 67, 92 } }, {} } }));
    ASSERT_TRUE(TrackAutomation::Get(*midi).SetLane({ "instrument/0", true, { { 0.0, 0.6, 0.0 } } }));
    TrackList::Get(project()).Add(midi);
    auto bus = WaveTrackFactory::Get(project()).Create(1, floatSample, 48000.0);
    TrackRouting::Get(*bus).busId = "copy-bus";
    TrackList::Get(project()).Add(bus);
    TrackRouting::Get(*midi).output = "copy-bus";
    TrackRouting::Get(*frozen).output = "copy-bus";
    history->pushHistoryState("MIDI and bus", "MIDI");

    auto& io = ProjectFileIO::Get(project());
    if (GetParam()) {
        io.MarkTemporary();
    }
    const auto sourcePath = accessor->getFileName();
    const auto temporary = io.IsTemporary();
    auto* connection = io.GetConnection().DB();
    const auto undoState = UndoManager::Get(project()).GetCurrentState();
    ASSERT_TRUE(accessor->hasUnsavedChanges());
    auto& savedMaster = SavedMasterEffectList::Get(project());
    const auto* savedSnapshot = &savedMaster.List();
    ASSERT_EQ(savedSnapshot->GetStatesCount(), 0u);
    static MetadataEffect effect;
    RealtimeEffectState::EffectFactory::Scope factory { [](const PluginID&) { return &effect; } };
    ASSERT_TRUE(RealtimeEffectList::Get(project()).AddState(RealtimeEffectState::make_shared("copy-master")));

    ASSERT_TRUE(accessor->saveCopy(muse::io::path_t(directory.filePath("fresh-copy.aup4"))));
    const auto copyPath = directory.filePath("overwrite-copy.aup4");
    ASSERT_NO_FATAL_FAILURE(makeJournalDestination(directory.filePath("copy-writer.sqlite"), copyPath));
    ASSERT_TRUE(accessor->saveCopy(muse::io::path_t(copyPath)));
    EXPECT_TRUE(ui.errors.empty());
    EXPECT_EQ(accessor->getFileName(), sourcePath);
    EXPECT_EQ(io.GetConnection().DB(), connection);
    EXPECT_EQ(io.IsTemporary(), temporary);
    EXPECT_TRUE(accessor->hasUnsavedChanges());
    EXPECT_EQ(UndoManager::Get(project()).GetCurrentState(), undoState);
    EXPECT_EQ(&savedMaster.List(), savedSnapshot);
    EXPECT_EQ(savedMaster.List().GetStatesCount(), 0u);
    EXPECT_EQ(RealtimeEffectList::Get(project()).GetStatesCount(), 1u);
    EXPECT_TRUE(QDir(directory.path()).entryList({ "*.bak*" }, QDir::Files).empty());

    // No copied block or frozen source may depend on the source database.
    midi.reset();
    bus.reset();
    closeProject();
    accessor = std::make_shared<au3::Au3ProjectAccessor>(muse::modularity::globalCtx());
    ASSERT_TRUE(accessor->load(muse::io::path_t(copyPath), false));
    ASSERT_EQ(TrackList::Get(project()).Any<MidiTrack>().size(), 1u);
    const auto* restored = *TrackList::Get(project()).Any<MidiTrack>().begin();
    EXPECT_EQ(restored->GetInstrumentState(), "{\"0\":0.25}");
    ASSERT_EQ(restored->GetClips().size(), 1u);
    ASSERT_EQ(restored->GetClips()[0].notes.size(), 1u);
    EXPECT_EQ(restored->GetClips()[0].notes[0].pitch, 67);
    EXPECT_EQ(restored->GetClips()[0].notes[0].velocity, 92);
    ASSERT_NE(TrackAutomation::Get(*restored).Find("instrument/0"), nullptr);
    EXPECT_DOUBLE_EQ(TrackAutomation::Get(*restored).Find("instrument/0")->points[0].value, 0.6);
    EXPECT_EQ(TrackRouting::Get(*restored).output, "copy-bus");
    EXPECT_TRUE(TrackRouting::Validate(TrackList::Get(project())));
    EXPECT_EQ(RealtimeEffectList::Get(project()).GetStatesCount(), 1u);
    auto* restoredFrozen = *TrackList::Get(project()).Any<WaveTrack>().begin();
    EXPECT_EQ(TrackFreeze::Get(*restoredFrozen).sourceXml, frozenXml);
    ASSERT_FALSE(TrackFreeze::Get(*restoredFrozen).blocks.empty());
    std::vector<float> actual(samples.size());
    ASSERT_TRUE(restoredFrozen->GetChannel(0)->GetFloats(actual.data(), 0, actual.size()));
    EXPECT_EQ(actual, frozenSamples);
    freeze.setTrackId(static_cast<int64_t>(restoredFrozen->GetId()));
    freeze.unfreeze();
    ASSERT_FALSE(freeze.frozen());
    auto* unfrozen = *TrackList::Get(project()).Any<WaveTrack>().begin();
    ASSERT_TRUE(unfrozen->GetChannel(0)->GetFloats(actual.data(), 0, actual.size()));
    EXPECT_EQ(actual, samples);
}

TEST_P(NativeSaveDestinationTests, FailedSaveCopyRestoresOldDestinationAndKeepsSourceDirtyAndConnected)
{
    RenderProgressUI ui(true);
    const auto track = wave();
    history->pushHistoryState("Audio", "Audio");
    auto& io = ProjectFileIO::Get(project());
    if (GetParam()) {
        io.MarkTemporary();
    }
    const auto temporary = io.IsTemporary();
    auto* connection = io.GetConnection().DB();
    const auto destination = directory.filePath("copy.aup4");
    ASSERT_NO_FATAL_FAILURE(makeJournalDestination(directory.filePath("writer.sqlite"), destination));
    const auto oldDatabase = readFile(destination);
    const auto oldJournal = readFile(destination + "-wal");
    ASSERT_FALSE(oldDatabase.isEmpty());
    ASSERT_FALSE(oldJournal.isEmpty());
    ASSERT_EQ(sqlite3_set_authorizer(connection, [](void*, int action, const char*, const char*, const char* schema, const char*) {
        return action == SQLITE_CREATE_TABLE && schema && std::string_view(schema) == "outbound" ? SQLITE_DENY : SQLITE_OK;
    }, nullptr), SQLITE_OK);
    const auto clearAuthorizer = finally([&] { sqlite3_set_authorizer(connection, nullptr, nullptr); });
    EXPECT_FALSE(accessor->saveCopy(muse::io::path_t(destination)));
    EXPECT_EQ(readFile(destination), oldDatabase);
    EXPECT_EQ(readFile(destination + "-wal"), oldJournal);
    EXPECT_EQ(accessor->getFileName(), path);
    EXPECT_EQ(io.GetConnection().DB(), connection);
    EXPECT_EQ(io.IsTemporary(), temporary);
    EXPECT_TRUE(accessor->hasUnsavedChanges());
    ASSERT_EQ(sqlite3_set_authorizer(connection, nullptr, nullptr), SQLITE_OK);
    EXPECT_TRUE(accessor->saveCopy(muse::io::path_t(directory.filePath("retry-copy.aup4"))));
    EXPECT_FALSE(accessor->saveCopy(path));
    EXPECT_FALSE(accessor->saveCopy(muse::io::path_t(directory.filePath("./freeze.aup4"))));
    EXPECT_FALSE(accessor->saveCopy({}));
    EXPECT_TRUE(QDir(directory.path()).entryList({ "*.bak*" }, QDir::Files).empty());
}

TEST_P(NativeSaveDestinationTests, OverwritingAnInterruptedDestinationDoesNotReplayItsOldJournalIntoMidiOrAudio)
{
    RenderProgressUI ui(true);
    auto audioTrack = wave();
    const auto expectedAudio = render(*audioTrack);
    auto midi = MidiTrack::Create();
    midi->SetInstrument({}, "{\"0\":0.25}");
    ASSERT_TRUE(midi->SetClips({ { 1, "Phrase", 0.0, 4.0, 1, { { 1, 0.5, 1.0, 67, 92 } }, {} } }));
    ASSERT_TRUE(TrackAutomation::Get(*midi).SetLane({ "instrument/0", true, { { 0.0, 0.6, 0.0 } } }));
    TrackList::Get(project()).Add(midi);
    history->pushHistoryState("Source", "Source");
    // New projects use SaveProject's rename path; existing projects use CopyTo.
    // Both must move the previous destination's journal out of the way first.
    if (GetParam()) {
        ProjectFileIO::Get(project()).MarkTemporary();
    }
    const auto destination = directory.filePath("overwrite.aup4");
    ASSERT_NO_FATAL_FAILURE(makeJournalDestination(directory.filePath("writer.sqlite"), destination));
    ASSERT_TRUE(accessor->save(muse::io::path_t(destination)));
    EXPECT_TRUE(ui.errors.empty());
    audioTrack.reset();
    midi.reset();
    closeProject();
    accessor = std::make_shared<au3::Au3ProjectAccessor>(muse::modularity::globalCtx());
    ASSERT_TRUE(accessor->load(muse::io::path_t(destination), false)) << testing::PrintToString(ui.errors);
    const auto waves = TrackList::Get(project()).Any<WaveTrack>();
    ASSERT_EQ(waves.size(), 1u);
    EXPECT_EQ(render(**waves.begin()), expectedAudio);
    const auto tracks = TrackList::Get(project()).Any<MidiTrack>();
    ASSERT_EQ(tracks.size(), 1u);
    const auto* restored = *tracks.begin();
    EXPECT_EQ(restored->GetInstrumentState(), "{\"0\":0.25}");
    ASSERT_EQ(restored->GetClips().size(), 1u);
    ASSERT_EQ(restored->GetClips()[0].notes.size(), 1u);
    EXPECT_EQ(restored->GetClips()[0].notes[0].pitch, 67);
    EXPECT_EQ(restored->GetClips()[0].notes[0].velocity, 92);
    ASSERT_NE(TrackAutomation::Get(*restored).Find("instrument/0"), nullptr);
    EXPECT_DOUBLE_EQ(TrackAutomation::Get(*restored).Find("instrument/0")->points[0].value, 0.6);
    EXPECT_TRUE(QDir(directory.path()).entryList({ "*.bak*" }, QDir::Files).empty());
}

INSTANTIATE_TEST_SUITE_P(ExistingAndTemporarySources, NativeSaveDestinationTests, testing::Bool());

TEST_F(NativeEditModelTests, FailedOverwriteRestoresTheOldDatabaseAndJournalAndKeepsTheSourceOpen)
{
    RenderProgressUI ui(true);
    const auto source = wave();
    const auto expectedAudio = render(*source);
    history->pushHistoryState("Source", "Source");
    const auto destination = directory.filePath("overwrite.aup4");
    ASSERT_NO_FATAL_FAILURE(makeJournalDestination(directory.filePath("writer.sqlite"), destination));
    const auto originalDatabase = readFile(destination);
    const auto originalJournal = readFile(destination + "-wal");
    ASSERT_FALSE(originalDatabase.isEmpty());
    ASSERT_FALSE(originalJournal.isEmpty());
    auto& io = ProjectFileIO::Get(project());
    auto* database = io.GetConnection().DB();
    ASSERT_EQ(sqlite3_set_authorizer(database, [](void*, int action, const char*, const char*, const char* schema, const char*) {
        return action == SQLITE_CREATE_TABLE && schema && std::string_view(schema) == "outbound" ? SQLITE_DENY : SQLITE_OK;
    }, nullptr), SQLITE_OK);
    bool authorizerInstalled = true;
    const auto restoreAuthorizer = finally([&] {
        if (authorizerInstalled) {
            sqlite3_set_authorizer(database, nullptr, nullptr);
        }
    });
    EXPECT_FALSE(accessor->save(muse::io::path_t(destination)));
    EXPECT_FALSE(ui.errors.empty());
    EXPECT_EQ(readFile(destination), originalDatabase);
    EXPECT_EQ(readFile(destination + "-wal"), originalJournal);
    EXPECT_EQ(accessor->getFileName(), path);
    EXPECT_EQ(io.GetConnection().DB(), database);
    EXPECT_EQ(render(*source), expectedAudio);
    EXPECT_TRUE(QDir(directory.path()).entryList({ "*.bak*" }, QDir::Files).empty());
    ASSERT_EQ(sqlite3_set_authorizer(database, nullptr, nullptr), SQLITE_OK);
    authorizerInstalled = false;
    EXPECT_TRUE(accessor->save(muse::io::path_t(directory.filePath("retry.aup4"))));
    EXPECT_EQ(readFile(destination), originalDatabase);
    EXPECT_EQ(readFile(destination + "-wal"), originalJournal);
}

TEST_F(NativeEditModelTests, SavingThroughAnAliasOfTheActiveFileDoesNotMoveOrReplaceItsConnection)
{
    RenderProgressUI ui(true);
    const auto source = wave();
    const auto expectedAudio = render(*source);
    history->pushHistoryState("Source", "Source");
    ASSERT_TRUE(accessor->save(path));
    ASSERT_TRUE(QDir().mkpath(directory.filePath("alias")));
    const auto alternate = directory.filePath("alias/../freeze.aup4");
    auto* connection = ProjectFileIO::Get(project()).GetConnection().DB();
    ASSERT_TRUE(accessor->save(muse::io::path_t(alternate)));
    EXPECT_EQ(accessor->getFileName(), path);
    EXPECT_EQ(ProjectFileIO::Get(project()).GetConnection().DB(), connection);
    EXPECT_EQ(render(*source), expectedAudio);
    EXPECT_TRUE(ui.errors.empty());
    EXPECT_TRUE(QDir(directory.path()).entryList({ "*.bak*" }, QDir::Files).empty());
}

namespace {
void pumpModelEvents(int milliseconds)
{
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < milliseconds) {
        QCoreApplication::processEvents();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}
}

TEST_F(NativeEditModelTests, ContinuousInstrumentEditsStayLiveWithoutTakingPresetSnapshotsUntilIdle)
{
    auto track = MidiTrack::Create();
    TrackList::Get(project()).Add(track);
    history->pushHistoryState("Instrument", "Instrument");
    trackedit::PianoRollModel model(ctx);
    model.init();
    model.setTrackId(QVariant::fromValue<qlonglong>(static_cast<int64_t>(track->GetId())));
    ASSERT_TRUE(model.enableInput());
    const auto editor = inputHost->editor;
    editor->snapshots = 0;
    const auto index = history->currentStateIndex();
    for (int i = 1; i <= 6; ++i) {
        editor->preset = "Live knob " + std::to_string(i);
        editor->gain = i / 10.0;
        editor->changed = true;
        pumpModelEvents(25);
        EXPECT_EQ(editor->snapshots, 0);
        EXPECT_EQ(history->currentStateIndex(), index);
        EXPECT_NEAR(model.parameters().front().toMap()["value"].toDouble(), i / 10.0, 1e-6);
    }
    EXPECT_TRUE(track->GetInstrumentState().empty());
    pumpModelEvents(400);
    EXPECT_EQ(editor->snapshots, 1);
    EXPECT_EQ(track->GetInstrumentState().utf8_string(), "Live knob 6");
    EXPECT_EQ(history->currentStateIndex(), index + 1);
    EXPECT_TRUE(editor->opened);
}

TEST_F(NativeEditModelTests, PendingInstrumentChangesFlushWithoutClosingTheEditorAndAreSaved)
{
    auto track = MidiTrack::Create();
    TrackList::Get(project()).Add(track);
    history->pushHistoryState("Instrument", "Instrument");
    trackedit::PianoRollModel model(ctx);
    model.init();
    model.setTrackId(QVariant::fromValue<qlonglong>(static_cast<int64_t>(track->GetId())));
    ASSERT_TRUE(model.enableInput());
    const auto editor = inputHost->editor;
    editor->snapshots = 0;
    editor->preset = "Save before idle";
    editor->changed = true;
    requests->requestFlushInstrumentChanges();
    EXPECT_EQ(editor->snapshots, 1);
    EXPECT_EQ(track->GetInstrumentState().utf8_string(), editor->preset);
    EXPECT_TRUE(editor->opened);
    EXPECT_EQ(model.instrumentEditor(), editor);
    ASSERT_TRUE(accessor->save(path));
    model.disableInput();
    reopen();
    ASSERT_FALSE(TrackList::Get(project()).Any<MidiTrack>().empty());
    EXPECT_EQ((*TrackList::Get(project()).Any<MidiTrack>().begin())->GetInstrumentState().utf8_string(), "Save before idle");
    pumpModelEvents(350);
    EXPECT_EQ(editor->snapshots, 1);
}

TEST_F(NativeEditModelTests, UndoBeforeInstrumentIdleFlushesAndUndoesTheKnobGesture)
{
    auto track = MidiTrack::Create();
    TrackList::Get(project()).Add(track);
    history->pushHistoryState("Instrument", "Instrument");
    trackedit::PianoRollModel model(ctx);
    model.init();
    model.setTrackId(QVariant::fromValue<qlonglong>(static_cast<int64_t>(track->GetId())));
    ASSERT_TRUE(model.enableInput());
    const auto editor = inputHost->editor;
    editor->snapshots = 0;
    editor->preset = "Undo before idle";
    editor->changed = true;
    history->undo();
    ASSERT_FALSE(TrackList::Get(project()).Any<MidiTrack>().empty());
    EXPECT_TRUE((*TrackList::Get(project()).Any<MidiTrack>().begin())->GetInstrumentState().empty());
    EXPECT_EQ(editor->snapshots, 1);
    history->redo();
    EXPECT_EQ((*TrackList::Get(project()).Any<MidiTrack>().begin())->GetInstrumentState().utf8_string(), "Undo before idle");
    pumpModelEvents(350);
    EXPECT_EQ(editor->snapshots, 1);
}

TEST_F(NativeEditModelTests, ControllerOnlyInputDoesNotRebuildKeyboardHighlights)
{
    auto track = MidiTrack::Create();
    TrackList::Get(project()).Add(track);
    history->pushHistoryState("Instrument", "Instrument");
    trackedit::PianoRollModel model(ctx);
    model.init();
    model.setTrackId(QVariant::fromValue<qlonglong>(static_cast<int64_t>(track->GetId())));
    ASSERT_TRUE(model.enableInput());
    int highlights = 0;
    QObject::connect(&model, &trackedit::PianoRollModel::heldNotesChanged, &model, [&] { ++highlights; });
    const auto editor = inputHost->editor;
    editor->received = { { 0xb0, 74, 25, 0 }, { 0xb0, 74, 50, 0 }, { 0xb0, 17, 75, 0 } };
    pumpModelEvents(25);
    EXPECT_EQ(highlights, 0);
    editor->received = { { 0x90, 60, 100, 0 } };
    pumpModelEvents(25);
    EXPECT_EQ(highlights, 1);
    ASSERT_EQ(model.heldNotes().size(), 1);
    editor->received = { { 0xb0, 64, 127, 0 }, { 0xb0, 74, 90, 0 } };
    pumpModelEvents(25);
    EXPECT_EQ(highlights, 1);
    editor->received = { { 0xb0, 123, 0, 0 } };
    pumpModelEvents(25);
    EXPECT_EQ(highlights, 2);
    EXPECT_TRUE(model.heldNotes().empty());
}

TEST_F(NativeEditModelTests, LiveEditorReplacementNotifiesTheBoundParameterPane)
{
    auto track = MidiTrack::Create();
    TrackList::Get(project()).Add(track);
    QVariantList boundParameters;
    trackedit::PianoRollModel model(ctx);
    model.init();
    model.setTrackId(QVariant::fromValue<qlonglong>(static_cast<int64_t>(track->GetId())));
    boundParameters = model.parameters();
    ASSERT_TRUE(boundParameters.empty());
    QObject::connect(&model, &trackedit::PianoRollModel::parametersChanged, &model, [&] { boundParameters = model.parameters(); });
    ASSERT_TRUE(model.enableInput());
    ASSERT_EQ(boundParameters.size(), 1);
    EXPECT_EQ(boundParameters.front().toMap().value("id").toString(), "gain");
    model.loadParameters();
    EXPECT_EQ(model.parameters(), boundParameters);
    model.disableInput();
    EXPECT_TRUE(boundParameters.empty());
    ASSERT_TRUE(model.enableInput());
    ASSERT_EQ(boundParameters.size(), 1);
}

TEST_F(NativeEditModelTests, UndoWithUnchangedPresetClosesTheSessionBoundToTheOldTrackSnapshot)
{
    auto original = MidiTrack::Create();
    TrackList::Get(project()).Add(original);
    history->pushHistoryState("Instrument", "Instrument");
    trackedit::PianoRollModel model(ctx);
    model.init();
    model.setTrackId(QVariant::fromValue<qlonglong>(static_cast<int64_t>(original->GetId())));
    const auto clip = model.addClip(0.0);
    ASSERT_GT(clip, 0);
    model.addNote(0.0, 1.0, 60, 100);
    ASSERT_TRUE(model.enableInput());
    const auto oldEditor = inputHost->editor;
    ASSERT_EQ(inputHost->boundTrack, original.get());
    model.undo();
    auto* restored = *TrackList::Get(project()).Any<MidiTrack>().begin();
    ASSERT_NE(restored, original.get());
    EXPECT_EQ(restored->GetInstrumentState(), original->GetInstrumentState());
    EXPECT_EQ(oldEditor->closed, 1);
    EXPECT_EQ(model.instrumentEditor(), nullptr);
    ASSERT_TRUE(model.enableInput());
    EXPECT_EQ(inputHost->boundTrack, restored);
    EXPECT_NE(inputHost->editor, oldEditor);
    ASSERT_TRUE(model.notes().empty());
    model.redo();
    ASSERT_EQ(model.notes().size(), 1);
    EXPECT_EQ(inputHost->editor->closed, 1);
}

TEST_F(NativeEditModelTests, PianoRollMeterFollowsTheCurrentProjectWithoutChangingItsNotesOrKeepingOldSubscriptions)
{
    const auto track = MidiTrack::Create();
    TrackList::Get(project()).Add(track);
    muse::async::Notification projectChanged;
    muse::async::Channel<trackedit::TimeSignature> firstChanged, secondChanged;
    trackedit::TimeSignature first { 120, 7, 8 }, second { 90, 3, 2 };
    ON_CALL(*context, currentTrackeditProjectChanged()).WillByDefault(Return(projectChanged));
    ON_CALL(*trackeditProject, timeSignature()).WillByDefault([&] { return first; });
    ON_CALL(*trackeditProject, timeSignatureChanged()).WillByDefault(Return(firstChanged));
    trackedit::PianoRollModel model(ctx);
    model.init();
    model.setTrackId(QVariant::fromValue<qlonglong>(static_cast<int64_t>(track->GetId())));
    model.addClip(0, 4);
    model.addNote(0.5, 1.5, 64, 95);
    const auto notes = model.notes();
    EXPECT_EQ(model.property("upperTimeSignature").toInt(), 7);
    EXPECT_EQ(model.property("lowerTimeSignature").toInt(), 8);
    int notifications = 0;
    QObject::connect(&model, &trackedit::PianoRollModel::dataChanged, &model, [&] { ++notifications; });
    first = { 120, 6, 8 };
    firstChanged.send(first);
    EXPECT_GT(notifications, 0);
    EXPECT_EQ(model.property("upperTimeSignature").toInt(), 6);
    EXPECT_EQ(model.notes(), notes);
    auto next = std::make_shared<NiceMock<trackedit::TrackeditProjectMock> >();
    ON_CALL(*next, timeSignature()).WillByDefault([&] { return second; });
    ON_CALL(*next, timeSignatureChanged()).WillByDefault(Return(secondChanged));
    ON_CALL(*context, currentTrackeditProject()).WillByDefault(Return(next));
    projectChanged.notify();
    EXPECT_EQ(model.property("upperTimeSignature").toInt(), 3);
    EXPECT_EQ(model.property("lowerTimeSignature").toInt(), 2);
    auto count = notifications;
    firstChanged.send(first);
    EXPECT_EQ(notifications, count);
    second = { 90, 5, 16 };
    secondChanged.send(second);
    EXPECT_GT(notifications, count);
    EXPECT_EQ(model.property("upperTimeSignature").toInt(), 5);
    EXPECT_EQ(model.property("lowerTimeSignature").toInt(), 16);
    ON_CALL(*context, currentTrackeditProject()).WillByDefault(Return(nullptr));
    projectChanged.notify();
    EXPECT_EQ(model.property("upperTimeSignature").toInt(), 4);
    EXPECT_EQ(model.property("lowerTimeSignature").toInt(), 4);
    count = notifications;
    secondChanged.send(second);
    EXPECT_EQ(notifications, count);
    EXPECT_EQ(model.notes(), notes);
}

TEST_F(NativeEditModelTests, TempoEditsDuringANoteTakePreserveDriverTimingAndExistingDocumentBeats)
{
    const auto track = MidiTrack::Create();
    track->SetTempo(120);
    TrackList::Get(project()).Add(track);
    ProjectTimeSignature::Get(project()).SetTempo(120);
    history->pushHistoryState("Instrument", "Instrument");
    trackedit::TimeSignature signature { 120, 4, 4 };
    muse::async::Channel<trackedit::TimeSignature> signatureChanged;
    ON_CALL(*trackeditProject, timeSignature()).WillByDefault([&] { return signature; });
    ON_CALL(*trackeditProject, timeSignatureChanged()).WillByDefault(Return(signatureChanged));
    trackedit::PianoRollModel model(ctx);
    model.init();
    model.setTrackId(QVariant::fromValue<qlonglong>(static_cast<int64_t>(track->GetId())));
    model.addClip(0, 4);
    model.addNote(0.5, 0.5, 55, 80);
    const auto original = model.notes();
    model.startInputRecording();
    ASSERT_TRUE(model.inputRecording());
    const auto time = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
    inputHost->editor->received = {
        { 0xb0, 64, 127, time + 0.03 }, { 0x90, 60, 100, time + 0.05 },
        { 0x80, 60, 0, time + 0.25 }, { 0xb0, 64, 0, time + 0.30 }, { 0x90, 67, 95, time + 0.35 }
    };
    QElapsedTimer wait;
    wait.start();
    while (model.inputNotes().size() != 2 && wait.elapsed() < 1000) {
        QCoreApplication::processEvents();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    ASSERT_EQ(model.inputNotes().size(), 2);
    ASSERT_EQ(model.heldNotes().size(), 1);
    EXPECT_NEAR(model.inputNotes().front().toMap().value("length").toDouble(), 0.4, 1e-6);
    signature = { 150, 6, 8 };
    track->SetTempo(150);
    ProjectTimeSignature::Get(project()).SetTempo(150);
    signatureChanged.send(signature);
    history->pushHistoryState("Tempo", "Tempo");
    EXPECT_NEAR(model.inputNotes().front().toMap().value("length").toDouble(), 0.5, 1e-6);
    EXPECT_EQ(model.notes(), original);
    ASSERT_EQ(model.heldNotes().size(), 1);
    inputHost->editor->received = {
        { 0x80, 67, 0, time + 0.65 }, { 0x90, 72, 90, time + 0.7 }, { 0x80, 72, 0, time + 1.1 }
    };
    model.stopInputRecording();
    const auto saved = track->GetClips().front();
    ASSERT_EQ(saved.notes.size(), 4u);
    ASSERT_EQ(saved.controllers.size(), 2u);
    EXPECT_DOUBLE_EQ(saved.notes[0].start, 0.5);
    EXPECT_DOUBLE_EQ(saved.notes[0].length, 0.5);
    EXPECT_NEAR(saved.notes[1].length, 0.5, 1e-6);
    EXPECT_NEAR(saved.notes[2].length, 0.75, 1e-6);
    EXPECT_NEAR(saved.notes[3].length, 1.0, 1e-6);
    EXPECT_NEAR(saved.notes[2].start - saved.notes[1].start, 0.75, 1e-6);
    EXPECT_NEAR(saved.controllers[1].beat - saved.controllers[0].beat, 0.675, 1e-6);
    model.undo();
    EXPECT_EQ(model.notes(), original);
    model.redo();
    ASSERT_EQ(model.notes().size(), 4);
    const auto restored = model.notes();
    EXPECT_NEAR(restored[1].toMap().value("length").toDouble(), 0.5, 1e-6);
    EXPECT_NEAR(restored[2].toMap().value("length").toDouble(), 0.75, 1e-6);
    EXPECT_NEAR(restored[3].toMap().value("length").toDouble(), 1.0, 1e-6);
}

TEST_F(NativeEditModelTests, PianoRollHistoryNamesTheActualEditAndTrimmedNotesUseVisibleCoordinates)
{
    auto track = MidiTrack::Create();
    TrackList::Get(project()).Add(track);
    history->pushHistoryState("Instrument", "Instrument");
    trackedit::PianoRollModel model(ctx);
    model.setTrackId(QVariant::fromValue<qlonglong>(static_cast<int64_t>(track->GetId())));
    auto expectHistory = [&](const char* expected) {
        auto& undo = UndoManager::Get(project());
        TranslatableString description;
        undo.GetShortDescription(undo.GetCurrentState(), &description);
        EXPECT_EQ(description.Translation().ToStdString(), expected);
    };
    model.addClip(0, 8);
    expectHistory("Added MIDI clip");
    model.addNote(2, 1, 60, 100);
    expectHistory("Added MIDI note");
    const auto id = track->GetClips().front().notes.front().id;
    model.editNote(id, 3, 1, 60, 100);
    expectHistory("Moved MIDI note");
    model.editNote(id, 3, 2, 60, 100);
    expectHistory("Resized MIDI note");
    model.editNote(id, 3, 2, 60, 90);
    expectHistory("Changed MIDI note velocity");
    model.editNote(id, 3, 2, 62, 90);
    expectHistory("Changed MIDI note pitch");
    auto clips = track->GetClips();
    clips.front().trimLeft = 2;
    clips.front().trimRight = 1;
    ASSERT_TRUE(track->SetClips(clips));
    EXPECT_DOUBLE_EQ(model.clipLength(), 5);
    EXPECT_DOUBLE_EQ(model.notes().front().toMap()["start"].toDouble(), 1);
    model.editNote(id, 0.5, 1, 62, 90);
    expectHistory("Moved MIDI note");
    EXPECT_DOUBLE_EQ(track->GetClips().front().notes.front().start, 2.5);
    model.removeNote(id);
    expectHistory("Removed MIDI note");
    model.undo();
    ASSERT_EQ(model.notes().size(), 1);
    EXPECT_DOUBLE_EQ(model.notes().front().toMap()["start"].toDouble(), 0.5);
    model.redo();
    EXPECT_TRUE(model.notes().empty());
}

TEST_F(NativeEditModelTests, PianoRollUndoAndRedoRespectRecordingAndPlaybackOwnership)
{
    const auto track = MidiTrack::Create();
    TrackList::Get(project()).Add(track);
    history->pushHistoryState("Instrument", "Instrument");
    trackedit::PianoRollModel model(ctx);
    model.setTrackId(QVariant::fromValue<qlonglong>(static_cast<int64_t>(track->GetId())));
    model.addClip(0.0);
    model.addNote(0.0, 1.0, 60, 100);
    ON_CALL(*audio, isBusy()).WillByDefault(Return(true));
    ON_CALL(*audio, canStopAudioStream(testing::_)).WillByDefault(Return(false));
    const auto before = history->currentStateIndex();
    model.undo();
    EXPECT_EQ(history->currentStateIndex(), before);
    EXPECT_FALSE(model.error().isEmpty());
    ON_CALL(*audio, canStopAudioStream(testing::_)).WillByDefault(Return(true));
    EXPECT_CALL(*audio, stopStream()).Times(1);
    model.undo();
    EXPECT_EQ(history->currentStateIndex(), before - 1);
    ON_CALL(*context, isRecording()).WillByDefault(Return(true));
    model.redo();
    EXPECT_EQ(history->currentStateIndex(), before - 1);
    ON_CALL(*context, isRecording()).WillByDefault(Return(false));
    ON_CALL(*audio, isBusy()).WillByDefault(Return(false));
    model.redo();
    EXPECT_EQ(history->currentStateIndex(), before);
    ASSERT_EQ(model.notes().size(), 1);
}

TEST_F(NativeEditModelTests, ComputerAndMidiKeyboardTakesReachTheDocumentAndAudacityUndo)
{
    const auto track = MidiTrack::Create();
    TrackList::Get(project()).Add(track);
    history->pushHistoryState("Instrument", "Instrument");
    trackedit::PianoRollModel model(ctx);
    model.init();
    model.setTrackId(QVariant::fromValue<qlonglong>(static_cast<int64_t>(track->GetId())));
    model.addClip(0.0, 1.0);
    model.startInputRecording();
    ASSERT_TRUE(model.inputRecording());
    model.noteOn(60, 97);
    model.noteOff(60);
    model.stopInputRecording();
    ASSERT_EQ(model.notes().size(), 1);
    EXPECT_EQ(model.notes().front().toMap().value("velocity").toInt(), 97);
    ASSERT_EQ(inputHost->editor->sent.size(), 2u);
    EXPECT_EQ(inputHost->editor->sent[0].status, 0x90);
    EXPECT_EQ(inputHost->editor->sent[1].status, 0x80);
    model.startInputRecording();
    ASSERT_TRUE(model.inputRecording());
    model.selectMidiInput(1);
    ASSERT_EQ(inputHost->editor->input, "keyboard");
    const auto start = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
    inputHost->editor->received = {
        { 0xb0, 64, 127, start + 0.05 }, { 0x90, 67, 112, start + 0.1 },
        { 0x80, 67, 0, start + 0.35 }, { 0x90, 72, 80, start + 0.4 }, { 0x80, 72, 0, start + 0.7 }
    };
    const auto historyBeforeTake = history->currentStateIndex();
    model.stopInputRecording();
    ASSERT_FALSE(model.inputRecording());
    ASSERT_EQ(model.notes().size(), 3);
    const auto& data = track->GetClips().front();
    ASSERT_EQ(data.notes.size(), 3u);
    EXPECT_EQ(data.notes[1].pitch, 67);
    EXPECT_EQ(data.notes[1].velocity, 112);
    EXPECT_NEAR(data.notes[1].length, 0.5, 1e-6);
    EXPECT_EQ(data.notes[2].pitch, 72);
    EXPECT_NEAR(data.notes[2].length, 0.6, 1e-6);
    ASSERT_EQ(data.controllers.size(), 2u);
    EXPECT_EQ(data.controllers[0].value, 127);
    EXPECT_EQ(data.controllers[1].value, 0); // Recording stop releases sustain.
    EXPECT_GT(data.length, 1.4);
    EXPECT_EQ(history->currentStateIndex(), historyBeforeTake + 1);
    model.undo();
    ASSERT_EQ(model.notes().size(), 1);
    model.redo();
    ASSERT_EQ(model.notes().size(), 3);
}

TEST_F(NativeEditModelTests, SamePitchFromComputerAndDeviceKeepsBothRecordedNotesAndIndependentReleases)
{
    for (const int mode : { 120, 123, 124, 125, 126, 127 }) {
        SCOPED_TRACE(mode);
        using Source = audio::InstrumentMidiMessage::Source;
        const auto track = MidiTrack::Create();
        TrackList::Get(project()).Add(track);
        history->pushHistoryState("Instrument", "Instrument");
        trackedit::PianoRollModel model(ctx);
        model.init();
        model.setTrackId(QVariant::fromValue<qlonglong>(static_cast<int64_t>(track->GetId())));
        model.addClip(0.0, 1.0);
        model.startInputRecording();
        ASSERT_TRUE(model.inputRecording());
        model.noteOn(60, 97);
        const auto now = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
        inputHost->editor->received = { { 0x90, 60, 112, now, Source::MidiDevice } };
        QElapsedTimer wait;
        wait.start();
        while (model.heldNotes().size() < 2 && wait.elapsed() < 1000) {
            QCoreApplication::processEvents();
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        ASSERT_EQ(model.heldNotes().size(), 2);
        // A physical channel-mode message finishes only the device's note.
        inputHost->editor->received = { { 0xb0, mode, 0, now + 0.2, Source::MidiDevice } };
        wait.restart();
        while (model.heldNotes().size() != 1 && wait.elapsed() < 1000) {
            QCoreApplication::processEvents();
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        ASSERT_EQ(model.heldNotes().size(), 1);
        model.noteOff(60);
        EXPECT_TRUE(model.heldNotes().empty());
        model.stopInputRecording();
        ASSERT_EQ(track->GetClips().front().notes.size(), 2u);
        const auto& notes = track->GetClips().front().notes;
        EXPECT_TRUE(std::any_of(notes.begin(), notes.end(), [](const auto& note) { return note.velocity == 97; }));
        EXPECT_TRUE(std::any_of(notes.begin(), notes.end(), [](const auto& note) { return note.velocity == 112; }));
        model.undo();
        EXPECT_TRUE(model.notes().empty());
        model.redo();
        EXPECT_EQ(model.notes().size(), 2);
    }
}

TEST_F(NativeEditModelTests, RecordingControllerResetKeepsItsOriginalTimeAndDoesNotHoldThePedalUntilStop)
{
    using Source = audio::InstrumentMidiMessage::Source;
    const auto track = MidiTrack::Create();
    TrackList::Get(project()).Add(track);
    history->pushHistoryState("Instrument", "Instrument");
    trackedit::PianoRollModel model(ctx);
    model.init();
    model.setTrackId(QVariant::fromValue<qlonglong>(static_cast<int64_t>(track->GetId())));
    model.addClip(0, 1);
    model.startInputRecording();
    ASSERT_TRUE(model.inputRecording());
    const auto time = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
    inputHost->editor->received = {
        { 0xb0, 64, 127, time, Source::MidiDevice }, { 0x90, 60, 100, time + 0.05, Source::MidiDevice },
        { 0x80, 60, 0, time + 0.15, Source::MidiDevice }, { 0xb0, 121, 0, time + 0.3, Source::MidiDevice }
    };
    model.stopInputRecording();
    const auto saved = track->GetClips().front();
    ASSERT_EQ(saved.notes.size(), 1u);
    ASSERT_EQ(saved.controllers.size(), 2u);
    EXPECT_EQ(saved.controllers[0].number, 64);
    EXPECT_EQ(saved.controllers[0].value, 127);
    EXPECT_EQ(saved.controllers[1].number, 121);
    EXPECT_EQ(saved.controllers[1].value, 0);
    EXPECT_NEAR(saved.controllers[1].beat - saved.controllers[0].beat, 0.3 * track->GetTempo() / 60, 1e-6);
    model.undo();
    EXPECT_TRUE(model.notes().empty());
    model.redo();
    ASSERT_EQ(model.notes().size(), 1);
    const auto restored = TrackList::Get(project()).Any<MidiTrack>().begin();
    const auto& controllers = (*restored)->GetClips().front().controllers;
    ASSERT_EQ(controllers.size(), saved.controllers.size());
    for (size_t i = 0; i < controllers.size(); ++i) {
        EXPECT_DOUBLE_EQ(controllers[i].beat, saved.controllers[i].beat);
        EXPECT_EQ(controllers[i].number, saved.controllers[i].number);
        EXPECT_EQ(controllers[i].value, saved.controllers[i].value);
    }
}

TEST_F(NativeEditModelTests, SelectingAMidiDeviceDoesNotReleaseComputerKeys)
{
    const auto track = MidiTrack::Create();
    TrackList::Get(project()).Add(track);
    trackedit::PianoRollModel model(ctx);
    model.init();
    model.setTrackId(QVariant::fromValue<qlonglong>(static_cast<int64_t>(track->GetId())));
    model.noteOn(60, 97);
    ASSERT_EQ(model.heldNotes().size(), 1);
    const auto released = inputHost->editor->released;
    model.selectMidiInput(1);
    EXPECT_EQ(inputHost->editor->input, "keyboard");
    EXPECT_EQ(inputHost->editor->released, released);
    EXPECT_EQ(model.heldNotes().size(), 1);
    model.noteOff(60);
    EXPECT_TRUE(model.heldNotes().empty());
}

TEST_F(NativeEditModelTests, ConnectMidiInputRetriesTheRememberedDeviceAndReportsFailureWithoutDroppingComputerKeys)
{
    const auto track = MidiTrack::Create();
    TrackList::Get(project()).Add(track);
    trackedit::PianoRollModel model(ctx);
    model.init();
    model.setTrackId(QVariant::fromValue<qlonglong>(static_cast<int64_t>(track->GetId())));
    ASSERT_TRUE(model.enableInput());
    model.selectMidiInput(1);
    model.noteOn(60, 97);
    inputHost->editor->input.clear();
    inputHost->editor->midiAvailable = false;
    const auto connections = inputHost->editor->inputConnections;
    const auto releases = inputHost->editor->released;
    EXPECT_FALSE(model.connectMidiInput());
    EXPECT_GT(inputHost->editor->inputConnections, connections);
    EXPECT_FALSE(model.error().isEmpty());
    EXPECT_EQ(model.heldNotes().size(), 1);
    EXPECT_EQ(inputHost->editor->released, releases);
    inputHost->editor->midiAvailable = true;
    EXPECT_TRUE(model.connectMidiInput());
    EXPECT_EQ(inputHost->editor->input, "keyboard");
    EXPECT_TRUE(model.error().isEmpty());
    EXPECT_EQ(model.heldNotes().size(), 1);
    model.noteOff(60);
}

TEST_F(NativeEditModelTests, AutomationDefaultsFollowSameInstrumentPresetUndoAndRedoWithoutReloadingForUnrelatedEdits)
{
    MetadataRegistration metadata;
    const auto track = MidiTrack::Create();
    track->SetInstrument({}, "{\"0\":0.25}");
    TrackList::Get(project()).Add(track);
    history->pushHistoryState("Instrument", "Instrument");
    automation::TrackAutomationModel model(ctx);
    model.init();
    model.setTrackId(static_cast<qlonglong>(track->GetId()));
    model.setTargetIndex(1);
    ASSERT_FALSE(model.parameters().empty());
    EXPECT_NEAR(model.defaultValue(), 0.25, 1e-12);
    EXPECT_EQ(metadata.services->creations, 1);
    track->SetName("Renamed instrument");
    history->pushHistoryState("Rename", "Rename");
    EXPECT_EQ(metadata.services->creations, 1);
    track->SetInstrument({}, "{\"0\":0.75}");
    history->pushHistoryState("Preset", "Preset");
    EXPECT_NEAR(model.defaultValue(), 0.75, 1e-12);
    EXPECT_EQ(metadata.services->creations, 2);
    history->undo();
    EXPECT_NEAR(model.defaultValue(), 0.25, 1e-12);
    history->redo();
    EXPECT_NEAR(model.defaultValue(), 0.75, 1e-12);
    EXPECT_EQ(metadata.services->creations, 4);
}

TEST_F(NativeEditModelTests, AutomationReadsTheExistingLiveEditorAfterPresetChangesWithoutCreatingAnotherPlugin)
{
    MetadataRegistration metadata;
    const auto track = MidiTrack::Create();
    TrackList::Get(project()).Add(track);
    const auto live = inputHost->openInstrument(project(), *track).val;
    inputHost->editor->gain = 0.25;
    inputHost->editor->preset = "{\"0\":0.25}";
    track->SetInstrument({}, wxString::FromUTF8(inputHost->editor->preset));
    history->pushHistoryState("Instrument", "Instrument");
    automation::TrackAutomationModel model(ctx);
    model.init();
    model.setTrackId(static_cast<qlonglong>(track->GetId()));
    model.setTargetIndex(1);
    EXPECT_NEAR(model.defaultValue(), 0.25, 1e-12);
    inputHost->editor->gain = 0.75;
    inputHost->editor->preset = "{\"0\":0.75}";
    track->SetInstrument({}, wxString::FromUTF8(inputHost->editor->preset));
    history->pushHistoryState("Preset", "Preset");
    EXPECT_NEAR(model.defaultValue(), 0.75, 1e-12);
    EXPECT_EQ(metadata.services->creations, 0);
    EXPECT_EQ(inputHost->editor, live);
    EXPECT_TRUE(live->active());
}

TEST_F(NativeEditModelTests, AutomationDoesNotRetryAnUnavailableInstrumentOnEveryUnrelatedHistoryChange)
{
    MetadataRegistration metadata;
    metadata.services->fail = true;
    const auto track = MidiTrack::Create();
    track->SetInstrument({}, "{\"0\":0.25}");
    TrackList::Get(project()).Add(track);
    history->pushHistoryState("Instrument", "Instrument");
    automation::TrackAutomationModel model(ctx);
    model.init();
    model.setTrackId(static_cast<qlonglong>(track->GetId()));
    model.setTargetIndex(1);
    EXPECT_FALSE(model.error().isEmpty());
    track->SetName("Renamed instrument");
    history->pushHistoryState("Rename", "Rename");
    EXPECT_EQ(metadata.services->creations, 1);
    metadata.services->fail = false;
    model.setTargetIndex(0);
    model.setTargetIndex(1);
    EXPECT_TRUE(model.error().isEmpty());
    EXPECT_NEAR(model.defaultValue(), 0.25, 1e-12);
    EXPECT_EQ(metadata.services->creations, 2);
}

TEST_F(NativeEditModelTests, AutomationRefreshesWhenAnEffectIsReplacedWithTheSamePluginId)
{
    MetadataRegistration metadata;
    MetadataEffect effect;
    const RealtimeEffectState::EffectFactory::Scope factory([&](const PluginID&) -> const EffectInstanceFactory* { return &effect; });
    const auto track = MidiTrack::Create();
    TrackList::Get(project()).Add(track);
    auto first = std::make_shared<RealtimeEffectState>("metadata-effect");
    auto& effects = RealtimeEffectList::Get(*track);
    ASSERT_TRUE(effects.AddState(first));
    history->pushHistoryState("Instrument", "Instrument");
    automation::TrackAutomationModel model(ctx);
    model.init();
    model.setTrackId(static_cast<qlonglong>(track->GetId()));
    model.setTargetIndex(2);
    EXPECT_NEAR(model.defaultValue(), 0.25, 1e-12);
    metadata.services->effectValue = 0.75;
    ASSERT_TRUE(effects.ReplaceState(0, std::make_shared<RealtimeEffectState>("metadata-effect")));
    history->pushHistoryState("Replace effect", "Effect");
    EXPECT_NEAR(model.defaultValue(), 0.75, 1e-12);
    EXPECT_EQ(metadata.services->effectQueries, 2);
}

TEST_F(NativeEditModelTests, ReopeningAutomationReadsPendingManualEffectSettingsWithoutAnUnrelatedHistoryRefresh)
{
    MetadataRegistration metadata;
    metadata.services->nativeEffectMetadata = true;
    MetadataEffect effect;
    const RealtimeEffectState::EffectFactory::Scope factory([&](const PluginID&) -> const EffectInstanceFactory* { return &effect; });
    const auto track = MidiTrack::Create();
    TrackList::Get(project()).Add(track);
    auto state = std::make_shared<RealtimeEffectState>("metadata-effect");
    ASSERT_TRUE(RealtimeEffectList::Get(*track).AddState(state));
    history->pushHistoryState("Instrument", "Instrument");
    automation::TrackAutomationModel model(ctx);
    model.initPanel();
    model.setTrackId(static_cast<qlonglong>(track->GetId()));
    model.setTargetIndex(2);
    const auto parameters = model.parameters();
    auto bass = std::find_if(parameters.begin(), parameters.end(), [](const QVariant& parameter) {
        return parameter.toMap().value("id").toString() == "Bass";
    });
    ASSERT_NE(bass, parameters.end());
    model.setParameterIndex(static_cast<int>(bass - parameters.begin()));
    EXPECT_DOUBLE_EQ(model.defaultValue(), 0.5);
    EXPECT_EQ(metadata.services->effectQueries, 1);

    const auto access = state->GetAccess();
    access->ModifySettings([](EffectSettings& settings) {
        settings.cast<BassTrebleSettings>()->mBass = 15.0;
        return nullptr;
    });
    // The main document still has the committed value; the pending manual
    // editor value must be read without Flush or a worker acknowledgement.
    EXPECT_DOUBLE_EQ(state->GetSettings().cast<BassTrebleSettings>()->mBass, 0.0);
    requests->requestAutomation(static_cast<int64_t>(track->GetId()));
    EXPECT_DOUBLE_EQ(model.defaultValue(), 0.75);
    EXPECT_EQ(metadata.services->effectQueries, 2);
    history->pushHistoryState("Unrelated rename", "Rename");
    EXPECT_EQ(metadata.services->effectQueries, 2);
    requests->requestAutomation(static_cast<int64_t>(track->GetId()));
    EXPECT_EQ(metadata.services->effectQueries, 2);

    const auto generation = access->modificationCounter();
    for (int i = 0; i < 256; ++i) {
        access->ModifySettings([](EffectSettings& settings) {
            settings.cast<BassTrebleSettings>()->mBass = -15.0;
            return nullptr;
        });
    }
    EXPECT_EQ(access->modificationCounter(), generation + 256);
    requests->requestAutomation(static_cast<int64_t>(track->GetId()));
    EXPECT_DOUBLE_EQ(model.defaultValue(), 0.25);
    EXPECT_EQ(metadata.services->effectQueries, 3);
    access->Set();
    EXPECT_EQ(access->modificationCounter(), generation + 257);
}

TEST_F(NativeEditModelTests, MixAndRenderBakesAutomationAndDoesNotInheritFrozenSourceIdentity)
{
    auto original = wave();
    const auto expected = render(*original);
    trackedit::TrackFreezeModel freeze(ctx);
    freeze.setTrackId(static_cast<int64_t>(original->GetId()));
    freeze.freeze(0.0);
    finish(freeze);
    auto* frozen = *TrackList::Get(project()).Any<WaveTrack>().begin();
    RenderProgressUI progress;
    const auto mixed = MixAndRender(TrackList::Get(project()).Any<const WaveTrack>(),
                                    Mixer::WarpOptions { &project() }, "Rendered audio", &WaveTrackFactory::Get(project()),
                                    48000.0, floatSample, 0.0, frozen->GetEndTime());
    auto* wave = dynamic_cast<WaveTrack*>(mixed.get());
    ASSERT_NE(wave, nullptr);
    EXPECT_TRUE(TrackAutomation::Get(*wave).Lanes().empty());
    EXPECT_TRUE(TrackFreeze::Get(*wave).sourceXml.empty());
    EXPECT_TRUE(TrackFreeze::Get(*wave).blocks.empty());
    EXPECT_TRUE(TrackRouting::Get(*wave).busId.empty());
    EXPECT_TRUE(TrackRouting::Get(*wave).output.empty());
    EXPECT_TRUE(TrackRouting::Get(*wave).sends.empty());
    EXPECT_DOUBLE_EQ(wave->GetVolume(), 1.0);
    EXPECT_DOUBLE_EQ(wave->GetPan(), 0.0);
    expectSamples(expected, render(*wave));
}

TEST_F(NativeEditModelTests, MixAndRenderKeepsStereoAutomationAndDoesNotRouteBakedBusAudioTwice)
{
    auto source = wave();
    source->SetPan(0.0);
    source->SetSelected(true);
    ASSERT_TRUE(TrackAutomation::Get(*source).SetLane({ "pan", true, { { 0.0, 0.75, 0.0 } } }));
    auto bus = WaveTrackFactory::Get(project()).Create(2, floatSample, 48000.0);
    bus->SetPan(-0.5);
    bus->SetVolume(0.5);
    TrackRouting::Get(*bus).busId = "render-bus";
    TrackRouting::Get(*source).output = "render-bus";
    TrackList::Get(project()).Add(bus);
    const auto expected = render(*source, true);
    RenderProgressUI progress;
    const auto mixed = MixAndRender(TrackList::Get(project()).Selected<const WaveTrack>(),
                                    Mixer::WarpOptions { &project() }, "Rendered bus audio", &WaveTrackFactory::Get(project()),
                                    48000.0, floatSample, 0.0, source->GetEndTime());
    auto* wave = dynamic_cast<WaveTrack*>(mixed.get());
    ASSERT_NE(wave, nullptr);
    ASSERT_EQ(wave->NChannels(), 2u);
    EXPECT_TRUE(TrackAutomation::Get(*wave).Lanes().empty());
    EXPECT_TRUE(TrackRouting::Get(*wave).output.empty());
    expectSamples(expected, render(*wave, true));
}

TEST_F(NativeEditModelTests, MixCommandRendersSelectedBusWithMidiAndAutomationAndPreservesSourcesInUndoAndFile)
{
    auto audioTrack = wave();
    auto midi = MidiTrack::Create();
    ASSERT_TRUE(midi->SetClips({ { 1, "Phrase", 0.0, 0.064, 1, { { 1, 0.0, 0.05, 67, 100 } }, {} } }));
    TrackList::Get(project()).Add(midi);
    auto bus = WaveTrackFactory::Get(project()).Create(2, floatSample, 48000.0);
    TrackRouting::Get(*bus).busId = "mix-command-bus";
    TrackRouting::Get(*audioTrack).output = "mix-command-bus";
    TrackRouting::Get(*midi).output = "mix-command-bus";
    bus->SetSelected(true); // Selected bus includes the two unselected feeders.
    TrackList::Get(project()).Add(bus);
    ASSERT_TRUE(TrackAutomation::Get(*bus).SetLane({ "volume", true, { { 0.0, 0.3, 0.0 }, { 0.02, 0.8, 0.0 } } }));
    auto ui = std::make_shared<NiceMock<muse::InteractiveMock> >();
    muse::modularity::ioc(ctx)->registerExport<muse::IInteractive>("utests", ui);
    AudioRenderSource::Options options;
    options.sampleRate = 48000.0;
    options.stopTime = midi->GetEndTime();
    options.midiTracks = { midi };
    auto expectedSource = AudioRenderSource::Create(project(), { audioTrack, bus }, options);
    std::vector<float> expected;
    while (const auto count = expectedSource->Process()) {
        for (size_t i = 0; i < count; ++i) {
            for (int ch = 0; ch < 2; ++ch) {
                expected.push_back(reinterpret_cast<const float*>(expectedSource->GetBuffer(ch))[i]);
            }
        }
    }
    expectedSource.reset();
    history->pushHistoryState("Sources", "Sources");
    const auto before = history->currentStateIndex();
    trackedit::MixRenderScenario mix(ctx);
    EXPECT_TRUE(mix.run());
    ASSERT_EQ(TrackList::Get(project()).Size(), 4u);
    auto* rendered = *TrackList::Get(project()).Any<WaveTrack>().rbegin();
    EXPECT_TRUE(TrackRouting::Get(*rendered).output.empty());
    EXPECT_TRUE(TrackRouting::Get(*rendered).busId.empty());
    EXPECT_TRUE(TrackAutomation::Get(*rendered).Lanes().empty());
    expectSamples(expected, render(*rendered));
    EXPECT_EQ(history->currentStateIndex(), before + 1);
    EXPECT_EQ(TrackList::Get(project()).Any<MidiTrack>().size(), 1u);
    history->undo();
    ASSERT_EQ(TrackList::Get(project()).Size(), 3u);
    history->redo();
    audioTrack.reset();
    midi.reset();
    bus.reset();
    reopen();
    ASSERT_EQ(TrackList::Get(project()).Size(), 4u);
    expectSamples(expected, render(**TrackList::Get(project()).Any<WaveTrack>().rbegin()));
}

TEST_F(NativeEditModelTests, MixCommandRejectsCancellationEditsReentryAndAnotherProjectsStream)
{
    auto source = wave();
    source->SetSelected(true);
    auto ui = std::make_shared<NiceMock<muse::InteractiveMock> >();
    muse::modularity::ioc(ctx)->registerExport<muse::IInteractive>("utests", ui);
    trackedit::MixRenderScenario mix(ctx);
    const auto before = history->currentStateIndex();
    muse::async::Asyncable receiver;
    EXPECT_CALL(*ui, showProgress(testing::_, testing::_)).WillOnce([&](auto, muse::Progress progress) {
        EXPECT_FALSE(mix.run()); // A nested command cannot publish a second mix.
        progress.started().onNotify(&receiver, [progress]() mutable { progress.cancel(); });
    });
    EXPECT_EQ(mix.run().code(), static_cast<int>(muse::Ret::Code::Cancel));
    EXPECT_EQ(TrackList::Get(project()).Size(), 1u);
    EXPECT_EQ(history->currentStateIndex(), before);
    EXPECT_CALL(*ui, showProgress(testing::_, testing::_)).WillOnce([&](auto, muse::Progress progress) {
        progress.progressChanged().onReceive(&receiver, [source](auto, auto, auto) { source->SetVolume(0.123); });
    });
    EXPECT_EQ(mix.run().code(), static_cast<int>(muse::Ret::Code::Cancel));
    EXPECT_EQ(TrackList::Get(project()).Size(), 1u);
    EXPECT_EQ(history->currentStateIndex(), before);
    // Owning project's monitor can be stopped, another project cannot.
    auto other = AudacityProject::Create();
    ON_CALL(*audio, currentStream()).WillByDefault(Return(std::optional<audio::AudioStreamDescriptor> {
            { audio::AudioStreamKind::Monitoring, other.get(), 48000.0 } }));
    EXPECT_CALL(*audio, stopStream()).Times(0);
    EXPECT_FALSE(mix.run());
    EXPECT_EQ(TrackList::Get(project()).Size(), 1u);
    testing::Mock::VerifyAndClearExpectations(audio.get());
    std::optional<audio::AudioStreamDescriptor> monitoring { { audio::AudioStreamKind::Monitoring, &project(), 48000.0 } };
    ON_CALL(*audio, currentStream()).WillByDefault([&] { return monitoring; });
    ON_CALL(*audio, canStopAudioStream(testing::_)).WillByDefault(Return(true));
    EXPECT_CALL(*audio, stopStream()).Times(1).WillOnce([&] { monitoring.reset(); });
    EXPECT_CALL(*ui, showProgress(testing::_, testing::_)).Times(1);
    EXPECT_TRUE(mix.run());
    EXPECT_EQ(TrackList::Get(project()).Size(), 2u);
}
}
