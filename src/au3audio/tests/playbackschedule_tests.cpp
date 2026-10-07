/*
 * Audacity: A Digital Audio Editor
 */
#include <gtest/gtest.h>

#include "au3-audio-io/PlaybackSchedule.h"

namespace au::au3audio {
namespace {
constexpr double RATE = 48000.0;
constexpr double GRAIN_SECONDS = TimeQueueGrainSize / RATE;

class PlaybackScheduleTimeQueueTests : public ::testing::Test
{
protected:
    void SetUp() override
    {
        m_schedule.mT0 = 0.0;
        m_schedule.mT1 = 100.0;
        m_schedule.mEnvelope = nullptr;
        m_schedule.GetPolicy().Initialize(m_schedule, RATE);
    }

    void produceGrain()
    {
        m_schedule.mTimeQueue.Producer(m_schedule, PlaybackSlice { TimeQueueGrainSize, TimeQueueGrainSize, TimeQueueGrainSize });
    }

    double consumeGrain()
    {
        return m_schedule.mTimeQueue.Consumer(TimeQueueGrainSize, RATE);
    }

    PlaybackSchedule m_schedule;
};
}

// The producer fills the ring up to the slot before the consumer's head after
// the tail has wrapped; it must grow the queue instead of overwriting unread times
TEST_F(PlaybackScheduleTimeQueueTests, ConsumerKeepsTimeWhenProducerFillsWrappedRing)
{
    m_schedule.mTimeQueue.Init(4);
    m_schedule.mTimeQueue.Prime(0.0);

    produceGrain();
    produceGrain();
    EXPECT_NEAR(consumeGrain(), 1 * GRAIN_SECONDS, 1e-9);
    EXPECT_NEAR(consumeGrain(), 2 * GRAIN_SECONDS, 1e-9);

    for (int i = 0; i < 4; ++i) {
        produceGrain();
    }

    for (int i = 3; i <= 6; ++i) {
        EXPECT_NEAR(consumeGrain(), i * GRAIN_SECONDS, 1e-9) << "grain " << i;
    }
}
}
