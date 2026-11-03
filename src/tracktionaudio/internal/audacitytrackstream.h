/*
* Audacity: A Digital Audio Editor
*/
#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <utility>
#include <vector>

class WideSampleSequence;

namespace au::tracktionaudio {
//! Reads a track ahead of time on its own thread and hands the samples to the
//! real-time thread through a lock-free queue. Reading involves stretching and
//! database access, neither of which a real-time thread can afford. Resamples
//! if the rate of the sequence differs. Volume, pan and clip envelopes aren't applied.
class AudacityTrackStream
{
public:
    using SampleRange = std::pair<int64_t, int64_t>;

    //! The sequence's own dependencies must outlive this object.
    explicit AudacityTrackStream(std::shared_ptr<const WideSampleSequence> sequence);
    ~AudacityTrackStream();

    unsigned channelCount() const;

    //! Starts reading ahead at this rate. Does nothing if already prepared for it.
    /*! @pre `read` isn't running if the rate changes. */
    void prepare(double sampleRate);

    //! Makes the read-ahead wrap at the end of this range. Positions are in samples.
    void setLoop(std::optional<SampleRange> range);

    //! Moves to a position and waits for enough samples to start playing from it.
    /*!
     * @pre `read` isn't running.
     * @return false if the samples weren't ready in time.
     */
    bool prime(int64_t position, std::chrono::milliseconds timeout);

    //! Real-time safe. Outputs silence for whatever isn't ready.
    /*! Reading from an unexpected position makes the read-ahead follow, unless the
     * next block already starts there, as it does when wrapping around a loop. */
    void read(int64_t position, float* const* destinations, int frames);

    //! Only valid for the thread calling `read`.
    int bufferedFrames() const;

    //! The number of `read` calls that couldn't be filled.
    uint64_t underrunCount() const;

private:
    struct Block {
        int64_t start = 0;
        int frames = 0;
        uint32_t generation = 0;
        std::vector<float> samples;
    };

    class Resampler;

    void start(double sampleRate);
    void stop();
    void run();
    void produce(Block& block, int64_t position, int frames);
    void requestPosition(int64_t position);
    std::optional<SampleRange> loop() const;
    Block* front();
    void pop();

    const std::shared_ptr<const WideSampleSequence> m_sequence;
    const unsigned m_channels;

    double m_sampleRate = 0.0;
    std::unique_ptr<Resampler> m_resampler;
    std::thread m_thread;
    std::atomic<bool> m_stopping { false };

    std::vector<Block> m_blocks;
    std::atomic<size_t> m_written { 0 };
    std::atomic<size_t> m_consumed { 0 };

    // Blocks from an older generation are obsolete.
    std::atomic<uint32_t> m_generation { 0 };
    std::atomic<int64_t> m_requestedPosition { 0 };
    std::atomic<int64_t> m_expectedPosition { 0 };
    std::atomic<uint64_t> m_underruns { 0 };

    mutable std::mutex m_loopMutex;
    std::optional<SampleRange> m_loop;
};
}
