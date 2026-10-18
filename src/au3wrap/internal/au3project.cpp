/*
* Audacity: A Digital Audio Editor
*/
#include "au3project.h"

#include "framework/global/defer.h"
#include "framework/global/translation.h"
#include "framework/global/log.h"

#include "au3-project-history/ProjectHistory.h"
#include "au3-project-history/UndoManager.h"
#include "au3-realtime-effects/SavedMasterEffectList.h"
#include "au3-file-formats/AcidizerTags.h"
#include "au3-import-export/Import.h"
#include "au3-import-export/ImportPlugin.h"
#include "au3-numeric-formats/ProjectTimeSignature.h"
#include "au3-project-file-io/ProjectFileIO.h"
#include "au3-project-file-io/ProjectSerializer.h"
#include "au3-project/Project.h"
#include "au3-wave-track/WaveClip.h"
#include "au3-wave-track/WaveTrack.h"
#include "au3-wave-track/WaveTrackUtilities.h"
#include "au3-stretching-sequence/TempoChange.h"
#include "projectthumbnail.h"

//! HACK
//! Static variable is not initialized
//! static SampleBlockFactory::Factory::Scope scope{ []( AudacityProject &project )
//! so, to fix it, included this file here
#include "au3-project-file-io/SqliteSampleBlock.cpp"

#include "project/projecterrors.h"

#include "wxtypes_convert.h"
#include "trackcolor.h"
#include "../au3projectcontext.h"
#include "../au3types.h"

#include <random>
#include <wx/filename.h>

using namespace au::au3;

namespace {
size_t randomIndex()
{
    std::random_device rd;
    std::mt19937 generator(rd());
    std::uniform_int_distribution<size_t> distribution(0, 20);
    return distribution(generator);
}
}

std::shared_ptr<IAu3Project> Au3ProjectCreator::create(const muse::modularity::ContextPtr& ctx) const
{
    TrackColor::Init(randomIndex());
    return std::make_shared<Au3ProjectAccessor>(ctx);
}

std::optional<std::vector<uint8_t> > Au3ProjectReader::readProjectThumbnail(const muse::io::path_t& projectPath) const
{
    const std::string sstr = projectPath.toStdString();
    const FilePath fileName = wxString::FromUTF8(sstr.c_str(), sstr.size());
    if (fileName.empty()) {
        return std::nullopt;
    }

    return ProjectFileIO::ReadThumbnail(fileName);
}

muse::Ret Au3ProjectCreator::removeUnsavedData(const muse::io::path_t& projectPath) const
{
    // Helper method to remove autosave data from a project file without keeping it open
    const auto tempProject = create(muse::modularity::globalCtx());
    if (!tempProject) {
        return muse::make_ret(static_cast<muse::Ret::Code>(au::project::Err::NoProjectError));
    }
    constexpr auto ignoreAutosave = true;
    auto ret = tempProject->load(projectPath, ignoreAutosave);
    if (ret) {
        if (tempProject->isTemporary()) {
            tempProject->close();
            ret = fileSystem()->remove(projectPath);
        } else {
            ret = tempProject->removeAutosaveData();
            tempProject->close();
        }
    }
    return ret;
}

struct au::au3::Au3ProjectData
{
    std::shared_ptr<Au3Project> project;

    Au3Project& projectRef() { return *project.get(); }
};

Au3ProjectAccessor::Au3ProjectAccessor(const muse::modularity::ContextPtr& ctx)
    : IAu3Project(ctx), m_data(std::make_shared<Au3ProjectData>())
{
    m_data->project = Au3Project::Create();
    setProjectIocContext(m_data->projectRef(), ctx);
    mTrackListSubstription = Au3TrackList::Get(m_data->projectRef()).Subscribe([this](const TrackListEvent& event)
    {
        if (event.mType == TrackListEvent::ADDITION) {
            const auto tempo = ProjectTimeSignature::Get(m_data->projectRef()).GetTempo();
            if (const auto track = event.mpTrack.lock()) {
                DoProjectTempoChange(*track, tempo);
            }
        }
    });

    // Subscribe to undo manager changes for save status notifications
    mUndoSubscription = UndoManager::Get(m_data->projectRef()).Subscribe([this](const UndoRedoMessage& message) {
        switch (message.type) {
            case UndoRedoMessage::Pushed:
            case UndoRedoMessage::Modified:
            case UndoRedoMessage::UndoOrRedo:
            case UndoRedoMessage::Reset:
                m_projectChanged.notify();
                break;
            default:
                break;
        }
    });
}

