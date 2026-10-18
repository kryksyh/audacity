/*
* Audacity: A Digital Audio Editor
*/
#include "nativeofflinegraph.h"

#include <algorithm>
#include <cmath>
#include <deque>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>

namespace au::au3audio {
namespace {
using Stereo = std::array<std::vector<float>, 2>;
using Buffer = NativeOfflineGraph::Buffer;
Stereo storage(size_t frames) { return { std::vector<float>(frames), std::vector<float>(frames) }; }
Buffer pointers(Stereo& values) { return { values[0].data(), values[1].data() }; }
}

struct NativeOfflineGraph::Impl {
    struct Chunk {
        int64_t start;
        Stereo pre, post;
        size_t size() const { return pre[0].size(); }
    };
    struct State {
        Node node;
        std::deque<Chunk> cache;
        std::vector<size_t> incoming;
        std::vector<std::pair<size_t, int64_t> > readers;
        Stereo scratch;
        int64_t next;
    };
    struct Edge {
        size_t source, destination;
        double gain;
        bool pre;
    };

    Impl(std::vector<Node> nodes, size_t maximumFrames, int64_t first)
        : capacity(maximumFrames), next(first), scratch(storage(maximumFrames)), mapped(storage(maximumFrames))
    {
        if (!capacity) {
            throw std::invalid_argument("Invalid offline routing buffer size.");
        }
        std::map<std::string, size_t> buses;
        for (size_t i = 0; i < nodes.size(); ++i) {
            if (!nodes[i].routing.busId.empty() && !buses.emplace(nodes[i].routing.busId, i).second) {
                throw std::invalid_argument("Audio bus identifiers must be unique.");
            }
            states.push_back({ std::move(nodes[i]), {}, {}, {}, storage(capacity), first });
        }
        std::vector<size_t> incoming(states.size());
        const auto add = [&](size_t source, size_t destination, double gain, bool pre) {
            if (source == destination) {
                throw std::invalid_argument("An audio bus must not route to itself.");
            }
            const auto edge = edges.size();
            edges.push_back({ source, destination, gain, pre });
            states[source].readers.emplace_back(edge, first);
            if (destination < states.size()) {
                states[destination].incoming.push_back(edge);
                ++incoming[destination];
            } else {
                roots.push_back(edge);
            }
        };
        for (size_t source = 0; source < states.size(); ++source) {
            const auto& routing = states[source].node.routing;
            const auto output = buses.find(routing.output);
            add(source, output == buses.end() ? states.size() : output->second, 1.0, false);
            std::set<std::string> targets;
            for (const auto& send : routing.sends) {
                if (send.bus.empty() || !std::isfinite(send.gain) || send.gain < 0 || send.gain > 4 || !targets.insert(send.bus).second) {
                    throw std::invalid_argument("The audio routing graph contains an invalid send.");
                }
                const auto target = buses.find(send.bus);
                if (target != buses.end()) {
                    add(source, target->second, send.gain, send.preFader);
                }
            }
        }
        std::vector<size_t> order;
        for (size_t i = 0; i < states.size(); ++i) {
            if (!incoming[i]) {
                order.push_back(i);
            }
        }
        for (size_t i = 0; i < order.size(); ++i) {
            for (const auto& [reader, _] : states[order[i]].readers) {
                const auto destination = edges[reader].destination;
                if (destination < states.size() && !--incoming[destination]) {
                    order.push_back(destination);
                }
            }
        }
        if (order.size() != states.size()) {
            throw std::invalid_argument("Audio routes and sends must not form a feedback loop.");
        }
    }

    void input(size_t index, Buffer output, size_t frames, int64_t sample)
    {
        auto& state = states[index];
        for (size_t offset = 0; offset < frames;) {
            const auto count = std::min(capacity, frames - offset);
            const Buffer destination { output[0] + offset, output[1] + offset };
            const auto position = sample + static_cast<int64_t>(offset);
            for (auto* channel : destination) {
                std::fill_n(channel, count, 0.0f);
            }
            if (state.node.source) {
                state.node.source(destination, count, position);
            }
            for (const auto edgeIndex : state.incoming) {
                const auto& edge = edges[edgeIndex];
                read(edgeIndex, pointers(state.scratch), count, position);
                for (size_t channel = 0; channel < 2; ++channel) {
                    for (size_t frame = 0; frame < count; ++frame) {
                        destination[channel][frame] += state.scratch[channel][frame] * edge.gain;
                    }
                }
            }
            offset += count;
        }
    }

