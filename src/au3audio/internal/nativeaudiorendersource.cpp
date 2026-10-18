/*
* Audacity: A Digital Audio Editor
*/
#include "nativeaudiorendersource.h"
#include "au3-numeric-formats/ProjectTimeSignature.h"
#include "nativeeffectchain.h"
#include "nativeofflinegraph.h"
#include "midiplayablesequence.h"

#include <cmath>
#include <map>
#include <limits>
#include <set>
#include <stdexcept>

#include "au3-math/Dither.h"
#include "au3-mixer/Mix.h"
#include "au3-playable-track/TrackAutomation.h"
#include "au3-realtime-effects/RealtimeEffectList.h"
#include "au3-track/Track.h"
#include "au3-wave-track/WaveTrack.h"
#include "au3-stretching-sequence/StretchingSequence.h"

namespace au::au3audio {
namespace {
using Buffer = NativeRoutingGraph::Buffer;

struct Controls {
    explicit Controls(std::shared_ptr<const PlayableTrack> source, bool enabled)
        : track(std::move(source)), wave(dynamic_cast<const WaveTrack*>(track.get())), midi(dynamic_cast<const MidiTrack*>(track.get()))
    {
        if (enabled) {
            automation = TrackAutomation::Get(*track);
        }
        volume = automation.Find("volume");
        pan = automation.Find("pan");
    }

    double gain(size_t channel, double time) const
    {
        const auto manualVolume = wave ? wave->GetVolume() : midi->GetGain();
        const auto manualPan = wave ? wave->GetPan() : midi->GetPan();
        const auto value = AutomationData::ValueAt(volume, time, manualVolume / TrackAutomation::MaximumVolume)
                           * TrackAutomation::MaximumVolume;
        const auto position = AutomationData::ValueAt(pan, time, (manualPan + 1.0) * 0.5) * 2.0 - 1.0;
        return value * ((channel % 2) == 0 ? std::min(1.0, 1.0 - position) : std::min(1.0, 1.0 + position));
    }

