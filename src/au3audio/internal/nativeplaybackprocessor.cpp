/*
* Audacity: A Digital Audio Editor
*/
#include "nativeplaybackprocessor.h"
#include "nativeroutinggraph.h"
#include "nativetimesignature.h"

#include <algorithm>
#include <atomic>
#include <map>
#include <cmath>
#include <limits>
#include <stdexcept>
#include "au3-midi-track/MidiTrack.h"
#include "au3-numeric-formats/ProjectTimeSignature.h"
#include "au3-playable-track/TrackAutomation.h"
#include "au3-realtime-effects/RealtimeEffectList.h"
#include "au3-realtime-effects/RealtimeEffectManager.h"
#include "au3-realtime-effects/RealtimeEffectState.h"
#include "au3-wave-track/WaveTrack.h"

namespace au::au3audio {
namespace {
using Stereo = std::array<std::vector<float>, 2>;
using Buffer = NativeRoutingGraph::Buffer;
Stereo storage(size_t frames) { return { std::vector<float>(frames), std::vector<float>(frames) }; }
Buffer pointers(Stereo& data) { return { data[0].data(), data[1].data() }; }
size_t plusLatency(size_t first, size_t second)
{
    if (second > static_cast<size_t>(std::numeric_limits<int64_t>::max()) - first) {
        throw std::overflow_error("Playback effect latency is out of range.");
    }
    return first + second;
}

// Content time follows the original playback policy, including speed changes
// and loop boundaries. DSP sample positions stay monotonic across a loop so a
// loop shorter than an insert delay still plays continuously.
class PlaybackClock final
{
public:
    void prepare(size_t capacity, double rate, double speed)
    {
        times.assign(capacity, 0.0);
        sampleRate = rate;
        end = 0;
        firstSpeed = speed;
    }

    void append(std::span<const double> positions, std::optional<double> nextTime)
    {
        if (!end && !positions.empty()) {
            firstTime = positions.front();
            for (size_t i = 1; i < positions.size(); ++i) {
                const auto speed = (positions[i] - positions[i - 1]) * sampleRate;
                if (speed > 0.0 && speed <= 100.0) {
                    firstSpeed = speed;
                    break;
                }
            }
        }
        for (const auto position : positions) {
            times[static_cast<size_t>(end++) % times.size()] = position;
        }
        if (!positions.empty()) {
            endTime = nextTime.value_or(positions.back() + firstSpeed / sampleRate);
        }
    }

    double at(int64_t sample) const
    {
        if (sample < 0) {
            return firstTime + sample / sampleRate * firstSpeed;
        }
        if (sample == end) {
            return endTime;
        }
        if (sample >= end || end - sample > static_cast<int64_t>(times.size())) {
            throw std::out_of_range("Playback automation clock is outside retained history.");
        }
        return times[static_cast<size_t>(sample) % times.size()];
    }

    double speedAt(int64_t sample) const
    {
        if (sample < 0) {
            return firstSpeed;
        }
        const auto valid = [&](int64_t position) {
            return position >= 0 && position < end && end - position <= static_cast<int64_t>(times.size());
        };
        for (const auto start : { sample, sample - 1 }) {
            if (valid(start) && valid(start + 1)) {
                const auto speed = (at(start + 1) - at(start)) * sampleRate;
                // A loop wrap is a position discontinuity, not reverse speed.
                if (speed > 0.0 && speed <= 100.0) {
                    return speed;
                }
            }
        }
        return firstSpeed;
    }

private:
    std::vector<double> times;
    double firstTime = 0.0, sampleRate = 48000.0;
    double endTime = 0.0;
    double firstSpeed = 1.0;
    int64_t end = 0;
};

class ClockTime final : public RealtimeEffects::ProcessingTime
{
public:
    ClockTime(const PlaybackClock& clock, int64_t sample, const EffectProcessingContext& context)
        : clock(clock), sample(sample), context(context) {}
    double TimeAt(int64_t offset) const override { return clock.at(sample + offset); }
    EffectProcessingContext ContextAt(int64_t offset) const override
    {
        auto result = context;
        result.time = TimeAt(offset);
        result.speed = result.playing ? clock.speedAt(sample + offset) : 0.0;
        return result;
    }

private:
    const PlaybackClock& clock;
    int64_t sample;
    EffectProcessingContext context;
};
}

struct NativePlaybackProcessor::Impl {
    struct Track {
        std::shared_ptr<const PlayableSequence> sequence;
        const PlayableTrack* document;
        const WaveTrack* wave;
        const MidiTrack* midi;
        TrackRouting routing;
        AutomationData automation;
        size_t inputOffset = 0;
        unsigned channels = 2;
        bool silent = false;
        Stereo meter;
        Stereo meterDelay;
        size_t meterPosition = 0;
        const AutomationLaneData* volumeLane = nullptr;
        const AutomationLaneData* panLane = nullptr;
        unsigned muteSolo = 4;
        double gain(unsigned channel, double time) const
        {
            const double volume = wave ? wave->GetVolume() : midi->GetGain();
            const double pan = wave ? wave->GetPan() : midi->GetPan();
            const auto gain = AutomationData::ValueAt(volumeLane, time, volume / TrackAutomation::MaximumVolume)
                              * TrackAutomation::MaximumVolume;
            const auto position = AutomationData::ValueAt(panLane, time, (pan + 1.0) * 0.5) * 2.0 - 1.0;
            return gain * (channel == 0 ? std::min(1.0, 1.0 - position) : std::min(1.0, 1.0 + position));
        }
    };

