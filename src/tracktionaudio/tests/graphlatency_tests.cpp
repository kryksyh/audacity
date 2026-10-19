/*
* Audacity: A Digital Audio Editor
*/
#include <tracktion_engine/tracktion_engine.h>
#include <tracktion_graph/tracktion_graph.h>

#include <gtest/gtest.h>

namespace au::tracktionaudio {
TEST(GraphLatencyTests, PartialChannelReadsKeepSamplesAcrossTheFifoBoundary)
{
    for (const bool adding : { false, true }) {
        SCOPED_TRACE(adding);
        tracktion::graph::AudioFifo fifo(2, 5);
        juce::AudioBuffer<float> input(2, 3);
        for (int channel = 0; channel < 2; ++channel) {
            for (int frame = 0; frame < 3; ++frame) {
                input.setSample(channel, frame, (channel + 1) * (frame + 1));
            }
        }
        ASSERT_TRUE(fifo.write(tracktion::graph::toBufferView(input)));
        juce::AudioBuffer<float> first(1, 2);
        ASSERT_TRUE(fifo.readOverwriting(tracktion::graph::toBufferView(first)));
        EXPECT_FLOAT_EQ(first.getSample(0, 0), 1.0f);
        EXPECT_FLOAT_EQ(first.getSample(0, 1), 2.0f);
        for (int channel = 0; channel < 2; ++channel) {
            for (int frame = 0; frame < 3; ++frame) {
                input.setSample(channel, frame, (channel + 1) * (frame + 4));
            }
        }
        ASSERT_TRUE(fifo.write(tracktion::graph::toBufferView(input)));
        juce::AudioBuffer<float> second(1, 4);
        std::fill_n(second.getWritePointer(0), 4, 100.0f);
        const auto view = tracktion::graph::toBufferView(second);
        ASSERT_TRUE(adding ? fifo.readAdding(view) : fifo.readOverwriting(view));
        for (int frame = 0; frame < 4; ++frame) {
            EXPECT_FLOAT_EQ(second.getSample(0, frame), frame + 3.0f + (adding ? 100.0f : 0.0f));
        }
        EXPECT_EQ(fifo.getNumReady(), 0);
    }
}
}