    std::shared_ptr<const PlayableTrack> track;
    const WaveTrack* wave;
    const MidiTrack* midi;
    AutomationData automation;
    const AutomationLaneData* volume = nullptr;
    const AutomationLaneData* pan = nullptr;
};
}

struct NativeAudioRenderSource::Impl {
    Impl(const AudacityProject& project, const Tracks& waves, const Options& options)
        : options(options)
    {
        if (!options.bufferSize || !options.channels || options.channels > 256 || !std::isfinite(options.sampleRate)
            || options.sampleRate <= 0.0 || !std::isfinite(options.startTime) || !std::isfinite(options.stopTime)
            || options.stopTime < options.startTime
            || (options.format != int16Sample && options.format != int24Sample && options.format != floatSample)) {
            throw std::invalid_argument("Invalid native audio rendering format or time range.");
        }
        if (options.channels > 2 && !options.channelMap) {
            throw std::invalid_argument("Multichannel rendering requires an output channel map.");
        }
        auto masterSpecifications = options.masterEffects
                                    ? NativeEffectChain::specifications(RealtimeEffectList::Get(project), options.trackAutomation)
                                    : std::vector<NativeEffectChain::Specification> {};
        if (options.channelMap && (!masterSpecifications.empty() || options.channelMap->GetNumChannels() != options.channels)) {
            throw std::invalid_argument("The output channel map does not match the requested render format.");
        }
        const long double first = static_cast<long double>(options.startTime) * options.sampleRate;
        const long double last = static_cast<long double>(options.stopTime) * options.sampleRate;
        if (first <= std::numeric_limits<int64_t>::min() || last >= std::numeric_limits<int64_t>::max()
            || last - first >= std::numeric_limits<int64_t>::max()) {
            throw std::invalid_argument("The audio rendering time range is too large.");
        }
        start = static_cast<int64_t>(std::llround(options.startTime * options.sampleRate));
        stop = static_cast<int64_t>(std::llround(options.stopTime * options.sampleRate));
        current = rawSample = start;
        // Exporters may request several seconds per block. Keep per-track
        // working memory bounded while satisfying the same streaming API.
        capacity = std::min<size_t>(options.bufferSize, 4096);
        stereo = { std::vector<float>(capacity), std::vector<float>(capacity) };
        floats.assign(options.channels, std::vector<float>(capacity));
        output.resize(options.interleaved ? 1 : options.channels);
        const auto stride = options.interleaved ? options.channels : 1u;
        if (options.bufferSize > std::numeric_limits<size_t>::max() / (stride * SAMPLE_SIZE(options.format))) {
            throw std::overflow_error("The audio rendering output buffer is too large.");
        }
        for (auto& buffer : output) {
            buffer.Allocate(options.bufferSize * stride, options.format);
        }
        std::set<const PlayableTrack*> seen;
        auto add = [&](std::shared_ptr<const PlayableTrack> track) {
            if (track && seen.insert(track.get()).second) {
                tracks.push_back(std::move(track));
            }
        };
        for (const auto& wave : waves) {
            add(wave);
        }
        for (const auto& midi : options.midiTracks) {
            add(midi);
        }
        const std::set<const PlayableTrack*> explicitTracks(seen.begin(), seen.end());
        const auto& signature = ProjectTimeSignature::Get(project);
        const EffectProcessingContext transport { options.startTime, options.sampleRate, signature.GetTempo(),
                                                  signature.GetUpperTimeSignature(), signature.GetLowerTimeSignature() };
        if (options.trackRouting) {
            std::map<std::string, std::shared_ptr<const PlayableTrack> > buses;
            for (const auto* track : TrackList::Get(project).Any<const PlayableTrack>()) {
                if (!TrackRouting::Get(*track).busId.empty()) {
                    buses.emplace(TrackRouting::Get(*track).busId,
                                  std::static_pointer_cast<const PlayableTrack>(track->shared_from_this()));
                }
            }
            // A document operation may render detached snapshots. Its explicit
            // bus snapshots must take precedence over the live document buses.
            for (const auto& track : tracks) {
                const auto& id = TrackRouting::Get(*track).busId;
                if (!id.empty()) {
                    buses[id] = track;
                }
            }
            for (size_t i = 0; i < tracks.size(); ++i) {
                const auto& route = TrackRouting::Get(*tracks[i]);
                auto include = [&](const std::string& id) {
                    const auto bus = buses.find(id);
                    if (bus != buses.end()) {
                        add(bus->second);
                    }
                };
                include(route.output);
                for (const auto& send : route.sends) {
                    include(send.bus);
                }
            }
        }
        if (options.channelMap) {
            std::map<const PlayableTrack*, size_t> order;
            size_t index = 0;
            for (const auto* track : TrackList::Get(project).Any<const PlayableTrack>()) {
                order.emplace(track, index++);
            }
            std::stable_sort(tracks.begin(), tracks.end(), [&](const auto& left, const auto& right) {
                const auto a = order.find(left.get()), b = order.find(right.get());
                const auto first = a == order.end() ? std::numeric_limits<size_t>::max() : a->second;
                const auto second = b == order.end() ? std::numeric_limits<size_t>::max() : b->second;
                return first < second;
            });
        }
        std::vector<const PlayableTrack*> rawTracks;
        for (const auto& track : tracks) {
            rawTracks.push_back(track.get());
        }
        const auto audible = TrackRouting::AudibleSources(rawTracks, false);
        std::set<const PlayableTrack*> audibleSet(audible.begin(), audible.end());
        std::vector<NativeOfflineGraph::Node> nodes;
        size_t row = 0;
        for (size_t i = 0; i < tracks.size(); ++i) {
            const auto& track = tracks[i];
            const auto wave = std::dynamic_pointer_cast<const WaveTrack>(track);
            const auto midi = std::dynamic_pointer_cast<const MidiTrack>(track);
            if (!wave && !midi) {
                throw std::runtime_error("This track cannot be rendered as native audio.");
            }
            NativeOfflineGraph::Node node;
            if (options.trackRouting) {
                node.routing = TrackRouting::Get(*track);
            }
            const auto controls = std::make_shared<Controls>(track, options.trackAutomation);
            const double rate = options.sampleRate;
            node.fader = [controls, rate](std::span<float> left, std::span<float> right, int64_t sample) {
                for (size_t frame = 0; frame < left.size(); ++frame) {
                    const double time = (sample + static_cast<int64_t>(frame)) / rate;
                    left[frame] = controls->gain(0, time);
                    right[frame] = controls->gain(1, time);
                }
            };
            node.silent = [silent = !audibleSet.contains(track.get())] { return silent; };
            std::shared_ptr<Mixer> mixer;
            if (explicitTracks.contains(track.get())) {
                std::shared_ptr<const PlayableSequence> sequence;
                if (wave) {
                    sequence = StretchingSequence::Create(*wave, wave->GetClipInterfaces());
                } else {
                    sequence = std::make_shared<MidiPlayableSequence>(midi,
                                                                      createInstrumentProcessor(midi->GetInstrumentDescription().utf8_string(),
                                                                                                midi->GetInstrumentState().utf8_string()),
                                                                      rate, options.trackAutomation, true, transport.numerator, transport.denominator,
                                                                      options.stopTime + 0.5 / rate);
                }
                mixer = std::make_shared<Mixer>(Mixer::Inputs { Mixer::Input { sequence } }, std::nullopt, true,
                                                // Mixer converts remaining seconds by truncation. A half
                                                // sample guard prevents a final sample being lost after
                                                // repeated partial requests; this renderer owns the exact
                                                // integer output length.
                                                Mixer::WarpOptions { &project }, options.startTime, options.stopTime + 0.5 / rate, 2,
                                                capacity, false,
                                                rate, floatSample, true, nullptr, Mixer::ApplyVolume::Discard);
                auto next = std::make_shared<int64_t>(start);
                node.source = [mixer, next, rate](Buffer buffers, size_t frames, int64_t sample) {
                    if (sample != *next) {
                        mixer->Reposition(sample / rate, true);
                    }
                    for (size_t offset = 0; offset < frames;) {
                        const auto length = std::min(mixer->BufferSize(), frames - offset);
                        const auto count = mixer->Process(length);
                        for (size_t channel = 0; channel < 2; ++channel) {
                            const auto* source = reinterpret_cast<const float*>(mixer->GetBuffer(channel));
                            std::copy_n(source, count, buffers[channel] + offset);
                            std::fill(buffers[channel] + offset + count, buffers[channel] + offset + length, 0.0f);
                        }
                        offset += length;
                    }
                    *next = sample + static_cast<int64_t>(frames);
                };
            }
            const auto outputBus = std::find_if(tracks.begin(), tracks.end(), [&](const auto& target) {
                return !node.routing.output.empty() && TrackRouting::Get(*target).busId == node.routing.output;
            });
            if (options.channelMap && outputBus == tracks.end()
                && (audibleSet.contains(track.get()) || explicitTracks.contains(track.get()))) {
                const auto channels = midi ? 2 : wave->NChannels();
                if (row + channels > options.channelMap->GetNumTracks()) {
                    throw std::invalid_argument("The output map has too few source channels.");
                }
                std::vector<std::vector<bool> > mapping(channels, std::vector<bool>(options.channels));
                for (size_t source = 0; source < channels; ++source) {
                    for (size_t destination = 0; destination < options.channels; ++destination) {
                        mapping[source][destination] = options.channelMap->mMap[row + source][destination];
                    }
                }
                row += channels;
                node.mappedOutput = [this, controls, rate, mapping = std::move(mapping)](Buffer buffers, size_t frames, int64_t sample) {
                    if (sample < rawSample || static_cast<uint64_t>(sample - rawSample) + frames > capacity) {
                        throw std::out_of_range("The output map received audio outside its render packet.");
                    }
                    const auto offset = static_cast<size_t>(sample - rawSample);
                    for (size_t source = 0; source < mapping.size(); ++source) {
                        for (size_t destination = 0; destination < this->options.channels; ++destination) {
                            if (!mapping[source][destination]) {
                                continue;
                            }
                            for (size_t frame = 0; frame < frames; ++frame) {
                                const double time = (sample + static_cast<int64_t>(frame)) / rate;
                                floats[destination][offset + frame] += buffers[source][frame] * controls->gain(destination, time);
                            }
                        }
                    }
                };
            }
            auto effects = std::make_shared<NativeEffectChain>(options.trackEffects
                                                               ? NativeEffectChain::specifications(RealtimeEffectList::Get(*track),
                                                                                                   options.trackAutomation)
                                                               : std::vector<NativeEffectChain::Specification> {}, rate, 128,
                                                               wave && wave->NChannels() == 1 && node.routing.busId.empty() ? 1 : 2,
                                                               transport);
            node.effects = [effects](Buffer buffers, size_t frames, int64_t sample, const NativeOfflineGraph::Processor& input) {
                effects->pull(buffers, frames, sample, input);
            };
            nodes.push_back(std::move(node));
        }
        if (options.channelMap && row != options.channelMap->GetNumTracks()) {
            throw std::invalid_argument("The output map does not match the source channels.");
        }
        graph = std::make_unique<NativeOfflineGraph>(std::move(nodes), capacity, start);
        master
            = std::make_unique<NativeEffectChain>(std::move(
                                                      masterSpecifications), options.sampleRate, 128, options.channels == 1 ? 1 : 2,
                                                  transport);
    }