    Impl(AudacityProject& project, const Sequences& sequences, double rate, unsigned channels, size_t capacity, bool playing)
        : project(project), liveTimeSignature(NativeTimeSignature::Get(project)), rate(rate), channels(channels), capacity(capacity),
        output(storage(capacity)),
        scratch(storage(capacity)), dummy(capacity), outputTimes(capacity + 1)
    {
        if (channels == 0 || channels > 2 || !capacity || !std::isfinite(rate) || rate <= 0.0) {
            throw std::invalid_argument("Invalid native playback format.");
        }
        const auto& signature = ProjectTimeSignature::Get(project);
        context = { 0.0, rate, signature.GetTempo(), signature.GetUpperTimeSignature(), signature.GetLowerTimeSignature(),
                    playing ? 1.0 : 0.0, playing };
        tracks.reserve(sequences.size());
        for (const auto& sequence : sequences) {
            const auto* document = dynamic_cast<const PlayableTrack*>(sequence->FindChannelGroup());
            const auto* wave = dynamic_cast<const WaveTrack*>(document);
            const auto* midi = dynamic_cast<const MidiTrack*>(document);
            if ((!wave && !midi) || sequence->NChannels() == 0 || sequence->NChannels() > 2) {
                throw std::invalid_argument("This sequence cannot use native track routing.");
            }
            tracks.push_back({ sequence, document, wave, midi, TrackRouting::Get(*document), TrackAutomation::Get(*document),
                               inputChannels, static_cast<unsigned>(sequence->NChannels()), false, storage(capacity), {}, 0 });
            inputChannels += sequence->NChannels();
        }
        // Cache routing reachability once. Mute/solo updates on the device
        // callback must not allocate maps/sets or traverse document vectors.
        const auto count = tracks.size();
        reachable.assign(count * count, false);
        std::map<std::string, size_t> buses;
        for (size_t i = 0; i < count; ++i) {
            if (!tracks[i].routing.busId.empty()) {
                buses.emplace(tracks[i].routing.busId, i);
            }
        }
        for (size_t i = 0; i < count; ++i) {
            reachable[i * count + i] = true;
            const auto follow = [&](const std::string& bus) {
                if (const auto found = buses.find(bus); found != buses.end()) {
                    reachable[i * count + found->second] = true;
                }
            };
            follow(tracks[i].routing.output);
            for (const auto& send : tracks[i].routing.sends) {
                follow(send.bus);
            }
        }
        for (size_t via = 0; via < count; ++via) {
            for (size_t source = 0; source < count; ++source) {
                if (!reachable[source * count + via]) {
                    continue;
                }
                for (size_t target = 0; target < count; ++target) {
                    reachable[source * count + target] = reachable[source * count + target] || reachable[via * count + target];
                }
            }
        }
        for (auto& track : tracks) {
            track.volumeLane = track.automation.Find("volume");
            track.panLane = track.automation.Find("pan");
        }
    }

    unsigned processingChannels(size_t index) const
    {
        const auto& track = tracks[index];
        return track.channels == 1 && track.routing.busId.empty() ? 1 : 2;
    }

