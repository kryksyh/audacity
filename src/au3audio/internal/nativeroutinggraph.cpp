/*
* Audacity: A Digital Audio Editor
*/
#include "nativeroutinggraph.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>

namespace au::au3audio {
namespace {
using Stereo = std::array<std::vector<float>, 2>;
Stereo storage(size_t frames) { return { std::vector<float>(frames), std::vector<float>(frames) }; }
NativeRoutingGraph::Buffer pointers(Stereo& data) { return { data[0].data(), data[1].data() }; }

class Delay final
{
public:
    void prepare(size_t frames)
    {
        m_data = storage(frames);
        m_position = 0;
    }

    void reset()
    {
        for (auto& channel : m_data) {
            std::fill(channel.begin(), channel.end(), 0.0f);
        }
        m_position = 0;
    }

    void add(const Stereo& input, NativeRoutingGraph::Buffer output, size_t frames, float gain = 1.0f)
    {
        if (m_data[0].empty()) {
            for (size_t channel = 0; channel < 2; ++channel) {
                for (size_t frame = 0; frame < frames; ++frame) {
                    output[channel][frame] += input[channel][frame] * gain;
                }
            }
            return;
        }
        for (size_t frame = 0; frame < frames; ++frame) {
            for (size_t channel = 0; channel < 2; ++channel) {
                output[channel][frame] += m_data[channel][m_position] * gain;
                m_data[channel][m_position] = input[channel][frame];
            }
            if (++m_position == m_data[0].size()) {
                m_position = 0;
            }
        }
    }

private:
    Stereo m_data;
    size_t m_position = 0;
};
}

struct NativeRoutingGraph::Impl {
    struct State {
        Node node;
        Stereo input, source, pre, post, gain;
        Delay sourceDelay;
        size_t inputLatency = 0, outputLatency = 0;
        std::vector<size_t> edges;
        Delay meterDelay;
        Stereo meter;
    };
    struct Edge {
        size_t source, destination;
        bool preFader;
        float gain;
        Delay delay;
        Delay mappedDelay;
        Stereo mapped;
    };

    Impl(std::vector<Node> nodes, size_t capacity)
        : maximumFrames(capacity)
    {
        if (!capacity) {
            throw std::invalid_argument("An audio routing graph needs a nonzero block size.");
        }
        states.reserve(nodes.size());
        std::map<std::string, size_t> buses;
        for (size_t i = 0; i < nodes.size(); ++i) {
            if (!nodes[i].routing.busId.empty() && !buses.emplace(nodes[i].routing.busId, i).second) {
                throw std::invalid_argument("The audio routing graph contains duplicate bus IDs.");
            }
            states.push_back({ std::move(nodes[i]), storage(capacity), storage(capacity), storage(capacity),
                               storage(capacity), storage(capacity), {}, 0, 0, {}, {}, {} });
        }
        const size_t master = states.size();
        std::vector<size_t> incoming(states.size(), 0);
        auto addEdge = [&](size_t source, size_t destination, float gain, bool preFader) {
            states[source].edges.push_back(edges.size());
            edges.push_back({ source, destination, preFader, gain, {}, {}, {} });
            if (destination != master) {
                ++incoming[destination];
            }
        };
        for (size_t i = 0; i < states.size(); ++i) {
            const auto& routing = states[i].node.routing;
            const auto output = buses.find(routing.output);
            addEdge(i, output == buses.end() ? master : output->second, 1.0f, false);
            std::set<std::string> targets;
            for (const auto& send : routing.sends) {
                if (send.bus.empty() || !std::isfinite(send.gain) || send.gain < 0.0 || send.gain > 4.0
                    || !targets.insert(send.bus).second) {
                    throw std::invalid_argument("The audio routing graph contains an invalid send.");
                }
                const auto target = buses.find(send.bus);
                if (target != buses.end()) {
                    addEdge(i, target->second, send.gain, send.preFader);
                }
            }
        }
        for (size_t i = 0; i < incoming.size(); ++i) {
            if (!incoming[i]) {
                order.push_back(i);
            }
        }
        for (size_t i = 0; i < order.size(); ++i) {
            for (auto edge : states[order[i]].edges) {
                const auto destination = edges[edge].destination;
                if (destination != master && !--incoming[destination]) {
                    order.push_back(destination);
                }
            }
        }
        if (order.size() != states.size()) {
            throw std::invalid_argument("Audio routes and sends must not form a feedback loop.");
        }
        for (auto index : order) {
            auto& state = states[index];
            state.inputLatency = std::max(state.inputLatency, state.node.sourceLatency);
            state.sourceDelay.prepare(state.inputLatency - state.node.sourceLatency);
            if (state.node.effectLatency > std::numeric_limits<size_t>::max() - state.inputLatency) {
                throw std::invalid_argument("The audio routing latency is out of range.");
            }
            state.outputLatency = state.inputLatency + state.node.effectLatency;
            for (auto edge : state.edges) {
                const auto destination = edges[edge].destination;
                if (destination == master) {
                    outputLatency = std::max(outputLatency, state.outputLatency);
                } else {
                    states[destination].inputLatency = std::max(states[destination].inputLatency, state.outputLatency);
                }
            }
        }
        for (auto& state : states) {
            if (state.node.meterOutput) {
                state.meterDelay.prepare(outputLatency - state.outputLatency);
                state.meter = storage(maximumFrames);
            }
        }
        for (auto& edge : edges) {
            const auto targetLatency = edge.destination == master ? outputLatency : states[edge.destination].inputLatency;
            edge.delay.prepare(targetLatency - states[edge.source].outputLatency);
            if (edge.destination == master && states[edge.source].node.mappedOutput) {
                edge.mappedDelay.prepare(targetLatency - states[edge.source].outputLatency);
                edge.mapped = storage(maximumFrames);
            }
        }
    }