    void render(size_t frames)
    {
        for (auto& channel : floats) {
            std::fill_n(channel.data(), frames, 0.0f);
        }
        const Buffer buffers { stereo[0].data(), stereo[1].data() };
        master->pull(buffers, frames, rawSample, [this](Buffer input, size_t count, int64_t sample) {
            graph->process(input, count, sample);
            if (options.channels == 1) {
                for (size_t frame = 0; frame < count; ++frame) {
                    input[0][frame] = input[1][frame] = 0.5f * (input[0][frame] + input[1][frame]);
                }
            }
        });
        if (!options.channelMap) {
            for (size_t frame = 0; frame < frames; ++frame) {
                if (options.channels == 1) {
                    floats[0][frame] = 0.5f * (stereo[0][frame] + stereo[1][frame]);
                } else {
                    floats[0][frame] = stereo[0][frame];
                    floats[1][frame] = stereo[1][frame];
                }
            }
        }
        rawSample += static_cast<int64_t>(frames);
    }

    size_t process(size_t maximumFrames)
    {
        if (!maximumFrames || current >= stop) {
            return 0;
        }
        const auto count = std::min({ maximumFrames, options.bufferSize, static_cast<size_t>(std::max<int64_t>(0, stop - current)) });
        if (!count) {
            return 0;
        }
        // Codecs can require a complete requested packet except at EOF. Keep
        // per-track/graph work bounded, accumulating only the final public
        // output buffer across those internal chunks.
        for (size_t offset = 0; offset < count;) {
            const auto frames = std::min(capacity, count - offset);
            render(frames);
            const auto stride = options.interleaved ? options.channels : 1u;
            for (size_t channel = 0; channel < options.channels; ++channel) {
                const auto first = offset * stride + (options.interleaved ? channel : 0);
                auto* destination = output[options.interleaved ? 0 : channel].ptr() + first * SAMPLE_SIZE(options.format);
                CopySamples(reinterpret_cast<constSamplePtr>(floats[channel].data()), floatSample, destination,
                            options.format, frames, gHighQualityDither, 1, stride);
            }
            offset += frames;
        }
        current += count;
        return count;
    }