muse::Ret Au3ProjectAccessor::open()
{
    auto& projectFileIO = ProjectFileIO::Get(m_data->projectRef());
    return projectFileIO.OpenProject() ? muse::make_ok() : muse::make_ret(static_cast<muse::Ret::Code>(project::Err::DatabaseError));
}

muse::Ret Au3ProjectAccessor::load(const muse::io::path_t& filePath, bool ignoreAutosave)
{
    muse::Ret ret = muse::make_ok();

    auto& project = m_data->projectRef();
    auto& projectFileIO = ProjectFileIO::Get(project);
    std::string sstr = filePath.toStdString();
    FilePath fileName = wxString::FromUTF8(sstr.c_str(), sstr.size());

    std::optional<ProjectFileIO::TentativeConnection> conn;

    try {
        conn.emplace(projectFileIO.LoadProject(fileName, ignoreAutosave).value());
    } catch (AudacityException& exception) {
        LOGE() << "Project loading failed: " << filePath << " with exception";
        ret = project::make_ret(project::Err::AudacityExceptionError, filePath.toString());
        return ret;
    } catch (std::bad_optional_access)
    {
        auto ret = muse::Ret(projectFileIO.GetLastErrorCode());
        ret.setData("title", muse::mtrc("project", "Project loading failed").toStdString());
        ret.setData("body", projectFileIO.GetLastError().translated().toStdString());
        ret.setData("path", filePath);
        return ret;
    }

    muse::Defer([&conn]() {
        conn->Commit();
    });

    const bool bParseSuccess = conn.has_value();
    if (!bParseSuccess) {
        LOGE() << "failed load project: " << filePath;
        ret = make_ret(muse::Ret::Code::UnknownError, "Failed to load project: " + filePath.toString());
        return false;
    }

    //! TODO Look like, need doing all from method  ProjectFileManager::FixTracks
    //! and maybe what is done before this method (ProjectFileManager::ReadProjectFile)
    Au3TrackList& tracks = Au3TrackList::Get(project);
    for (auto pTrack : tracks) {
        pTrack->LinkConsistencyFix();
    }

    // Update saved state BEFORE sanitizing
    // Saved state should represent what is stored on disk
    updateSavedState();

    //! NOTE: sanitize legacy/corrupted projects that were saved with overlapping
    //! clip play regions (an unsupported state). Done here, before the trackedit
    //! project and UI are built, so no change notifications are needed.
    for (auto pWaveTrack : tracks.Any<WaveTrack>()) {
        WaveTrackUtilities::RemoveOverlaps(*pWaveTrack);
    }

    return ret;
}

void Au3ProjectAccessor::saveThumbnail(std::vector<uint8_t> pngData)
{
    ProjectThumbnail::Get(m_data->projectRef()).SetData(std::move(pngData));
}

Au3ProjectAccessor::~Au3ProjectAccessor() = default;

