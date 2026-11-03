/*
* Audacity: A Digital Audio Editor
*/
#include "audacitytrackstream.h"

#include <algorithm>
#include <cassert>
#include <cmath>

#include "au3-math/Resample.h"
#include "au3-mixer/WideSampleSequence.h"

using namespace au::tracktionaudio;

namespace {
constexpr int BLOCK_FRAMES = 1024;
constexpr double READ_AHEAD_SECONDS = 0.5;
constexpr auto IDLE_WAIT = std::chrono::milliseconds(5);
// Bug 2536: soxr can read past the end of its output buffer.
constexpr size_t OUTPUT_PADDING = 16;
}

class AudacityTrackStream::Resampler
{
public:
    Resampler(const WideSampleSequence& sequence, double outputRate)
        : m_sequence(sequence), m_channels(sequence.NChannels()), m_factor(outputRate / sequence.GetRate())
    {
        m_input.assign(m_channels, std::vector<float>(BLOCK_FRAMES));
        reposition(0);
    }

    void reposition(int64_t outputPosition)
    {
        m_inputPosition = std::llround(outputPosition / m_factor);
        m_inputStart = 0;
        m_inputLength = 0;
        m_resamplers.clear();
        for (size_t channel = 0; channel < m_channels; ++channel) {
            m_resamplers.emplace_back(false, m_factor, m_factor);
        }
    }

    //! @return at least one frame, and at most `frames`.
    int process(float* samples, int frames)
    {
        std::pair<size_t, size_t> result;
        do {
            if (m_inputLength == 0) {
                readInput();
            }
            for (size_t channel = 0; channel < m_channels; ++channel) {
                // Assuming that progress depends only on the size of the input, this is the same for each channel.
                result = m_resamplers[channel].Process(m_factor, m_input[channel].data() + m_inputStart, m_inputLength, false,
                                                       samples + channel * BLOCK_FRAMES, frames);
            }
            m_inputStart += result.first;
            m_inputLength -= result.first;
        } while (result.second == 0);
        return static_cast<int>(result.second);
    }

private:
    void readInput()
    {
        std::vector<float*> buffers;
        for (auto& input : m_input) {
            buffers.push_back(input.data());
        }
        m_sequence.GetFloats(0, m_channels, buffers.data(), m_inputPosition, BLOCK_FRAMES, false, FillFormat::fillZero, false);
        m_inputPosition += BLOCK_FRAMES;
        m_inputStart = 0;
        m_inputLength = BLOCK_FRAMES;
    }

    const WideSampleSequence& m_sequence;
    const size_t m_channels;
    const double m_factor;
    std::vector<Resample> m_resamplers;
    std::vector<std::vector<float> > m_input;
    int64_t m_inputPosition = 0;
    size_t m_inputStart = 0;
    size_t m_inputLength = 0;
};

AudacityTrackStream::AudacityTrackStream(std::shared_ptr<const WideSampleSequence> sequence)
    : m_sequence(std::move(sequence)), m_channels(static_cast<unsigned>(m_sequence->NChannels()))
{
}

AudacityTrackStream::~AudacityTrackStream()
{
    stop();
}

unsigned AudacityTrackStream::channelCount() const
{
    return m_channels;
}

void AudacityTrackStream::prepare(double sampleRate)
{
    if (sampleRate != m_sampleRate) {
        stop();
        start(sampleRate);
    }
}

void AudacityTrackStream::start(double sampleRate)
{
    m_sampleRate = sampleRate;
    m_resampler = sampleRate == m_sequence->GetRate() ? nullptr : std::make_unique<Resampler>(*m_sequence, sampleRate);

    const auto blockCount = static_cast<size_t>(std::ceil(READ_AHEAD_SECONDS * sampleRate / BLOCK_FRAMES));
    m_blocks.assign(blockCount, {});
    for (auto& block : m_blocks) {
        block.samples.resize(static_cast<size_t>(BLOCK_FRAMES) * m_channels + OUTPUT_PADDING);
    }
    m_written.store(0, std::memory_order_relaxed);
    m_consumed.store(0, std::memory_order_relaxed);
    m_stopping.store(false, std::memory_order_relaxed);
    m_thread = std::thread([this] { run(); });
}

void AudacityTrackStream::stop()
{
    if (m_thread.joinable()) {
        m_stopping.store(true, std::memory_order_relaxed);
        m_thread.join();
    }
}

void AudacityTrackStream::setLoop(std::optional<SampleRange> range)
{
    const std::lock_guard lock(m_loopMutex);
    m_loop = range;
}

std::optional<AudacityTrackStream::SampleRange> AudacityTrackStream::loop() const
{
    const std::lock_guard lock(m_loopMutex);
    return m_loop;
}