    Options options;
    int64_t start = 0, stop = 0, current = 0, rawSample = 0;
    size_t capacity = 0;
    std::vector<std::shared_ptr<const PlayableTrack> > tracks;
    std::array<std::vector<float>, 2> stereo;
    std::vector<std::vector<float> > floats;
    std::vector<SampleBuffer> output;
    std::unique_ptr<NativeOfflineGraph> graph;
    std::unique_ptr<NativeEffectChain> master;
};

NativeAudioRenderSource::NativeAudioRenderSource(const AudacityProject& project, const Tracks& tracks, const Options& options)
    : m_impl(std::make_unique<Impl>(project, tracks, options)) {}
NativeAudioRenderSource::~NativeAudioRenderSource() = default;
size_t NativeAudioRenderSource::Process() { return m_impl->process(m_impl->options.bufferSize); }
size_t NativeAudioRenderSource::Process(size_t maximumFrames) { return m_impl->process(maximumFrames); }
constSamplePtr NativeAudioRenderSource::GetBuffer() { return m_impl->output[0].ptr(); }
constSamplePtr NativeAudioRenderSource::GetBuffer(int channel) { return m_impl->output.at(channel).ptr(); }
double NativeAudioRenderSource::MixGetCurrentTime() { return m_impl->current / m_impl->options.sampleRate; }
sampleFormat NativeAudioRenderSource::EffectiveFormat() const { return floatSample; }
void registerNativeAudioRenderer()
{
    static AudioRenderSource::Factory::Scope factory {
        [](const AudacityProject& project, const AudioRenderSource::Tracks& tracks, const AudioRenderSource::Options& options) {
            return std::make_unique<NativeAudioRenderSource>(project, tracks, options);
        }
    };
}
}
