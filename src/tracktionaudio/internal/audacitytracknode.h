/*
* Audacity: A Digital Audio Editor
*/
#pragma once

#include <map>
#include <memory>
#include <mutex>

namespace tracktion {
inline namespace engine {
class AudioTrack;
struct ProcessState;
}
namespace graph {
class Node;
}
}

namespace au::tracktionaudio {
class AudacityTrackStream;

//! The streams of the tracks being played. They live here rather than in the
//! nodes because Tracktion replaces the nodes whenever it rebuilds its graph.
class AudacityTrackStreams
{
public:
    void add(const tracktion::AudioTrack& track, std::shared_ptr<AudacityTrackStream> stream);
    void remove(const tracktion::AudioTrack& track);

    //! @return null if the audio of this track doesn't come from a stream
    std::unique_ptr<tracktion::graph::Node> createNode(tracktion::AudioTrack& track, tracktion::ProcessState& state) const;

private:
    mutable std::mutex m_mutex;
    std::map<const tracktion::AudioTrack*, std::shared_ptr<AudacityTrackStream> > m_streams;
};
}
