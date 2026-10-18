/*
* Audacity: A Digital Audio Editor
*/
#pragma once

#include "nativeroutinggraph.h"

namespace au::au3audio {
// Offline effects can ask for future input when their delay changes. Shared
// upstream stems are cached until every route has consumed their content, so
// fan-out never replays a source or native instrument.
class NativeOfflineGraph final
{
public:
    using Buffer = NativeRoutingGraph::Buffer;
    using Processor = NativeRoutingGraph::Processor;
    struct Node {
        TrackRouting routing;
        Processor source;
        std::function<void(Buffer, size_t, int64_t, const Processor&)> effects;
        NativeRoutingGraph::Fader fader;
        std::function<bool()> silent;
        Processor mappedOutput;
    };
    NativeOfflineGraph(std::vector<Node> nodes, size_t maximumFrames, int64_t first);
    ~NativeOfflineGraph();
    void process(Buffer output, size_t frames, int64_t sample);
private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};
}
