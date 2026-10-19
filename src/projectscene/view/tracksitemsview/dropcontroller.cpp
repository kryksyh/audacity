#include "dropcontroller.h"

#include <set>

using namespace au::projectscene;

namespace {
auto isAudioTrack = [](au::trackedit::TrackType type) {
    return type == au::trackedit::TrackType::Mono || type == au::trackedit::TrackType::Stereo;
};
}

DropController::DropController(QObject* parent)
    : QObject(parent), muse::Contextable(muse::iocCtxForQmlObject(this))
{}

void DropController::probeAudioFiles(const QStringList& fileUrls)
{
    m_lastDraggedUrls.clear();
    m_lastDraggedFilesInfo.clear();

    std::vector<muse::io::path_t> localPaths;
    localPaths.reserve(fileUrls.size());

    const auto exts = importer()->supportedExtensions();
    for (const auto& fileUrl : fileUrls) {
        const QUrl url(fileUrl);
        QString local = url.isLocalFile() ? url.toLocalFile() : fileUrl;
        muse::io::path_t path = muse::io::path_t(local);
        if (muse::contains(exts, QString::fromStdString(muse::io::suffix(path)).toLower().toStdString())) {
            localPaths.push_back(path);
        }
    }

    if (localPaths.empty()) {
        return;
    }

    for (const auto& path : localPaths) {
        au::importexport::FileInfo fileInfo = importer()->fileInfo(path);
        if (fileInfo.isEmpty()) {
            continue;
        }
        m_lastDraggedUrls.push_back(fileInfo.path.toQString());
        m_lastDraggedFilesInfo.push_back(std::move(fileInfo));
    }
}

QVariantList DropController::lastProbedDurations() const
{
    QVariantList out;
    out.reserve(static_cast<int>(m_lastDraggedFilesInfo.size()));
    for (const auto& info : m_lastDraggedFilesInfo) {
        for (int i = 0; i < info.trackCount; ++i) {
            out.push_back(info.duration);
        }
    }
    return out;
}

QVariantList DropController::lastProbedFileNames() const
{
    QVariantList out;
    out.reserve(static_cast<int>(requiredTracksCount()));

    auto urlIter  = m_lastDraggedUrls.cbegin();
    auto infoIter = m_lastDraggedFilesInfo.cbegin();
    for (; urlIter != m_lastDraggedUrls.cend() && infoIter != m_lastDraggedFilesInfo.cend();
         ++urlIter, ++infoIter) {
        std::string title = muse::io::filename(*urlIter, false /* including extension */).toStdString();

        for (int n = 0; n < infoIter->trackCount; ++n) {
            out.push_back(QString::fromStdString(title));
        }
    }

    return out;
}

void DropController::startImportSession()
{
    if (m_trackCountBeforeImport != -1) {
        return;
    }

    au::trackedit::ITrackeditProjectPtr prj = globalContext()->currentTrackeditProject();
    if (!prj) {
        return;
    }

    m_trackCountBeforeImport = prj->trackList().size();
}

void DropController::endImportSession()
{
    tracksInteraction()->removeDragAddedTracks(m_trackCountBeforeImport, true /* emptyOnly */);

    m_trackCountBeforeImport = -1;
    m_lastDraggedFilesInfo.clear();
    m_lastDraggedUrls.clear();
}

int DropController::requiredTracksCount() const
{
    int count = 0;
    for (const auto& info : m_lastDraggedFilesInfo) {
        count += info.trackCount;
    }

    return count;
}

void DropController::prepareConditionalTracks(int currentTrackId, int draggedFilesCount)
{
    if (draggedFilesCount <= 0) {
        return;
    }

    if (hasMidiFiles()) {
        const auto types = draggedMidiTypes(draggedFilesCount);
        for (int i = 0; i < draggedFilesCount; ++i) {
            const auto ids = midiAwareTrackIds(currentTrackId, draggedFilesCount);
            if (ids.size() != draggedFilesCount) {
                return;
            }
            if (ids[i].toInt() != trackedit::INVALID_TRACK) {
                continue;
            }
            if (types[i]) {
                tracksInteraction()->addInstrumentTrack();
            } else {
                tracksInteraction()->addWaveTrack(1);
            }
        }
        return;
    }

    au::trackedit::ITrackeditProjectPtr prj = globalContext()->currentTrackeditProject();
    if (!prj) {
        return;
    }

    std::vector<trackedit::Track> trackList = prj->trackList();

    const int totalTracks = static_cast<int>(trackList.size());

    const int tracksCreated
        =(m_trackCountBeforeImport >= 0)
          ? std::max(0, totalTracks - m_trackCountBeforeImport)
          : 0;

    if (tracksCreated >= draggedFilesCount) {
        return;
    }

    int currentIndex = -1;
    for (int i = 0; i < totalTracks; ++i) {
        if (trackList[i].id == currentTrackId) {
            currentIndex = i;
            break;
        }
    }

    int startIndex = (currentIndex < 0) ? totalTracks : currentIndex;

    int availableAudioTracks = 0;
    for (int i = startIndex; i < totalTracks; ++i) {
        if (isAudioTrack(trackList[i].type)) {
            ++availableAudioTracks;
        }
    }

    int missingTracks = draggedFilesCount - availableAudioTracks;
    if (missingTracks <= 0) {
        return;
    }

    const int maxNewAllowed = draggedFilesCount - tracksCreated;
    if (maxNewAllowed <= 0) {
        return;
    }

    const int toCreate = std::min(missingTracks, maxNewAllowed);

    for (int i = 0; i < toCreate; ++i) {
        tracksInteraction()->addWaveTrack(1);
    }
}

