/*
* Audacity: A Digital Audio Editor
*/
#pragma once

#include <string>
#include <vector>
#include "au3-registries/ClientData.h"
#include "au3-xml/XMLTagHandler.h"

class ChannelGroup;
class TrackList;
class PlayableTrack;
class XMLWriter;

struct TrackSendData {
    std::string bus;
    double gain = 1.0;
    bool preFader = false;
    bool operator==(const TrackSendData&) const = default;
};

class PLAYABLE_TRACK_API TrackRouting final : public ClientData::Cloneable<>, public XMLTagHandler
{
public:
    static constexpr auto XMLTag = "routing";
    static TrackRouting& Get(ChannelGroup& group);
    static const TrackRouting& Get(const ChannelGroup& group);
    static bool HasDirectOutput(const TrackList& tracks, const PlayableTrack& source);
    enum class Audibility {
        Apply, Ignore
    };
    static std::vector<const PlayableTrack*> AudibleSources(const TrackList& tracks, bool selectedOnly,
                                                            Audibility audibility = Audibility::Apply);
    static std::vector<const PlayableTrack*> AudibleSources(const std::vector<const PlayableTrack*>& sources, bool selectedOnly,
                                                            Audibility audibility = Audibility::Apply);
    static bool Validate(const TrackList& tracks, const ChannelGroup* source = nullptr, const TrackRouting* replacement = nullptr);
    std::unique_ptr<ClientData::Cloneable<>> Clone() const override;
    bool HandleXMLTag(const std::string_view& tag, const AttributesList& attrs) override;
    XMLTagHandler* HandleXMLChild(const std::string_view& tag) override;
    void WriteXML(XMLWriter& writer) const;

    std::string busId;
    std::string output;
    std::vector<TrackSendData> sends;
};
