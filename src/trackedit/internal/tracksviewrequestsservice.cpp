/*
 * Audacity: A Digital Audio Editor
 */
#include "tracksviewrequestsservice.h"

using namespace au::trackedit;

void TracksViewRequestsService::requestItemMove(secs_t timeOffset, int trackOffset)
{
    m_itemMoveRequested.send(timeOffset, trackOffset);
}

muse::async::Channel<secs_t, int> TracksViewRequestsService::itemMoveRequested() const
{
    return m_itemMoveRequested;
}

void TracksViewRequestsService::requestLabelTitleEdit(const LabelKey& labelKey)
{
    m_pendingLabelTitleEdit = labelKey;
    m_labelTitleEditRequested.send(labelKey);
}

std::optional<LabelKey> TracksViewRequestsService::pendingLabelTitleEdit() const
{
    return m_pendingLabelTitleEdit;
}

void TracksViewRequestsService::labelTitleEditRequestHandled(const LabelKey& labelKey)
{
    if (m_pendingLabelTitleEdit == labelKey) {
        m_pendingLabelTitleEdit.reset();
    }
}

muse::async::Channel<LabelKey> TracksViewRequestsService::labelTitleEditRequested() const
{
    return m_labelTitleEditRequested;
}

void TracksViewRequestsService::requestClipTitleEdit(const ClipKey& clipKey)
{
    m_pendingClipTitleEdit = clipKey;
    m_clipTitleEditRequested.send(clipKey);
}

std::optional<ClipKey> TracksViewRequestsService::pendingClipTitleEdit() const
{
    return m_pendingClipTitleEdit;
}

void TracksViewRequestsService::clipTitleEditRequestHandled(const ClipKey& clipKey)
{
    if (m_pendingClipTitleEdit == clipKey) {
        m_pendingClipTitleEdit.reset();
    }
}

muse::async::Channel<ClipKey> TracksViewRequestsService::clipTitleEditRequested() const
{
    return m_clipTitleEditRequested;
}

void TracksViewRequestsService::requestPianoRoll(TrackId trackId, int64_t clipId)
{
    m_pianoRollRequested.send(trackId, clipId);
}

muse::async::Channel<TrackId, int64_t> TracksViewRequestsService::pianoRollRequested() const
{
    return m_pianoRollRequested;
}

void TracksViewRequestsService::requestAutomation(TrackId trackId)
{
    m_automationRequested.send(trackId);
}

muse::async::Channel<TrackId> TracksViewRequestsService::automationRequested() const
{
    return m_automationRequested;
}

void TracksViewRequestsService::requestRouting(TrackId trackId)
{
    m_routingRequested.send(trackId);
}

muse::async::Channel<TrackId> TracksViewRequestsService::routingRequested() const
{
    return m_routingRequested;
}

void TracksViewRequestsService::requestFinishInstrumentEditing()
{
    m_finishInstrumentEditingRequested.notify();
}

muse::async::Notification TracksViewRequestsService::finishInstrumentEditingRequested() const
{
    return m_finishInstrumentEditingRequested;
}

void TracksViewRequestsService::requestFlushInstrumentChanges()
{
    m_flushInstrumentChangesRequested.notify();
}

muse::async::Notification TracksViewRequestsService::flushInstrumentChangesRequested() const
{
    return m_flushInstrumentChangesRequested;
}
