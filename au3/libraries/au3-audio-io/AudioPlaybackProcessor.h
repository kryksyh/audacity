/*
* Audacity: A Digital Audio Editor
*/
#pragma once

#include <memory>
#include <optional>
#include <span>
#include <vector>
#include "au3-utility/GlobalVariable.h"

class AudacityProject;
struct PlayableSequence;
namespace RealtimeEffects { class ProcessingScope; }

// An optional project mixer on the original AudioIO disk worker. It consumes
// the original per-track resamplers and uses the original realtime effects;
// device callbacks, recording, the transport policy and ring buffers stay in AudioIO.
class AUDIO_IO_API AudioPlaybackProcessor
{
public:
    using Sequences = std::vector<std::shared_ptr<const PlayableSequence>>;
    struct AUDIO_IO_API Factory : GlobalHook<Factory,
        std::unique_ptr<AudioPlaybackProcessor>(AudacityProject&, const Sequences&, double, unsigned, size_t)> {};
    virtual ~AudioPlaybackProcessor() = default;
    virtual unsigned ProcessingChannels(size_t sequence) const = 0;
    virtual size_t Capacity() const = 0;
    // Called with both AudioIO threads suspended, including on a seek.
    virtual void Reset() = 0;
    virtual void SetPlaybackLoop(bool, double, double) {}
    // Returns initial compensation frames to omit from both output and meters.
    virtual size_t Process(RealtimeEffects::ProcessingScope&, std::span<const float* const> inputs,
                          std::span<const double> sampleTimes, std::optional<double> nextTime = std::nullopt) = 0;
    virtual const float* Output(unsigned channel) const = 0;
    //! Output frame boundaries before omitting the returned pre-roll count.
    virtual std::span<const double> OutputTimes() const = 0;
    virtual const float* Meter(size_t sequence, unsigned channel) const = 0;
};