QVariantList DropController::draggedTracksIds(int currentTrackId, int draggedFilesCount)
{
    if (hasMidiFiles()) {
        return midiAwareTrackIds(currentTrackId, draggedFilesCount);
    }
    au::trackedit::ITrackeditProjectPtr prj = globalContext()->currentTrackeditProject();
    if (!prj) {
        return {};
    }

    std::vector<trackedit::Track> trackList = prj->trackList();

    QVariantList trackIds;
    if (draggedFilesCount <= 0 || trackList.empty()) {
        return trackIds;
    }

    const int total = static_cast<int>(trackList.size());

    // collect indices of all audio tracks
    std::vector<int> audioIndices;
    audioIndices.reserve(total);
    for (int i = 0; i < total; ++i) {
        if (isAudioTrack(trackList[i].type)) {
            audioIndices.push_back(i);
        }
    }

    if (audioIndices.empty()) {
        return trackIds;
    }

    int currentRow = -1;
    if (currentTrackId >= 0) {
        for (int i = 0; i < total; ++i) {
            if (trackList[i].id == currentTrackId) {
                currentRow = i;
                break;
            }
        }
    }

    int thresholdRow;
    if (currentRow >= 0) {
        // cursor is over a track, start from that track
        thresholdRow = currentRow;
    } else {
        // cursor is below tracks, start from newly created track
        thresholdRow = std::max(0, m_trackCountBeforeImport);
    }

    int startAudioPos = 0;
    while (startAudioPos < static_cast<int>(audioIndices.size())
           && audioIndices[startAudioPos] < thresholdRow) {
        ++startAudioPos;
    }

    const int availFromStart = static_cast<int>(audioIndices.size()) - startAudioPos;

    if (availFromStart >= draggedFilesCount) {
        for (int i = 0; i < draggedFilesCount; ++i) {
            int trackIndex = audioIndices[startAudioPos + i];
            trackIds.push_back(static_cast<int>(trackList[trackIndex].id));
        }
        return trackIds;
    }

    // fallback, not enough tracks: use last `draggedFilesCount` audio tracks
    const int totalAudio = static_cast<int>(audioIndices.size());
    const int toTake = std::min(draggedFilesCount, totalAudio);
    const int firstIndex = totalAudio - toTake;

    for (int i = firstIndex; i < totalAudio; ++i) {
        int trackIndex = audioIndices[i];
        trackIds.push_back(static_cast<int>(trackList[trackIndex].id));
    }

    return trackIds;
}

void DropController::removeDragAddedTracks(int currentTrackId, int draggedFilesCount)
{
    // Keep compatible conditional tracks reusable as a mixed audio/MIDI drag
    // crosses track types. endImportSession removes all unused empty tracks.
    if (hasMidiFiles()) {
        return;
    }
    if (draggedFilesCount <= 0 || m_trackCountBeforeImport < 0) {
        return;
    }

    au::trackedit::ITrackeditProjectPtr prj = globalContext()->currentTrackeditProject();
    if (!prj) {
        return;
    }

    std::vector<trackedit::Track> trackList = prj->trackList();

    const int total = static_cast<int>(trackList.size());
    if (total <= m_trackCountBeforeImport) {
        return;
    }

    int currentIndex = -1;
    for (int i = 0; i < total; ++i) {
        if (trackList[i].id == currentTrackId) {
            currentIndex = i;
            break;
        }
    }

    int startIndex;
    if (currentIndex >= 0) {
        // cursor is over some existing track (label or audio)
        startIndex = currentIndex;
    } else if (m_trackCountBeforeImport >= 0
               && m_trackCountBeforeImport < total) {
        // cursor is below the last track
        startIndex = m_trackCountBeforeImport;
    } else {
        // fallback
        startIndex = 0;
    }

    int remaining = draggedFilesCount;
    int highestUsedIndex = m_trackCountBeforeImport - 1;

    // detect where dragged files will land: walk from startIndex downwards,
    // counting only audio tracks.
    for (int i = startIndex; i < total && remaining > 0; ++i) {
        if (!isAudioTrack(trackList[i].type)) {
            continue;
        }

        highestUsedIndex = std::max(highestUsedIndex, i);
        --remaining;
    }

    int neededTracksCount = m_trackCountBeforeImport;
    if (highestUsedIndex >= 0) {
        neededTracksCount = std::max(neededTracksCount, highestUsedIndex + 1);
    }

    // remove only extra empty tracks beyond neededTracksCount
    tracksInteraction()->removeDragAddedTracks(neededTracksCount, true /* emptyOnly */);
}

