/*
* Audacity: A Digital Audio Editor
*/
#pragma once

#include "au3-automation-data/AutomationData.h"
#include "au3-registries/ClientData.h"

class ChannelGroup;

class PLAYABLE_TRACK_API TrackAutomation final : public ClientData::Cloneable<>
{
public:
    static constexpr double MaximumVolume = 3.9810717055349722;
    static AutomationData& Get(ChannelGroup& group);
    static const AutomationData& Get(const ChannelGroup& group);
    std::unique_ptr<ClientData::Cloneable<> > Clone() const override;

private:
    AutomationData mData;
};