void AudacityTrackStream::run()
{
    std::optional<uint32_t> currentGeneration;
    int64_t position = 0;
    while (!m_stopping.load(std::memory_order_relaxed)) {
        const auto generation = m_generation.load(std::memory_order_acquire);
        if (generation != currentGeneration) {
            currentGeneration = generation;
            position = m_requestedPosition.load(std::memory_order_acquire);
            if (m_resampler) {
                m_resampler->reposition(std::max<int64_t>(position, 0));
            }
        }

        const auto written = m_written.load(std::memory_order_relaxed);
        if (written - m_consumed.load(std::memory_order_acquire) == m_blocks.size()) {
            std::this_thread::sleep_for(IDLE_WAIT);
            continue;
        }

        const auto wrap = loop();
        const bool looping = wrap && position < wrap->second;
        const int frames = looping ? static_cast<int>(std::min<int64_t>(BLOCK_FRAMES, wrap->second - position)) : BLOCK_FRAMES;

        auto& block = m_blocks[written % m_blocks.size()];
        block.generation = generation;
        block.start = position;
        produce(block, position, frames);
        position += block.frames;
        m_written.store(written + 1, std::memory_order_release);

        if (looping && position == wrap->second) {
            position = wrap->first;
            if (m_resampler) {
                m_resampler->reposition(std::max<int64_t>(position, 0));
            }
        }
    }
}

void AudacityTrackStream::produce(Block& block, int64_t position, int frames)
{
    if (position < 0) {
        // Before the start of the track.
        block.frames = static_cast<int>(std::min<int64_t>(frames, -position));
        std::fill(block.samples.begin(), block.samples.end(), 0.0f);
    } else if (m_resampler) {
        block.frames = m_resampler->process(block.samples.data(), frames);
    } else {
        block.frames = frames;
        std::vector<float*> buffers;
        for (unsigned channel = 0; channel < m_channels; ++channel) {
            buffers.push_back(block.samples.data() + channel * BLOCK_FRAMES);
        }
        m_sequence->GetFloats(0, m_channels, buffers.data(), position, frames, false, FillFormat::fillZero, false);
    }
}

void AudacityTrackStream::requestPosition(int64_t position)
{
    m_requestedPosition.store(position, std::memory_order_release);
    m_generation.fetch_add(1, std::memory_order_acq_rel);
}

AudacityTrackStream::Block* AudacityTrackStream::front()
{
    const auto consumed = m_consumed.load(std::memory_order_relaxed);
    if (consumed == m_written.load(std::memory_order_acquire)) {
        return nullptr;
    }
    return &m_blocks[consumed % m_blocks.size()];
}

void AudacityTrackStream::pop()
{
    m_consumed.fetch_add(1, std::memory_order_acq_rel);
}

bool AudacityTrackStream::prime(int64_t position, std::chrono::milliseconds timeout)
{
    if (!m_thread.joinable()) {
        return false;
    }
    requestPosition(position);
    m_expectedPosition.store(position, std::memory_order_relaxed);
    const auto generation = m_generation.load(std::memory_order_relaxed);
    const auto target = static_cast<int>(m_blocks.size() / 2) * BLOCK_FRAMES;
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (true) {
        // With `read` idle, only this can make room for the new position.
        for (auto* block = front(); block && block->generation != generation; block = front()) {
            pop();
        }
        if (bufferedFrames() >= target) {
            return true;
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

int AudacityTrackStream::bufferedFrames() const
{
    const auto generation = m_generation.load(std::memory_order_acquire);
    const auto end = m_written.load(std::memory_order_acquire);
    int frames = 0;
    for (auto index = m_consumed.load(std::memory_order_relaxed); index != end; ++index) {
        const auto& block = m_blocks[index % m_blocks.size()];
        if (block.generation == generation) {
            frames += block.frames;
        }
    }
    return frames;
}

uint64_t AudacityTrackStream::underrunCount() const
{
    return m_underruns.load(std::memory_order_relaxed);
}

void AudacityTrackStream::read(int64_t position, float* const* destinations, int frames)
{
    if (m_blocks.empty()) {
        for (unsigned channel = 0; channel < m_channels; ++channel) {
            std::fill_n(destinations[channel], frames, 0.0f);
        }
        return;
    }

    if (position != m_expectedPosition.load(std::memory_order_relaxed)) {
        const auto* next = front();
        if (!next || next->generation != m_generation.load(std::memory_order_acquire) || next->start != position) {
            requestPosition(position);
        }
    }
    m_expectedPosition.store(position + frames, std::memory_order_relaxed);

    const auto generation = m_generation.load(std::memory_order_acquire);
    int done = 0;
    while (done < frames) {
        const auto* block = front();
        if (!block) {
            break;
        }
        const auto wanted = position + done;
        if (block->generation != generation || block->start + block->frames <= wanted) {
            pop();
            continue;
        }
        if (block->start > wanted) {
            break;
        }
        const auto offset = static_cast<int>(wanted - block->start);
        const int count = std::min(frames - done, block->frames - offset);
        for (unsigned channel = 0; channel < m_channels; ++channel) {
            std::copy_n(block->samples.begin() + channel * BLOCK_FRAMES + offset, count, destinations[channel] + done);
        }
        done += count;
        // Leave no finished block behind, as the next read may be for what follows a loop wrap.
        if (offset + count == block->frames) {
            pop();
        }
    }

    if (done < frames) {
        for (unsigned channel = 0; channel < m_channels; ++channel) {
            std::fill_n(destinations[channel] + done, frames - done, 0.0f);
        }
        m_underruns.fetch_add(1, std::memory_order_relaxed);
    }
}