    size_t latency(const ChannelGroup* group) const
    {
        const auto& list = group ? RealtimeEffectList::Get(*group) : RealtimeEffectList::Get(project);
        size_t result = 0;
        const std::lock_guard guard(list.GetLock());
        list.Visit([&](const RealtimeEffectState& state, bool) { result = plusLatency(result, state.ProcessingLatency(group)); });
        return result;
    }

    void effects(const ChannelGroup* group, unsigned count, Buffer buffers, size_t frames, int64_t sample)
    {
        const auto scratches = pointers(scratch);
        ClockTime time(clock, sample, context);
        // Short plug-in output is zeroed by RealtimeEffectState without moving
        // the timeline or dropping samples from the other channels.
        scope->ProcessRaw(group, buffers.data(), scratches.data(), dummy.data(), count, frames, &time);
        if (count == 1) {
            std::copy_n(buffers[0], frames, buffers[1]);
        }
    }

    bool effectsChanged()
    {
        if (!allowReconfiguration) {
            size_t index = 0;
            bool changed = false;
            const auto compare = [&](const RealtimeEffectState* state) {
                changed |= index >= currentEffects.size()
                           || currentEffects[index] != EffectVersion { state, state ? state->ProcessingLatencyRevision() : 0 };
                ++index;
            };
            const auto visit = [&](const RealtimeEffectList& list) {
                const std::lock_guard guard(list.GetLock());
                compare(nullptr);
                list.Visit([&](const RealtimeEffectState& state, bool) { compare(&state); });
            };
            for (const auto& track : tracks) {
                visit(RealtimeEffectList::Get(*track.document));
            }
            visit(RealtimeEffectList::Get(project));
            return changed || index != currentEffects.size();
        }
        pendingEffects.clear();
        const auto collect = [&](const RealtimeEffectList& list) {
            const std::lock_guard guard(list.GetLock());
            // Include the group boundary so moving an insert between two
            // groups cannot leave a stale per-path delay configuration.
            pendingEffects.push_back({ nullptr, 0 });
            list.Visit([&](const RealtimeEffectState& state, bool) {
                pendingEffects.push_back({ &state, state.ProcessingLatencyRevision() });
            });
        };
        for (const auto& track : tracks) {
            collect(RealtimeEffectList::Get(*track.document));
        }
        collect(RealtimeEffectList::Get(project));
        if (pendingEffects == currentEffects) {
            return false;
        }
        currentEffects = pendingEffects;
        return true;
    }

    void prepare()
    {
        const size_t probeFrames = std::min<size_t>(512, capacity);
        const auto scratches = pointers(scratch);
        const auto buffers = pointers(output);
        const auto probe = [&](const ChannelGroup* group, unsigned count) {
            for (auto& channel : output) {
                std::fill_n(channel.data(), probeFrames, 0.0f);
            }
            scope->ProbeRaw(group, buffers.data(), scratches.data(), dummy.data(), count, probeFrames);
        };
        for (size_t index = 0; index < tracks.size(); ++index) {
            probe(tracks[index].document, processingChannels(index));
        }
        probe(RealtimeEffectManager::MasterGroup, channels);
        masterLatency = latency(RealtimeEffectManager::MasterGroup);
        std::vector<NativeRoutingGraph::Node> nodes;
        for (size_t index = 0; index < tracks.size(); ++index) {
            auto& track = tracks[index];
            NativeRoutingGraph::Node node;
            node.routing = track.routing;
            node.source = [this, index](Buffer buffers, size_t frames, int64_t) {
                const auto& track = tracks[index];
                for (size_t channel = 0; channel < 2; ++channel) {
                    std::copy_n(inputs[track.inputOffset + std::min<size_t>(channel, track.channels - 1)], frames, buffers[channel]);
                }
            };
            node.effectLatency = latency(track.document);
            node.effects = [this, index](Buffer buffers, size_t frames, int64_t sample) {
                effects(tracks[index].document, processingChannels(index), buffers, frames, sample);
            };
            node.fader = [this, index](std::span<float> left, std::span<float> right, int64_t sample) {
                const auto& track = tracks[index];
                for (size_t frame = 0; frame < left.size(); ++frame) {
                    const auto time = clock.at(sample + static_cast<int64_t>(frame));
                    left[frame] = track.gain(0, time);
                    right[frame] = track.gain(1, time);
                }
            };
            node.silent = [this, index] { return tracks[index].silent; };
            track.meterDelay = storage(masterLatency);
            track.meterPosition = 0;
            node.meterOutput = [this, index](Buffer buffers, size_t frames, int64_t) {
                auto& track = tracks[index];
                for (size_t frame = 0; frame < frames; ++frame) {
                    for (size_t channel = 0; channel < 2; ++channel) {
                        float value = buffers[channel][frame];
                        if (masterLatency) {
                            std::swap(value, track.meterDelay[channel][track.meterPosition]);
                        }
                        track.meter[channel][frame] = value;
                    }
                    if (masterLatency && ++track.meterPosition == masterLatency) {
                        track.meterPosition = 0;
                    }
                }
            };
            nodes.push_back(std::move(node));
        }
        graph = std::make_unique<NativeRoutingGraph>(std::move(nodes), capacity);
        remainingPreroll = plusLatency(graph->latency(), masterLatency);
        clock.prepare(plusLatency(remainingPreroll, capacity + 1), rate, context.speed);
        // A plug-in may publish its initial latency during the silent probe.
        // Keep the signature at that revision to avoid a second spurious reset.
        effectsChanged();
    }

