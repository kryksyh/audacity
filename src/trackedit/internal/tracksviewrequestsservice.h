/*
 * Audacity: A Digital Audio Editor
 */
#pragma once

#include "framework/global/modularity/ioc.h"
#include "framework/global/async/asyncable.h"

#include "../itracksviewrequestsservice.h"

namespace au::trackedit {
class TracksViewRequestsService : public ITracksViewRequestsService, public muse::async::Asyncable, public muse::Contextable
{
public:
    TracksViewRequestsService(const muse::modularity::ContextPtr& ctx)
        : muse::Contextable(ctx) {}

    void requestFinishInstrumentEditing() override;
    muse::async::Notification finishInstrumentEditingRequested() const override;
    void requestFlushInstrumentChanges() override;
    muse::async::Notification flushInstrumentChangesRequested() const override;

    void requestRouting(TrackId trackId) override;
    muse::async::Channel<TrackId> routingRequested() const override;

    void requestAutomation(TrackId trackId) override;
    muse::async::Channel<TrackId> automationRequested() const override;

    void requestPianoRoll(TrackId trackId, int64_t clipId) override;
    muse::async::Channel<TrackId, int64_t> pianoRollRequested() const override;

    void requestItemMove(secs_t timeOffset, int trackOffset) override;
    muse::async::Channel<secs_t, int> itemMoveRequested() const override;

    void requestLabelTitleEdit(const LabelKey& labelKey) override;
    std::optional<LabelKey> pendingLabelTitleEdit() const override;
    void labelTitleEditRequestHandled(const LabelKey& labelKey) override;
    muse::async::Channel<LabelKey> labelTitleEditRequested() const override;

    void requestClipTitleEdit(const ClipKey& clipKey) override;
    std::optional<ClipKey> pendingClipTitleEdit() const override;
    void clipTitleEditRequestHandled(const ClipKey& clipKey) override;
    muse::async::Channel<ClipKey> clipTitleEditRequested() const override;

private:
    muse::async::Notification m_finishInstrumentEditingRequested;
    muse::async::Notification m_flushInstrumentChangesRequested;
    muse::async::Channel<TrackId> m_routingRequested;
    muse::async::Channel<TrackId> m_automationRequested;
    muse::async::Channel<TrackId, int64_t> m_pianoRollRequested;
    muse::async::Channel<secs_t, int> m_itemMoveRequested;
    std::optional<LabelKey> m_pendingLabelTitleEdit;
    muse::async::Channel<LabelKey> m_labelTitleEditRequested;
    std::optional<ClipKey> m_pendingClipTitleEdit;
    muse::async::Channel<ClipKey> m_clipTitleEditRequested;
};
}
