/*
* Audacity: A Digital Audio Editor
*/
#include <gtest/gtest.h>
#include <algorithm>
#include <atomic>
#include <deque>
#include "../internal/nativeroutinggraph.h"

namespace au::au3audio {
namespace {
using Node = NativeRoutingGraph::Node;
using Buffer = NativeRoutingGraph::Buffer;

Node constant(float value)
{
    Node node;
    node.source = [value](Buffer output, size_t frames, int64_t) {
        for (auto* channel : output) {
            std::fill_n(channel, frames, value);
        }
    };
    return node;
}

void gain(Node& node, float value)
{
    node.fader = [value](std::span<float> left, std::span<float> right, int64_t) {
        std::fill(left.begin(), left.end(), value);
        std::fill(right.begin(), right.end(), value);
    };
}

std::vector<float> render(NativeRoutingGraph& graph, size_t frames, int64_t sample = 0)
{
    std::vector<float> left(frames), right(frames);
    graph.process({ left.data(), right.data() }, frames, sample);
    EXPECT_EQ(left, right);
    return left;
}

void delayEffect(Node& node, size_t latency, float scale = 1.0f)
{
    auto queues = std::make_shared<std::array<std::deque<float>, 2> >();
    auto clear = [queues, latency](int64_t) {
        for (auto& channel : *queues) {
            channel.assign(latency, 0.0f);
        }
    };
    node.effectLatency = latency;
    node.reset = clear;
    node.effects = [queues, scale](Buffer output, size_t frames, int64_t) {
        for (size_t channel = 0; channel < 2; ++channel) {
            auto& queue = (*queues)[channel];
            for (size_t i = 0; i < frames; ++i) {
                queue.push_back(output[channel][i] * scale);
                output[channel][i] = queue.front();
                queue.pop_front();
            }
        }
    };
}
}

TEST(NativeRoutingGraphTests, BusEffectsRunOnTheSumBeforeTheBusFader)
{
    auto first = constant(0.2f), second = constant(0.3f);
    first.routing.output = second.routing.output = "bus";
    Node bus;
    bus.routing.busId = "bus";
    // A nonlinear insert distinguishes processing the sum from processing each source.
    bus.effects = [](Buffer output, size_t frames, int64_t) {
        for (auto* channel : output) {
            for (size_t i = 0; i < frames; ++i) {
                channel[i] *= channel[i];
            }
        }
    };
    gain(bus, 0.5f);
    NativeRoutingGraph graph({ bus, first, second }, 64);
    for (auto sample : render(graph, 64)) {
        EXPECT_FLOAT_EQ(sample, 0.125f);
    }
}

TEST(NativeRoutingGraphTests, SendsChooseAudioBeforeOrAfterTheSourceFader)
{
    auto first = constant(1.0f), second = constant(1.0f);
    first.routing.output = "main-bus";
    second.routing.output = "main-bus";
    first.routing.sends = { { "send-bus", 0.25, true } };
    second.routing.sends = { { "send-bus", 0.25, false } };
    gain(first, 0.5f);
    gain(second, 0.5f);
    first.effects = [](Buffer output, size_t frames, int64_t) {
        for (auto* channel : output) {
            for (size_t i = 0; i < frames; ++i) {
                channel[i] *= 2.0f;
            }
        }
    };
    Node mainBus, sendBus;
    mainBus.routing.busId = "main-bus";
    sendBus.routing.busId = "send-bus";
    mainBus.effects = [](Buffer output, size_t frames, int64_t) {
        for (auto* channel : output) {
            for (size_t i = 0; i < frames; ++i) {
                channel[i] *= 2.0f;
            }
        }
    };
    gain(mainBus, 0.5f);
    NativeRoutingGraph graph({ sendBus, mainBus, first, second }, 64);
    for (auto sample : render(graph, 64)) {
        EXPECT_FLOAT_EQ(sample, 2.125f);
    }
}

TEST(NativeRoutingGraphTests, AudioOnABusIsDelayedToMatchItsIncomingRoutes)
{
    auto source = constant(0.0f);
    source.routing.output = "bus";
    source.source = [](Buffer output, size_t, int64_t) {
        output[0][0] = output[1][0] = 1.0f;
    };
    delayEffect(source, 3);
    auto bus = constant(1.0f);
    bus.routing.busId = "bus";
    NativeRoutingGraph graph({ bus, source }, 16);
    EXPECT_EQ(graph.latency(), 3u);
    const auto result = render(graph, 16);
    for (size_t i = 0; i < result.size(); ++i) {
        EXPECT_FLOAT_EQ(result[i], i < 3 ? 0.0f : i == 3 ? 2.0f : 1.0f) << i;
    }
}

TEST(NativeRoutingGraphTests, NestedBusPathsAndDirectOutputsStayAlignedAcrossPartialBlocks)
{
    auto impulse = constant(0.0f);
    impulse.source = [](Buffer output, size_t frames, int64_t sample) {
        if (sample <= 10 && sample + static_cast<int64_t>(frames) > 10) {
            for (auto* channel : output) {
                channel[10 - sample] = 1.0f;
            }
        }
    };
    auto routed = impulse;
    routed.routing.output = "bus";
    delayEffect(routed, 3);
    Node bus;
    bus.routing.busId = "bus";
    delayEffect(bus, 5);
    NativeRoutingGraph graph({ bus, impulse, routed }, 16);
    ASSERT_EQ(graph.latency(), 8u);
    std::vector<float> result;
    int64_t sample = 0;
    for (size_t count : { 5u, 7u, 13u, 11u }) {
        const auto block = render(graph, count, sample);
        result.insert(result.end(), block.begin(), block.end());
        sample += count;
    }
    for (size_t i = 0; i < result.size(); ++i) {
        EXPECT_FLOAT_EQ(result[i], i == 18 ? 2.0f : 0.0f) << i;
    }
    graph.reset(0);
    auto repeat = render(graph, 16, 0);
    auto tail = render(graph, 16, 16);
    EXPECT_FLOAT_EQ(tail[2], 2.0f);
}

TEST(NativeRoutingGraphTests, FadersUseTheContentTimeAfterLatencyCompensation)
{
    auto source = constant(1.0f);
    delayEffect(source, 4);
    source.fader = [](std::span<float> left, std::span<float> right, int64_t sample) {
        for (size_t i = 0; i < left.size(); ++i) {
            left[i] = right[i] = sample + static_cast<int64_t>(i) >= 3 ? 0.75f : 0.25f;
        }
    };
    NativeRoutingGraph graph({ source }, 16);
    const auto result = render(graph, 16);
    for (size_t i = 0; i < result.size(); ++i) {
        EXPECT_FLOAT_EQ(result[i], i < 4 ? 0.0f : i < 7 ? 0.25f : 0.75f) << i;
    }
}

TEST(NativeRoutingGraphTests, MutingASourceSilencesBothItsOutputAndPreFaderSends)
{
    auto source = constant(0.5f);
    bool mute = false;
    source.silent = [&] { return mute; };
    source.routing.sends = { { "bus", 1.0, true } };
    Node bus;
    bus.routing.busId = "bus";
    NativeRoutingGraph graph({ source, bus }, 16);
    EXPECT_FLOAT_EQ(render(graph, 16)[0], 1.0f);
    mute = true;
    EXPECT_FLOAT_EQ(render(graph, 16, 16)[0], 0.0f);
    mute = false;
    EXPECT_FLOAT_EQ(render(graph, 16, 32)[0], 1.0f);
}

TEST(NativeRoutingGraphTests, FeedbackCyclesAreRejectedEvenWhenASendIsCurrentlySilent)
{
    Node first, second;
    first.routing.busId = "one";
    first.routing.output = "two";
    second.routing.busId = "two";
    second.routing.sends = { { "one", 0.0, true } };
    EXPECT_THROW((NativeRoutingGraph { { first, second }, 64 }), std::invalid_argument);
    second.routing.sends.clear();
    second.routing.busId = "one";
    EXPECT_THROW((NativeRoutingGraph { { first, second }, 64 }), std::invalid_argument);
}

TEST(NativeRoutingGraphTests, ARemovedOutputBusFallsBackToMasterAndItsSendsAreIgnored)
{
    auto source = constant(0.5f);
    source.routing.output = "removed";
    source.routing.sends = { { "removed", 1.0, false } };
    NativeRoutingGraph graph({ source }, 16);
    EXPECT_FLOAT_EQ(render(graph, 16)[0], 0.5f);
}
}
