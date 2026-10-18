/*
 * Audacity: A Digital Audio Editor
 */
#pragma once

#include "au3-registries/ClientData.h"
#include "RealtimeEffectList.h"
#include <memory>

class AudacityProject;
class RealtimeEffectList;

class SavedMasterEffectList final : public ClientData::Base
{
public:
    SavedMasterEffectList(AudacityProject& project);

    static SavedMasterEffectList& Get(AudacityProject& project);
    static const SavedMasterEffectList& Get(const AudacityProject& project);

    void UpdateCopy();
    // Temporarily serialize another snapshot while retaining the original
    // saved-document snapshot for close/compaction during Save Copy or Save As.
    std::unique_ptr<RealtimeEffectList> SwapCopy(std::unique_ptr<RealtimeEffectList> copy) noexcept;
    RealtimeEffectList& List();
    const RealtimeEffectList& List() const;

private:

    AudacityProject& m_project;
    std::unique_ptr<RealtimeEffectList> mLastSavedList;
};