    size_t process(RealtimeEffects::ProcessingScope& processingScope, std::span<const float* const> raw,
                   std::span<const double> times, std::optional<double> nextTime)
    {
        if (times.size() > capacity || raw.size() != inputChannels) {
            throw std::invalid_argument("Invalid native playback input buffers.");
        }
        if (context.playing) {
            const auto meter = liveTimeSignature.meter();
            context.numerator = meter.numerator;
            context.denominator = meter.denominator;
        }
        scope = &processingScope;
        inputs = raw;
        outputTimeCount = times.size() + 1;
        if (effectsChanged() && graph) {
            if (!allowReconfiguration) {
                rebuildRequested.store(true, std::memory_order_release);
                for (auto& channel : output) {
                    std::fill_n(channel.data(), times.size(), 0.0f);
                }
                for (auto& track : tracks) {
                    for (auto& channel : track.meter) {
                        std::fill_n(channel.data(), times.size(), 0.0f);
                    }
                }
                scope = nullptr;
                inputs = {};
                return times.size();
            }
            graph.reset();
            sample = 0;
        }
        if (!graph) {
            prepare();
        }
        clock.append(times, nextTime);
        // Mute/solo controls remain live. Routing and automation edits stop the
        // transport before changing document data.
        bool controlsChanged = false;
        for (auto& track : tracks) {
            const unsigned current = (track.document->GetMute() ? 1 : 0) | (track.document->GetSolo() ? 2 : 0);
            controlsChanged |= current != track.muteSolo;
            track.muteSolo = current;
        }
        if (controlsChanged) {
            const auto count = tracks.size();
            const bool anySolo = std::any_of(tracks.begin(), tracks.end(), [](const Track& track) { return (track.muteSolo & 2) != 0; });
            for (size_t i = 0; i < count; ++i) {
                bool solo = !anySolo;
                for (size_t j = 0; anySolo && j < count && !solo; ++j) {
                    solo = (tracks[j].muteSolo & 2) && (reachable[i * count + j] || reachable[j * count + i]);
                }
                tracks[i].silent = !solo || ((tracks[i].muteSolo & 1) && !(tracks[i].muteSolo & 2));
            }
        }
        const auto buffers = pointers(output);
        graph->process(buffers, times.size(), sample);
        for (auto& track : tracks) {
            if (track.channels == 1 || channels == 1) {
                for (size_t frame = 0; frame < times.size(); ++frame) {
                    const auto left = track.meter[0][frame], right = track.meter[1][frame];
                    track.meter[0][frame] = track.meter[1][frame] = std::abs(left) >= std::abs(right) ? left : right;
                }
            }
        }
        if (channels == 1) {
            for (size_t frame = 0; frame < times.size(); ++frame) {
                output[0][frame] = 0.5f * (output[0][frame] + output[1][frame]);
            }
        }
        effects(RealtimeEffectManager::MasterGroup, channels, buffers, times.size(),
                sample - static_cast<int64_t>(graph->latency()));
        const auto discard = std::min(times.size(), remainingPreroll);
        remainingPreroll -= discard;
        const auto delay = static_cast<int64_t>(plusLatency(graph->latency(), masterLatency));
        for (size_t frame = discard; frame <= times.size(); ++frame) {
            outputTimes[frame] = clock.at(sample + static_cast<int64_t>(frame) - delay);
        }
        sample += static_cast<int64_t>(times.size());
        scope = nullptr;
        inputs = {};
        return discard;
    }