bool Au3ProjectAccessor::save(const muse::io::path_t& filePath)
{
    auto& project = m_data->projectRef();

    auto& projectFileIO = ProjectFileIO::Get(project);
    auto destination = wxFromString(filePath.toString());
    const auto current = projectFileIO.GetFileName();
    // An alias of the active file is an ordinary save. Do not move the live
    // database (or copy it onto itself) because its spelling changed.
    if (wxFileName(destination).SameAs(wxFileName(current))) {
        destination = current;
    }
    // Match classic Audacity's Save As protection: a destination's journal
    // belongs to its old database, and must move with it before replacement.
    // BackupProject restores the whole bundle if the new save fails.
    std::optional<ProjectFileIO::BackupProject> backup;
    if (destination != current && wxFileExists(destination)) {
        backup.emplace(projectFileIO, destination);
        if (!backup->IsOk()) {
            return false;
        }
    }

    // Serialize current effects into the destination, while retaining the
    // original file's saved snapshot for compaction and for a failed save.
    auto& master = SavedMasterEffectList::Get(project);
    auto previous = master.SwapCopy(std::make_unique<RealtimeEffectList>(RealtimeEffectList::Get(project)));
    bool result = false;
    DEFER {
        if (!result && projectFileIO.GetFileName() == current) {
            master.SwapCopy(std::move(previous));
        }
    };
    result = projectFileIO.SaveProject(destination, m_lastSavedTracks.get(), m_lastSavedDocument.get());
    if (result) {
        if (backup) {
            backup->Discard();
        }
        UndoManager::Get(project).StateSaved();
        // Update state only when save succeeded
        updateSavedState();
    }

    return result;
}

bool Au3ProjectAccessor::saveCopy(const muse::io::path_t& filePath)
{
    auto& project = m_data->projectRef();
    auto& io = ProjectFileIO::Get(project);
    const auto destination = wxFromString(filePath.toString());
    // Copying over the open source would destroy its connection's database.
    if (destination.empty() || wxFileName(destination).SameAs(wxFileName(io.GetFileName()))) {
        return false;
    }

    std::optional<ProjectFileIO::BackupProject> backup;
    if (wxFileExists(destination)) {
        backup.emplace(io, destination);
        if (!backup->IsOk()) {
            return false;
        }
    }

    auto& master = SavedMasterEffectList::Get(project);
    auto previous = master.SwapCopy(std::make_unique<RealtimeEffectList>(RealtimeEffectList::Get(project)));
    DEFER { master.SwapCopy(std::move(previous));
    };
    const auto result = io.SaveCopy(destination);
    if (result && backup) {
        backup->Discard();
    }
    // The active connection, last-saved tracks, undo save point and temporary
    // status continue to belong to the source document.
    return result;
}

void Au3ProjectAccessor::updateSavedState()
{
    auto& project = m_data->projectRef();

    SavedMasterEffectList::Get(project).UpdateCopy();

    clearSavedState();
    m_lastSavedTracks = TrackList::Create(nullptr);
    for (auto t : TrackList::Get(project).Any<Track>()) {
        m_lastSavedTracks->Add(t->Duplicate(Track::DuplicateOptions {}.Backup()), TrackList::DoAssignId::No);
    }
    auto& io = ProjectFileIO::Get(project);
    m_lastSavedDocument = io.CaptureDocument(m_lastSavedTracks.get());
}

void Au3ProjectAccessor::clearSavedState()
{
    m_lastSavedDocument.reset();
    if (m_lastSavedTracks) {
        m_lastSavedTracks->Clear();
        m_lastSavedTracks.reset();
    }
}

