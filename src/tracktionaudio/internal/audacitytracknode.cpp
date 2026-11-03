/*
* Audacity: A Digital Audio Editor
*/
#include <tracktion_engine/tracktion_engine.h>
#include <tracktion_graph/tracktion_graph.h>
#include <tracktion_engine/playback/graph/tracktion_TracktionEngineNode.h>

#include "audacitytracknode.h"

#include "audacitytrackstream.h"

using namespace au::tracktionaudio;

namespace {
class AudacityTrackNode final : public tracktion::graph::Node
{
public:
    AudacityTrackNode(std::shared_ptr<AudacityTrackStream> stream, tracktion::ProcessState& state, size_t id)
        : m_stream(std::move(stream)), m_state(state), m_id(id)
    {
    }

    tracktion::graph::NodeProperties getNodeProperties() override
    {
        tracktion::graph::NodeProperties properties;
        properties.hasAudio = true;
        properties.hasMidi = false;
        properties.numberOfChannels = static_cast<int>(m_stream->channelCount());
        properties.nodeID = m_id;
        return properties;
    }

    bool isReadyToProcess() override { return true; }

    void prepareToPlay(const tracktion::graph::PlaybackInitialisationInfo& info) override
    {
        m_stream->prepare(info.sampleRate);
    }

    void process(ProcessContext& context) override
    {
        auto audio = tracktion::graph::toAudioBuffer(context.buffers.audio);
        if (m_state.playHeadState.playHead.isStopped()) {
            audio.clear();
            return;
        }
        m_stream->read(m_state.timelineSampleRange.getStart(), audio.getArrayOfWritePointers(), audio.getNumSamples());
    }

private:
    const std::shared_ptr<AudacityTrackStream> m_stream;
    tracktion::ProcessState& m_state;
    const size_t m_id;
};
}

void AudacityTrackStreams::add(const tracktion::AudioTrack& track, std::shared_ptr<AudacityTrackStream> stream)
{
    const std::lock_guard lock(m_mutex);
    m_streams[&track] = std::move(stream);
}

void AudacityTrackStreams::remove(const tracktion::AudioTrack& track)
{
    const std::lock_guard lock(m_mutex);
    m_streams.erase(&track);
}

std::unique_ptr<tracktion::graph::Node> AudacityTrackStreams::createNode(tracktion::AudioTrack& track,
                                                                         tracktion::ProcessState& state) const
{
    const std::lock_guard lock(m_mutex);
    const auto found = m_streams.find(&track);
    if (found == m_streams.end()) {
        return {};
    }
    return std::make_unique<AudacityTrackNode>(found->second, state, static_cast<size_t>(track.itemID.getRawID()));
}