    void reset(int64_t sample)
    {
        for (auto& state : states) {
            state.sourceDelay.reset();
            state.meterDelay.reset();
            if (state.node.reset) {
                state.node.reset(sample);
            }
        }
        for (auto& edge : edges) {
            edge.delay.reset();
            edge.mappedDelay.reset();
        }
        nextSample = sample;
    }

    void process(Buffer output, size_t frames, int64_t sample)
    {
        if (frames > maximumFrames || !output[0] || !output[1]) {
            throw std::invalid_argument("Invalid native audio graph output buffers.");
        }
        if (sample != nextSample) {
            reset(sample);
        }
        for (auto* channel : output) {
            std::fill_n(channel, frames, 0.0f);
        }
        for (auto& state : states) {
            for (auto& channel : state.input) {
                std::fill_n(channel.data(), frames, 0.0f);
            }
        }
        for (auto index : order) {
            auto& state = states[index];
            for (auto& channel : state.source) {
                std::fill_n(channel.data(), frames, 0.0f);
            }
            if (state.node.source) {
                state.node.source(pointers(state.source), frames, sample);
            }
            state.sourceDelay.add(state.source, pointers(state.input), frames);
            for (size_t channel = 0; channel < 2; ++channel) {
                std::copy_n(state.input[channel].data(), frames, state.pre[channel].data());
            }
            if (state.node.effects) {
                state.node.effects(pointers(state.pre), frames, sample - static_cast<int64_t>(state.inputLatency));
            }
            for (auto& gain : state.gain) {
                std::fill_n(gain.data(), frames, 1.0f);
            }
            if (state.node.fader) {
                state.node.fader({ state.gain[0].data(), frames }, { state.gain[1].data(), frames },
                                 sample - static_cast<int64_t>(state.outputLatency));
            }
            const bool silent = state.node.silent && state.node.silent();
            for (size_t channel = 0; channel < 2; ++channel) {
                for (size_t frame = 0; frame < frames; ++frame) {
                    if (silent) {
                        state.pre[channel][frame] = 0.0f;
                    }
                    state.post[channel][frame] = state.pre[channel][frame] * state.gain[channel][frame];
                }
            }
            if (state.node.meterOutput) {
                for (auto& channel : state.meter) {
                    std::fill_n(channel.data(), frames, 0.0f);
                }
                state.meterDelay.add(state.post, pointers(state.meter), frames);
                state.node.meterOutput(pointers(state.meter), frames, sample - static_cast<int64_t>(outputLatency));
            }
            for (auto index : state.edges) {
                auto& edge = edges[index];
                auto destination = edge.destination == states.size() ? output : pointers(states[edge.destination].input);
                edge.delay.add(edge.preFader ? state.pre : state.post, destination, frames, edge.gain);
                if (edge.destination == states.size() && state.node.mappedOutput) {
                    for (auto& channel : edge.mapped) {
                        std::fill_n(channel.data(), frames, 0.0f);
                    }
                    edge.mappedDelay.add(state.pre, pointers(edge.mapped), frames, edge.gain);
                    state.node.mappedOutput(pointers(edge.mapped), frames, sample - static_cast<int64_t>(outputLatency));
                }
            }
        }
        nextSample = sample + static_cast<int64_t>(frames);
    }

    size_t maximumFrames, outputLatency = 0;
    int64_t nextSample = std::numeric_limits<int64_t>::min();
    std::vector<State> states;
    std::vector<Edge> edges;
    std::vector<size_t> order;
};

NativeRoutingGraph::NativeRoutingGraph(std::vector<Node> nodes, size_t maximumFrames)
    : m_impl(std::make_unique<Impl>(std::move(nodes), maximumFrames)) {}
NativeRoutingGraph::~NativeRoutingGraph() = default;
void NativeRoutingGraph::process(Buffer output, size_t frames, int64_t sample) { m_impl->process(output, frames, sample); }
void NativeRoutingGraph::reset(int64_t sample) { m_impl->reset(sample); }
size_t NativeRoutingGraph::latency() const { return m_impl->outputLatency; }
}