void Au3ProjectAccessor::close()
{
    auto& project = m_data->projectRef();

    //! ============================================================================
    //! NOTE Step 1 - Go back to the last saved state if needed
    //! ============================================================================
    auto& undoManager = UndoManager::Get(project);
    if (undoManager.GetSavedState() >= 0) {
        constexpr auto doAutoSave = false;
        ProjectHistory::Get(project).SetStateTo(
            undoManager.GetSavedState(), doAutoSave);
    }

    //! ============================================================================
    //! NOTE Step 2 - We compact the project only around the tracks that were actually
    //! saved. This will allow us to later on clear the UndoManager states bypassing
    //! the deletion of blocks 1 by 1, which can take a long time for projects with
    //!  large unsaved audio ; see https://github.com/audacity/audacity/issues/7382
    //! ============================================================================
    auto& projectFileIO = ProjectFileIO::Get(project);

    // Recovered and never saved this session
    if (projectFileIO.IsRecovered() && !projectFileIO.IsTemporary()) {
        // the project doc in the file is still the last save
        // so dropping the autosave is reverting to the last saved state
        //
        // Compacting here would silently commit the recovered state, that was presumably declined
        if (m_lastSavedTracks) {
            for (auto wt : m_lastSavedTracks->Any<WaveTrack>()) {
                WaveTrackUtilities::CloseLock(*wt);
            }
        }
        projectFileIO.AutoSaveDelete();
    } else if (m_lastSavedTracks) {
        // Lock all blocks in all tracks of the last saved version, so that
        // the sample blocks aren't deleted from the database when we destroy the
        // sample block objects in memory.
        for (auto wt : m_lastSavedTracks->Any<WaveTrack>()) {
            WaveTrackUtilities::CloseLock(*wt);
        }

        // Attempt to compact the project
        projectFileIO.Compact({ m_lastSavedTracks.get() }, false, m_lastSavedDocument.get());

        if (
            !projectFileIO.WasCompacted() && undoManager.UnsavedChanges()) {
            // If compaction failed, we must do some work in case of close
            // without save.  Don't leave the document blob from the last
            // push of undo history, when that undo state may get purged
            // with deletion of some new sample blocks.
            // REVIEW: UpdateSaved() might fail too.  Do we need to test
            // for that and report it?
            projectFileIO.UpdateSaved(m_lastSavedTracks.get(), m_lastSavedDocument.get());
        }
    }

    //! ============================================================================
    //! NOTE Step 3
    //! Set (or not) the bypass flag to indicate that deletes that would happen
    //! during undoManager.ClearStates() below are not necessary. Must be called
    //! between `CompactProjectOnClose()` and `undoManager.ClearStates()`.
    //! ============================================================================
    projectFileIO.SetBypass();

    // This can reduce reference counts of sample blocks in the project's
    // tracks.
    undoManager.ClearStates();

    // Delete all the tracks to free up memory
    TrackList::Get(project).Clear();

    projectFileIO.CloseProject();
}

std::string Au3ProjectAccessor::title() const
{
    if (!m_data->project) {
        return std::string();
    }

    return wxToStdString(m_data->project->GetProjectName());
}

uintptr_t Au3ProjectAccessor::au3ProjectPtr() const
{
    return reinterpret_cast<uintptr_t>(m_data->project.get());
}

bool Au3ProjectAccessor::hasUnsavedChanges() const
{
    const auto& undoManager = UndoManager::Get(m_data->projectRef());

    // Check UndoManager for regular changes
    if (undoManager.UnsavedChanges()) {
        return true;
    }

    // Check for autosave data (recovered projects with modifications)
    if (hasAutosaveData()) {
        return true;
    }

    return false;
}

void Au3ProjectAccessor::markAsSaved()
{
    auto& undoManager = UndoManager::Get(m_data->projectRef());
    undoManager.StateSaved();
}

bool Au3ProjectAccessor::isRecovered() const
{
    const auto& projectFileIO = ProjectFileIO::Get(m_data->projectRef());
    return projectFileIO.IsRecovered();
}

bool Au3ProjectAccessor::isTemporary() const
{
    const auto& projectFileIO = ProjectFileIO::Get(m_data->projectRef());
    return projectFileIO.IsTemporary();
}

muse::async::Notification Au3ProjectAccessor::projectChanged() const
{
    return m_projectChanged;
}

bool Au3ProjectAccessor::hasAutosaveData() const
{
    const auto& projectFileIO = ProjectFileIO::Get(m_data->projectRef());
    return projectFileIO.IsRecovered() && projectFileIO.IsModified();
}

muse::Ret Au3ProjectAccessor::removeAutosaveData()
{
    auto& projectFileIO = ProjectFileIO::Get(m_data->projectRef());
    return projectFileIO.AutoSaveDelete() ? muse::make_ok() : muse::make_ret(static_cast<muse::Ret::Code>(project::Err::DatabaseError));
}

muse::io::path_t Au3ProjectAccessor::getFileName() const
{
    auto& projectFileIO = ProjectFileIO::Get(m_data->projectRef());
    return wxToStdString(projectFileIO.GetFileName());
}
