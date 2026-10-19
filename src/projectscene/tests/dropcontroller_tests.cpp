/*
* Audacity: A Digital Audio Editor
*/
#include <gtest/gtest.h>
#include <gmock/gmock.h>
#include <QUrl>
#include <map>
#include <set>

#include "projectscene/view/tracksitemsview/dropcontroller.h"
#include "trackedit/internal/au3/au3tracksinteraction.h"
#include "context/tests/mocks/globalcontextmock.h"
#include "project/tests/mocks/audacityprojectmock.h"
#include "trackedit/tests/mocks/trackeditprojectmock.h"

using namespace testing;

namespace au::projectscene {
namespace {
class ImportProbe final : public importexport::IImporter
{
public:
    std::map<QString, importexport::FileInfo> files;
    importexport::FileInfo fileInfo(const muse::io::path_t& path) override { return files[path.toQString()]; }
    bool import(const muse::io::path_t&) override { return false; }
    bool importIntoTrack(const muse::io::path_t&, trackedit::TrackId, trackedit::secs_t) override { return false; }
    bool importFromSystemClipboard(const std::vector<muse::io::path_t>&, trackedit::secs_t) override { return false; }
    std::vector<std::string> supportedExtensions() const override { return { "mid", "midi", "wav" }; }
};

class DropTracks final : public trackedit::Au3TracksInteraction
{
public:
    explicit DropTracks(const muse::modularity::ContextPtr& ctx)
        : Au3TracksInteraction(ctx) {}
    trackedit::TrackList tracks;
    std::set<trackedit::TrackId> populated;
    trackedit::TrackId addWaveTrack(int) override { return add(trackedit::TrackType::Mono); }
    trackedit::TrackId addInstrumentTrack(trackedit::TrackId) override { return add(trackedit::TrackType::Midi); }
    bool deleteTracks(const trackedit::TrackIdList& ids) override
    {
        std::erase_if(tracks, [&](const auto& t) { return muse::contains(ids, t.id); });
        return true;
    }

    void removeDragAddedTracks(size_t before, bool) override
    {
        trackedit::TrackIdList unused;
        for (size_t i = before; i < tracks.size(); ++i) {
            if (!populated.contains(tracks[i].id)) {
                unused.push_back(tracks[i].id);
            }
        }
        deleteTracks(unused);
    }

private:
    trackedit::TrackId add(trackedit::TrackType type)
    {
        const auto id = nextId++;
        tracks.push_back({ id, {}, type });
        return id;
    }

    trackedit::TrackId nextId = 100;
};
}

class DropControllerTests : public Test
{
protected:
    void SetUp() override
    {
        ctx = std::make_shared<muse::modularity::Context>(811);
        context = std::make_shared<NiceMock<context::GlobalContextMock> >();
        document = std::make_shared<NiceMock<project::AudacityProjectMock> >();
        project = std::make_shared<NiceMock<trackedit::TrackeditProjectMock> >();
        tracks = std::make_shared<DropTracks>(ctx);
        probe = std::make_shared<ImportProbe>();
        ON_CALL(*context, currentProject()).WillByDefault(Return(document));
        ON_CALL(*context, currentTrackeditProject()).WillByDefault(Return(project));
        ON_CALL(*project, trackList()).WillByDefault([this] { return tracks->tracks; });
        auto* ioc = muse::modularity::ioc(ctx);
        ioc->registerExport<context::IGlobalContext>("utests", context);
        ioc->registerExport<trackedit::ITracksInteraction>("utests", tracks);
        ioc->registerExport<importexport::IImporter>("utests", probe);
        controller = std::make_unique<DropController>();
        controller->setContext(ctx);
    }

    void TearDown() override
    {
        controller.reset();
        muse::modularity::removeIoC(ctx);
    }

    void addFile(const QString& path, bool midi, int count)
    {
        probe->files[path] = { muse::io::path_t(path), 2.0, count, midi };
    }

    std::vector<trackedit::TrackId> ids(int target, int count)
    {
        std::vector<trackedit::TrackId> result;
        for (const auto& value : controller->draggedTracksIds(target, count)) {
            result.push_back(value.toInt());
        }
        return result;
    }

