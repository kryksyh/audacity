/*
* Audacity: A Digital Audio Editor
*/
#include "TrackRouting.h"

#include <cmath>
#include <functional>
#include <map>
#include <set>
#include "PlayableTrack.h"
#include "au3-channel/Channel.h"
#include "au3-track/Track.h"
#include "au3-xml/XMLWriter.h"

namespace {
const ChannelGroup::Attachments::RegisteredFactory routingFactory {
    [](auto&) { return std::make_unique<TrackRouting>(); }
};
}

TrackRouting& TrackRouting::Get(ChannelGroup& group)
{
    return group.Attachments::Get<TrackRouting>(routingFactory);
}

const TrackRouting& TrackRouting::Get(const ChannelGroup& group)
{
    return Get(const_cast<ChannelGroup&>(group));
}

std::unique_ptr<ClientData::Cloneable<> > TrackRouting::Clone() const
{
    return std::make_unique<TrackRouting>(*this);
}

bool TrackRouting::Validate(const TrackList& tracks, const ChannelGroup* source, const TrackRouting* replacement)
{
    std::map<std::string, const TrackRouting*> buses;
    std::vector<const TrackRouting*> routes;
    for (const auto* track : tracks.Any<const PlayableTrack>()) {
        const auto& route = track == source && replacement ? *replacement : Get(*track);
        routes.push_back(&route);
        if (!route.busId.empty() && !buses.emplace(route.busId, &route).second) {
            return false;
        }
        std::set<std::string> targets;
        for (const auto& send : route.sends) {
            if (send.bus.empty() || !std::isfinite(send.gain) || send.gain < 0.0 || send.gain > 4.0
                || !targets.insert(send.bus).second) {
                return false;
            }
        }
    }
    std::map<const TrackRouting*, int> state;
    std::function<bool(const TrackRouting*)> visit = [&](const TrackRouting* route) {
        auto& mark = state[route];
        if (mark == 1) {
            return false;
        }
        if (mark == 2) {
            return true;
        }
        mark = 1;
        const auto follow = [&](const std::string& id) {
            const auto bus = buses.find(id);
            return bus == buses.end() || visit(bus->second);
        };
        if (!follow(route->output)) {
            return false;
        }
        for (const auto& send : route->sends) {
            if (!follow(send.bus)) {
                return false;
            }
        }
        mark = 2;
        return true;
    };
    for (const auto* route : routes) {
        if (!visit(route)) {
            return false;
        }
    }
    return true;
}

bool TrackRouting::HandleXMLTag(const std::string_view& tag, const AttributesList& attrs)
{
    if (tag == XMLTag) {
        busId.clear();
        output.clear();
        sends.clear();
        for (const auto& [name, value] : attrs) {
            if (name == "bus") {
                busId = value.ToWString().utf8_string();
            } else if (name == "output") {
                output = value.ToWString().utf8_string();
            }
        }
        return true;
    }
    if (tag != "send") {
        return false;
    }
    TrackSendData send;
    for (const auto& [name, value] : attrs) {
        if (name == "bus") {
            send.bus = value.ToWString().utf8_string();
        } else if (name == "gain" && !value.TryGet(send.gain)) {
            return false;
        } else if (name == "prefader" && !value.TryGet(send.preFader)) {
            return false;
        }
    }
    if (send.bus.empty() || !std::isfinite(send.gain) || send.gain < 0.0 || send.gain > 4.0) {
        return false;
    }
    for (const auto& existing : sends) {
        if (existing.bus == send.bus) {
            return false;
        }
    }
    sends.push_back(std::move(send));
    return true;
}

XMLTagHandler* TrackRouting::HandleXMLChild(const std::string_view& tag)
{
    return tag == "send" ? this : nullptr;
}

void TrackRouting::WriteXML(XMLWriter& writer) const
{
    if (busId.empty() && output.empty() && sends.empty()) {
        return;
    }
    writer.StartTag(XMLTag);
    writer.WriteAttr("bus", wxString::FromUTF8(busId));
    writer.WriteAttr("output", wxString::FromUTF8(output));
    for (const auto& send : sends) {
        writer.StartTag("send");
        writer.WriteAttr("bus", wxString::FromUTF8(send.bus));
        writer.WriteAttr("gain", send.gain, 17);
        writer.WriteAttr("prefader", send.preFader);
        writer.EndTag("send");
    }
    writer.EndTag(XMLTag);
}

bool TrackRouting::HasDirectOutput(const TrackList& tracks, const PlayableTrack& source)
{
    const auto& output = Get(source).output;
    if (output.empty()) {
        return true;
    }
    for (const auto* track : tracks.Any<const PlayableTrack>()) {
        if (Get(*track).busId == output) {
            return false;
        }
    }
    return true;
}

std::vector<const PlayableTrack*> TrackRouting::AudibleSources(const TrackList& tracks, bool selectedOnly, Audibility audibility)
{
    std::vector<const PlayableTrack*> sources;
    for (const auto* track : tracks.Any<const PlayableTrack>()) {
        sources.push_back(track);
    }
    return AudibleSources(sources, selectedOnly, audibility);
}

std::vector<const PlayableTrack*> TrackRouting::AudibleSources(const std::vector<const PlayableTrack*>& sources, bool selectedOnly,
                                                               Audibility audibility)
{
    std::map<std::string, const PlayableTrack*> buses;
    bool anySolo = false;
    for (const auto* track : sources) {
        const auto& route = Get(*track);
        if (!route.busId.empty()) {
            buses.emplace(route.busId, track);
        }
        anySolo |= track->GetSolo();
    }
    std::map<const PlayableTrack*, std::set<const PlayableTrack*> > downstream;
    for (const auto* source : sources) {
        auto& reachable = downstream[source];
        std::function<void(const PlayableTrack*)> visit = [&](const auto* track) {
            if (!reachable.insert(track).second) {
                return;
            }
            const auto follow = [&](const std::string& id) {
                const auto bus = buses.find(id);
                if (bus != buses.end()) {
                    visit(bus->second);
                }
            };
            const auto& route = Get(*track);
            follow(route.output);
            for (const auto& send : route.sends) {
                follow(send.bus);
            }
        };
        visit(source);
    }
    std::vector<const PlayableTrack*> result;
    for (const auto* source : sources) {
        bool selected = !selectedOnly || source->IsSelected();
        bool solo = !anySolo;
        for (const auto* candidate : downstream[source]) {
            selected |= selectedOnly && !Get(*candidate).busId.empty() && candidate->IsSelected();
            solo |= candidate->GetSolo();
        }
        for (const auto* candidate : sources) {
            solo |= candidate->GetSolo() && downstream[candidate].contains(source);
            selected |= selectedOnly && candidate->IsSelected() && downstream[candidate].contains(source);
        }
        if (selected && (audibility == Audibility::Ignore || (solo && (!source->GetMute() || source->GetSolo())))) {
            result.push_back(source);
        }
    }
    return result;
}
