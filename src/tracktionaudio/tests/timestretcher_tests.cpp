/*
* Audacity: A Digital Audio Editor
*/
#include <tracktion_engine/tracktion_engine.h>
#include <rubberband/RubberBandStretcher.h>

#include <gtest/gtest.h>

namespace au::tracktionaudio {
TEST(TimeStretcherTests, VoiceModeDiscardsTheFullStartDelayBeforeReturningAudio)
{
    for (const double rate : { 44100.0, 48000.0, 96000.0, 192000.0 }) {
        SCOPED_TRACE(rate);
        constexpr int block = 256;
        tracktion::TimeStretcher stretcher;
        stretcher.initialise(rate, block, 1, tracktion::TimeStretcher::rubberbandVoice, {}, true);
        ASSERT_TRUE(stretcher.isInitialised());
        ASSERT_TRUE(stretcher.setSpeedAndPitch(1.0f, 0.0f));
        using Rubber = RubberBand::RubberBandStretcher;
        Rubber reference(static_cast<size_t>(rate), 1, Rubber::OptionProcessRealTime | Rubber::OptionPitchHighConsistency
                         | Rubber::OptionEngineFiner | Rubber::OptionFormantPreserved);
        reference.setMaxProcessSize(stretcher.getMaxFramesNeeded());
        std::vector<float> input(stretcher.getMaxFramesNeeded()), output(block), expected(block);
        const float* inputs[] { input.data() };
        float* outputs[] { output.data() };
        float* references[] { expected.data() };
        auto padding = static_cast<int>(reference.getPreferredStartPad());
        while (padding > 0) {
            const int count = std::min(padding, block);
            reference.process(inputs, count, false);
            padding -= count;
        }
        int delay = static_cast<int>(reference.getStartDelay());
        int position = 0;
        int compared = 0;
        for (int iteration = 0; iteration < 32 && compared < 4096; ++iteration) {
            const int needed = stretcher.getFramesNeeded();
            ASSERT_LE(needed, static_cast<int>(input.size()));
            for (int frame = 0; frame < needed; ++frame) {
                input[frame] = 0.25f + 0.00001f * (position + frame);
            }
            reference.process(inputs, needed, false);
            position += needed;
            const int count = stretcher.processData(inputs, needed, outputs);
            while (delay > 0 && reference.available() > 0) {
                const int discard = std::min({ delay, reference.available(), block });
                ASSERT_EQ(reference.retrieve(references, discard), static_cast<size_t>(discard));
                delay -= discard;
            }
            ASSERT_GE(reference.available(), count);
            ASSERT_EQ(reference.retrieve(references, count), static_cast<size_t>(count));
            for (int frame = 0; frame < count; ++frame) {
                ASSERT_NEAR(output[frame], expected[frame], 1e-5f) << compared + frame;
            }
            compared += count;
        }
        EXPECT_GE(compared, 4096);
    }
}

TEST(TimeStretcherTests, VoiceFlushDiscardsStartDelayForAShortSource)
{
    constexpr int block = 256;
    tracktion::TimeStretcher stretcher;
    stretcher.initialise(48000.0, block, 1, tracktion::TimeStretcher::rubberbandVoice, {}, true);
    ASSERT_TRUE(stretcher.setSpeedAndPitch(1.0f, 0.0f));
    using Rubber = RubberBand::RubberBandStretcher;
    Rubber reference(48000, 1, Rubber::OptionProcessRealTime | Rubber::OptionPitchHighConsistency
                     | Rubber::OptionEngineFiner | Rubber::OptionFormantPreserved);
    reference.setMaxProcessSize(stretcher.getMaxFramesNeeded());
    std::vector<float> input(block), output(block), expected(block);
    const float* inputs[] { input.data() };
    float* outputs[] { output.data() };
    float* references[] { expected.data() };
    int padding = static_cast<int>(reference.getPreferredStartPad());
    while (padding > 0) {
        const int count = std::min(padding, block);
        reference.process(inputs, count, false);
        padding -= count;
    }
    std::fill(input.begin(), input.end(), 0.25f);
    reference.process(inputs, 16, false);
    ASSERT_EQ(stretcher.processData(inputs, 16, outputs), 0);
    reference.process(inputs, 0, true);
    int delay = static_cast<int>(reference.getStartDelay());
    int compared = 0;
    for (int iteration = 0; iteration < 64; ++iteration) {
        const int count = stretcher.flush(outputs);
        while (delay > 0 && reference.available() > 0) {
            const int discard = std::min({ delay, reference.available(), block });
            ASSERT_EQ(reference.retrieve(references, discard), static_cast<size_t>(discard));
            delay -= discard;
        }
        ASSERT_GE(std::max(0, reference.available()), count);
        if (count == 0) {
            EXPECT_GT(compared, 0);
            return;
        }
        ASSERT_EQ(reference.retrieve(references, count), static_cast<size_t>(count));
        for (int frame = 0; frame < count; ++frame) {
            ASSERT_NEAR(output[frame], expected[frame], 1e-5f) << compared + frame;
        }
        compared += count;
    }
    FAIL() << "The stretcher did not finish the source.";
}
}