    muse::modularity::ContextPtr ctx;
    std::shared_ptr<NiceMock<context::GlobalContextMock> > context;
    std::shared_ptr<NiceMock<project::AudacityProjectMock> > document;
    std::shared_ptr<NiceMock<trackedit::TrackeditProjectMock> > project;
    std::shared_ptr<DropTracks> tracks;
    std::shared_ptr<ImportProbe> probe;
    std::unique_ptr<DropController> controller;
};

TEST_F(DropControllerTests, MidiDropSkipsAudioAndReusesCompatibleTracks)
{
    tracks->tracks = { { 1, {}, trackedit::TrackType::Mono }, { 2, {}, trackedit::TrackType::Midi },
        { 3, {}, trackedit::TrackType::Label }, { 4, {}, trackedit::TrackType::Midi } };
    addFile("/tmp/score.mid", true, 2);
    controller->probeAudioFiles({ "/tmp/score.mid" });
    controller->startImportSession();
    controller->prepareConditionalTracks(1, 2);
    EXPECT_EQ(ids(1, 2), (std::vector<trackedit::TrackId> { 2, 4 }));
    EXPECT_EQ(tracks->tracks.size(), 4u);
    controller->endImportSession();
    EXPECT_EQ(tracks->tracks.size(), 4u);
}

TEST_F(DropControllerTests, MixedDropCreatesTypedGhostsAndCancelRemovesThem)
{
    tracks->tracks = { { 1, {}, trackedit::TrackType::Mono } };
    addFile("/tmp/score.midi", true, 2);
    addFile("/tmp/drums.wav", false, 1);
    controller->probeAudioFiles({ "/tmp/score.midi", "/tmp/drums.wav" });
    controller->startImportSession();
    controller->prepareConditionalTracks(-1, 3);
    ASSERT_EQ(tracks->tracks.size(), 4u);
    EXPECT_EQ(tracks->tracks[1].type, trackedit::TrackType::Midi);
    EXPECT_EQ(tracks->tracks[2].type, trackedit::TrackType::Midi);
    EXPECT_EQ(tracks->tracks[3].type, trackedit::TrackType::Mono);
    const auto first = ids(-1, 3);
    controller->prepareConditionalTracks(-1, 3);
    EXPECT_EQ(ids(-1, 3), first);
    EXPECT_EQ(tracks->tracks.size(), 4u);
    for (int repeat = 0; repeat < 5; ++repeat) {
        for (const auto& id : first) {
            controller->prepareConditionalTracks(id, 3);
            EXPECT_EQ(ids(id, 3), first);
            EXPECT_EQ(tracks->tracks.size(), 4u);
        }
    }
    controller->endImportSession();
    EXPECT_EQ(tracks->tracks.size(), 1u);
}

TEST_F(DropControllerTests, UnusedGhostsAreRemovedBeforeTheImportUndoSnapshot)
{
    tracks->tracks = { { 1, {}, trackedit::TrackType::Midi }, { 2, {}, trackedit::TrackType::Mono } };
    addFile("/tmp/score.mid", true, 1);
    addFile("/tmp/drums.wav", false, 1);
    controller->probeAudioFiles({ "/tmp/score.mid", "/tmp/drums.wav" });
    controller->startImportSession();
    controller->prepareConditionalTracks(-1, 2);
    ASSERT_EQ(tracks->tracks.size(), 4u);
    controller->prepareConditionalTracks(1, 2);
    EXPECT_CALL(*document, importIntoTracks(_, _, muse::secs_t(3.0))).WillOnce([&](const auto& paths, const auto& targets, double) {
        EXPECT_EQ(paths.size(), 2u);
        EXPECT_EQ(targets, (std::vector<trackedit::TrackId> { 1, 2 }));
        EXPECT_EQ(tracks->tracks.size(), 2u);
        return muse::Ret(muse::Ret::Code::Ok);
    });
    controller->handleDroppedFiles(ids(1, 2), 3.0);
    controller->endImportSession();
    EXPECT_EQ(tracks->tracks.size(), 2u);
}

TEST_F(DropControllerTests, ProbeKeepsValidUnicodePathsAlignedWhenOtherFilesAreInvalid)
{
    const QString path = QString::fromUtf8("/tmp/Mélodie.MID");
    addFile(path, true, 2);
    controller->probeAudioFiles({ "/tmp/bad.mid", QUrl::fromLocalFile(path).toString() });
    EXPECT_EQ(controller->requiredTracksCount(), 2);
    EXPECT_EQ(controller->lastProbedFileNames(), (QVariantList { QString::fromUtf8("Mélodie"), QString::fromUtf8("Mélodie") }));
    controller->startImportSession();
    controller->prepareConditionalTracks(-1, 2);
    EXPECT_CALL(*document, importIntoTracks(_, _, muse::secs_t(0.0))).WillOnce([&](const auto& paths, const auto& targets, double) {
        EXPECT_EQ(paths, (std::vector<muse::io::path_t> { muse::io::path_t(path) }));
        EXPECT_EQ(targets.size(), 1u);
        tracks->populated.insert(100);
        tracks->populated.insert(101);
        return muse::Ret(muse::Ret::Code::Ok);
    });
    controller->handleDroppedFiles(ids(-1, 2), 0.0);
    controller->endImportSession();
    EXPECT_EQ(tracks->tracks.size(), 2u);
}
}
