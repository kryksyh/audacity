/*
* Audacity: A Digital Audio Editor
*/
#pragma once
#include <map>
#include <memory>
#include <vector>
#include "au3-registries/ClientData.h"

class AudacityProject;
class MidiTrack;
namespace au::au3audio {
class LiveInstrumentSession;

// Main-thread cache, owned by the document. Keeps native editors and playback
// on the same instrument and disposes of retired plug-ins on the main thread.
class NativeInstrumentSessions final : public ClientData::Base
{
public:
    static NativeInstrumentSessions& Get(AudacityProject&);
    std::shared_ptr<LiveInstrumentSession> find(const MidiTrack&) const;
    std::shared_ptr<LiveInstrumentSession> get(AudacityProject&, const std::shared_ptr<MidiTrack>&);
private:
    std::map<const MidiTrack*, std::shared_ptr<LiveInstrumentSession> > m_sessions;
    std::vector<std::shared_ptr<LiveInstrumentSession> > m_retired;
};
}
