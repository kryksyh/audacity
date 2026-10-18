/*
* Audacity: A Digital Audio Editor
*/
#include "nativeinstrumentsessions.h"
#include "liveinstrumentsession.h"
#include <algorithm>
#include <set>
#include "au3-project/Project.h"
#include "au3-track/Track.h"

namespace au::au3audio {
namespace {
const AttachedProjectObjects::RegisteredFactory sessionsFactory {
    [](AudacityProject&) { return std::make_shared<NativeInstrumentSessions>(); }
};
}
NativeInstrumentSessions& NativeInstrumentSessions::Get(AudacityProject& project)
{
    return project.AttachedObjects::Get<NativeInstrumentSessions&>(sessionsFactory);
}

std::shared_ptr<LiveInstrumentSession> NativeInstrumentSessions::get(AudacityProject& project, const std::shared_ptr<MidiTrack>& track)
{
    std::erase_if(m_retired, [](const auto& session) { return session.use_count() == 1; });
    std::set<const MidiTrack*> present;
    for (const auto* current : TrackList::Get(project).Any<const MidiTrack>()) {
        present.insert(current);
    }
    for (auto it = m_sessions.begin(); it != m_sessions.end();) {
        if (!present.contains(it->first)) {
            m_retired.push_back(std::move(it->second));
            it = m_sessions.erase(it);
        } else {
            ++it;
        }
    }
    auto& session = m_sessions[track.get()];
    if (session && !session->matchesDocument()) {
        m_retired.push_back(std::move(session));
    }
    if (!session) {
        auto processor = createInstrumentProcessor(track->GetInstrumentDescription().utf8_string(),
                                                   track->GetInstrumentState().utf8_string());
        session = std::make_shared<LiveInstrumentSession>(track, std::move(processor));
        session->close();
    }
    return session;
}

std::shared_ptr<LiveInstrumentSession> NativeInstrumentSessions::find(const MidiTrack& track) const
{
    const auto found = m_sessions.find(&track);
    return found != m_sessions.end() && found->second && found->second->matchesDocument() ? found->second : nullptr;
}
}
