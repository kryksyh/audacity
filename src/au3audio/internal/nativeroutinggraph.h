/*
* Audacity: A Digital Audio Editor
*/
#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "au3-playable-track/TrackRouting.h"

namespace au::au3audio {
// Processing shared by the native transport and offline renderer. Effects run
// after the sum of incoming routes and before pre-fader sends. Every path is
// delayed to match the longest path to its destination.
class NativeRoutingGraph final
{
public:
    using Buffer = std::array<float*, 2>;
    using Processor = std::function<void (Buffer, size_t, int64_t)>;
    using Fader = std::function<void (std::span<float>, std::span<float>, int64_t)>;
    struct Node {
        TrackRouting routing;
        Processor source;
        Processor effects;
        Fader fader;
        std::function<bool()> silent;
        std::function<void(int64_t)> reset;
        size_t sourceLatency = 0;
        size_t effectLatency = 0;
        //! Aligned post-insert/pre-fader stem, for an explicit export matrix.
        Processor mappedOutput;
        //! Post-fader meter, aligned to the graph output on every track/bus.
        Processor meterOutput;
    };

    NativeRoutingGraph(std::vector<Node> nodes, size_t maximumFrames);
    ~NativeRoutingGraph();
    NativeRoutingGraph(const NativeRoutingGraph&) = delete;
    NativeRoutingGraph& operator=(const NativeRoutingGraph&) = delete;

    void process(Buffer output, size_t frames, int64_t sample);
    void reset(int64_t sample);
    size_t latency() const;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};
}
