/*
* Audacity: A Digital Audio Editor
*/
#include "TrackAutomation.h"
#include "au3-channel/Channel.h"

namespace {
const ChannelGroup::Attachments::RegisteredFactory trackAutomationFactory {
    [](auto&) { return std::make_unique<TrackAutomation>(); }
};
}

AutomationData& TrackAutomation::Get(ChannelGroup& group)
{
    return group.Attachments::Get<TrackAutomation>(trackAutomationFactory).mData;
}

const AutomationData& TrackAutomation::Get(const ChannelGroup& group)
{
    return Get(const_cast<ChannelGroup&>(group));
}

std::unique_ptr<ClientData::Cloneable<> > TrackAutomation::Clone() const
{
    return std::make_unique<TrackAutomation>(*this);
}