void DropController::handleDroppedFiles(const std::vector<trackedit::TrackId>& trackIds, double startTime)
{
    std::vector<muse::io::path_t> localPaths;

    // NOTE: importer only needs the first trackId (out of many) for multichannel files
    // while `trackIds` contains all, we may need to skip some of them
    std::vector<trackedit::TrackId> adjustedDstTrackIds;
    auto dstTrackIter = trackIds.begin();
    for (const auto& info : m_lastDraggedFilesInfo) {
        if (info.trackCount <= 0 || std::distance(dstTrackIter, trackIds.end()) < info.trackCount
            || *dstTrackIter == trackedit::INVALID_TRACK) {
            return;
        }
        localPaths.push_back(info.path);

        adjustedDstTrackIds.push_back(*dstTrackIter);
        std::advance(dstTrackIter, info.trackCount);
    }

    project::IAudacityProjectPtr prj = globalContext()->currentProject();

    if (hasMidiFiles() && m_trackCountBeforeImport >= 0) {
        // Discard unused conditional tracks before the project's Import undo
        // snapshot is made, including gaps in a mixed audio/MIDI preview.
        const auto tracks = globalContext()->currentTrackeditProject()->trackList();
        trackedit::TrackIdList unused;
        for (size_t i = static_cast<size_t>(m_trackCountBeforeImport); i < tracks.size(); ++i) {
            if (!muse::contains(trackIds, tracks[i].id)
                && globalContext()->currentTrackeditProject()->clipList(tracks[i].id).empty()) {
                unused.push_back(tracks[i].id);
            }
        }
        if (!unused.empty()) {
            tracksInteraction()->deleteTracks(unused);
        }
    }

    prj->importIntoTracks(localPaths, adjustedDstTrackIds, startTime);
}

bool DropController::hasMidiFiles() const
{
    return std::any_of(m_lastDraggedFilesInfo.begin(), m_lastDraggedFilesInfo.end(), [](const auto& info) { return info.midi; });
}

std::vector<bool> DropController::draggedMidiTypes(int count) const
{
    std::vector<bool> types;
    for (const auto& info : m_lastDraggedFilesInfo) {
        for (int i = 0; i < info.trackCount; ++i) {
            types.push_back(info.midi);
        }
    }
    types.resize(std::max(0, count), false);
    return types;
}

QVariantList DropController::midiAwareTrackIds(int currentTrackId, int count) const
{
    const auto project = globalContext()->currentTrackeditProject();
    if (!project || count <= 0) {
        return QVariantList();
    }
    const auto tracks = project->trackList();
    const auto types = draggedMidiTypes(count);
    size_t cursor = tracks.size();
    for (size_t i = 0; i < tracks.size(); ++i) {
        if (tracks[i].id == currentTrackId) {
            cursor = i;
        }
    }
    if (cursor == tracks.size() || (m_trackCountBeforeImport >= 0 && cursor >= static_cast<size_t>(m_trackCountBeforeImport))) {
        // Conditional rows are one preview group. Hovering a later row must
        // not rotate the parts or keep creating more preview tracks.
        cursor = std::min(tracks.size(), static_cast<size_t>(std::max(0, m_trackCountBeforeImport)));
    }
    std::set<trackedit::TrackId> used;
    size_t midiMinimum = 0;
    QVariantList result;
    for (bool midi : types) {
        if (!midi) {
            midiMinimum = 0;
        }
        const auto matches = [&](size_t i) {
            return !used.contains(tracks[i].id) && (midi ? tracks[i].type == trackedit::TrackType::Midi : isAudioTrack(tracks[i].type));
        };
        size_t found = cursor;
        while (found < tracks.size() && !matches(found)) {
            ++found;
        }
        if (found == tracks.size() && m_trackCountBeforeImport >= 0) {
            found = std::min(tracks.size(), std::max(midi ? midiMinimum : size_t(0), static_cast<size_t>(m_trackCountBeforeImport)));
            while (found < tracks.size() && !matches(found)) {
                ++found;
            }
        }
        if (found == tracks.size()) {
            result.push_back(static_cast<int>(trackedit::INVALID_TRACK));
        } else {
            used.insert(tracks[found].id);
            result.push_back(static_cast<int>(tracks[found].id));
            cursor = found + 1;
            if (midi) {
                midiMinimum = cursor;
            }
        }
    }
    return result;
}