    void generate(size_t index, size_t frames)
    {
        auto& state = states[index];
        Chunk chunk { state.next, storage(frames), storage(frames) };
        const Processor upstream = [&](Buffer output, size_t count, int64_t sample) { input(index, output, count, sample); };
        if (state.node.effects) {
            state.node.effects(pointers(chunk.pre), frames, state.next, upstream);
        } else {
            upstream(pointers(chunk.pre), frames, state.next);
        }
        auto gain = storage(frames);
        for (auto& channel : gain) {
            std::fill(channel.begin(), channel.end(), 1.0f);
        }
        if (state.node.fader) {
            state.node.fader(gain[0], gain[1], state.next);
        }
        const bool silent = state.node.silent && state.node.silent();
        for (size_t channel = 0; channel < 2; ++channel) {
            for (size_t frame = 0; frame < frames; ++frame) {
                if (silent) {
                    chunk.pre[channel][frame] = 0;
                }
                chunk.post[channel][frame] = chunk.pre[channel][frame] * gain[channel][frame];
            }
        }
        state.next += static_cast<int64_t>(frames);
        state.cache.push_back(std::move(chunk));
    }

    void read(size_t edgeIndex, Buffer output, size_t frames, int64_t sample, const Buffer* pre = nullptr)
    {
        auto& state = states[edges[edgeIndex].source];
        auto reader = std::find_if(state.readers.begin(), state.readers.end(), [&](const auto& r) { return r.first == edgeIndex; });
        if (reader == state.readers.end() || reader->second != sample) {
            throw std::runtime_error("Offline route requests must advance continuously.");
        }
        if (frames > static_cast<size_t>(std::numeric_limits<int64_t>::max())
            || sample > std::numeric_limits<int64_t>::max() - static_cast<int64_t>(frames)) {
            throw std::out_of_range("Offline route position is out of range.");
        }
        size_t written = 0;
        while (written < frames) {
            while (sample >= state.next) {
                generate(edges[edgeIndex].source, std::min(capacity, frames - written));
            }
            const auto chunk = std::find_if(state.cache.begin(), state.cache.end(), [&](const auto& c) {
                return sample >= c.start && static_cast<uint64_t>(sample) - static_cast<uint64_t>(c.start) < c.size();
            });
            if (chunk == state.cache.end()) {
                throw std::runtime_error("Offline source audio was released before its route consumed it.");
            }
            const auto offset = static_cast<size_t>(sample - chunk->start);
            const auto count = std::min(frames - written, chunk->size() - offset);
            const auto& values = edges[edgeIndex].pre ? chunk->pre : chunk->post;
            for (size_t channel = 0; channel < 2; ++channel) {
                std::copy_n(values[channel].data() + offset, count, output[channel] + written);
                if (pre) {
                    std::copy_n(chunk->pre[channel].data() + offset, count, (*pre)[channel] + written);
                }
            }
            sample += static_cast<int64_t>(count);
            written += count;
            reader->second = sample;
            const auto oldest = std::min_element(state.readers.begin(), state.readers.end(), [](const auto& a, const auto& b) {
                return a.second < b.second;
            })->second;
            while (!state.cache.empty() && state.cache.front().start + static_cast<int64_t>(state.cache.front().size()) <= oldest) {
                state.cache.pop_front();
            }
        }
    }

    void process(Buffer output, size_t frames, int64_t sample)
    {
        if (sample != next || frames > static_cast<size_t>(std::numeric_limits<int64_t>::max())
            || sample > std::numeric_limits<int64_t>::max() - static_cast<int64_t>(frames)) {
            throw std::out_of_range("Offline output requests must advance continuously.");
        }
        for (auto* channel : output) {
            std::fill_n(channel, frames, 0.0f);
        }
        for (size_t offset = 0; offset < frames;) {
            const auto count = std::min(capacity, frames - offset);
            const auto position = sample + static_cast<int64_t>(offset);
            for (const auto edgeIndex : roots) {
                const auto& edge = edges[edgeIndex];
                const auto& node = states[edge.source].node;
                auto mappedBuffer = pointers(mapped);
                read(edgeIndex, pointers(scratch), count, position, node.mappedOutput ? &mappedBuffer : nullptr);
                for (size_t channel = 0; channel < 2; ++channel) {
                    for (size_t frame = 0; frame < count; ++frame) {
                        output[channel][offset + frame] += scratch[channel][frame] * edge.gain;
                        if (node.mappedOutput) {
                            mapped[channel][frame] *= edge.gain;
                        }
                    }
                }
                if (node.mappedOutput) {
                    node.mappedOutput(mappedBuffer, count, position);
                }
            }
            offset += count;
        }
        next += static_cast<int64_t>(frames);
    }

    size_t capacity;
    int64_t next;
    Stereo scratch, mapped;
    std::vector<State> states;
    std::vector<Edge> edges;
    std::vector<size_t> roots;
};

NativeOfflineGraph::NativeOfflineGraph(std::vector<Node> nodes, size_t maximumFrames, int64_t first)
    : m_impl(std::make_unique<Impl>(std::move(nodes), maximumFrames, first)) {}
NativeOfflineGraph::~NativeOfflineGraph() = default;
void NativeOfflineGraph::process(Buffer output, size_t frames, int64_t sample) { m_impl->process(output, frames, sample); }
}