    AudacityProject& project;
    const NativeTimeSignature& liveTimeSignature;
    double rate;
    unsigned channels;
    size_t capacity, inputChannels = 0, masterLatency = 0, remainingPreroll = 0;
    int64_t sample = 0;
    std::vector<Track> tracks;
    std::vector<bool> reachable;
    bool allowReconfiguration = true;
    std::atomic<bool> rebuildRequested { false };
    struct EffectVersion {
        const RealtimeEffectState* state;
        uint64_t revision;
        bool operator==(const EffectVersion&) const = default;
    };
    std::vector<EffectVersion> currentEffects, pendingEffects;
    Stereo output, scratch;
    std::vector<float> dummy;
    std::vector<double> outputTimes;
    size_t outputTimeCount = 0;
    PlaybackClock clock;
    EffectProcessingContext context;
    std::unique_ptr<NativeRoutingGraph> graph;
    RealtimeEffects::ProcessingScope* scope = nullptr;
    std::span<const float* const> inputs;
};

NativePlaybackProcessor::NativePlaybackProcessor(AudacityProject& project, const Sequences& sequences,
                                                 double rate, unsigned channels, size_t capacity, bool playing)
    : m_impl(std::make_unique<Impl>(project, sequences, rate, channels, capacity, playing)) {}
NativePlaybackProcessor::~NativePlaybackProcessor() = default;
unsigned NativePlaybackProcessor::ProcessingChannels(size_t sequence) const { return m_impl->processingChannels(sequence); }
size_t NativePlaybackProcessor::Capacity() const { return m_impl->capacity; }
void NativePlaybackProcessor::Reset() { m_impl->graph.reset(); m_impl->sample = 0; }
void NativePlaybackProcessor::SetPlaybackLoop(bool cycling, double start, double end)
{
    m_impl->context.cycling = cycling;
    m_impl->context.cycleStart = start;
    m_impl->context.cycleEnd = end;
}

void NativePlaybackProcessor::SetRealtimeMode() { m_impl->allowReconfiguration = false; }
void NativePlaybackProcessor::SetTransportContext(double tempo, int numerator, int denominator)
{
    m_impl->context.tempo = tempo;
    m_impl->context.numerator = numerator;
    m_impl->context.denominator = denominator;
}

bool NativePlaybackProcessor::NeedsRebuild() const { return m_impl->rebuildRequested.load(std::memory_order_acquire); }
size_t NativePlaybackProcessor::Process(RealtimeEffects::ProcessingScope& scope, std::span<const float* const> inputs,
                                        std::span<const double> times, std::optional<double> nextTime)
{
    return m_impl->process(scope, inputs, times, nextTime);
}

const float* NativePlaybackProcessor::Output(unsigned channel) const { return m_impl->output[channel].data(); }
std::span<const double> NativePlaybackProcessor::OutputTimes() const { return { m_impl->outputTimes.data(), m_impl->outputTimeCount }; }
const float* NativePlaybackProcessor::Meter(size_t sequence, unsigned channel) const
{
    return m_impl->tracks[sequence].meter[channel].data();
}

void registerNativePlaybackProcessor()
{
    static AudioPlaybackProcessor::Factory::Scope registration([](AudacityProject& project,
                                                                  const AudioPlaybackProcessor::Sequences& sequences,
                                                                  double rate, unsigned channels, size_t capacity)
                                                               -> std::unique_ptr<AudioPlaybackProcessor> {
        if (!channels || channels > 2 || sequences.empty()) {
            return {};
        }
        for (const auto& sequence : sequences) {
            const auto* group = sequence->FindChannelGroup();
            if ((!dynamic_cast<const WaveTrack*>(group) && !dynamic_cast<const MidiTrack*>(group)) || sequence->NChannels() > 2) {
                return {};
            }
        }
        return std::make_unique<NativePlaybackProcessor>(project, sequences, rate, channels, capacity);
    });
}
}
