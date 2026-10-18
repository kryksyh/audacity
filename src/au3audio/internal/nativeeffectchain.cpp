/*
* Audacity: A Digital Audio Editor
*/
#include "nativeeffectchain.h"
#include <QCoreApplication>
#include <QMetaObject>
#include <QThread>
#include <exception>
#include "nativeeffectparameters.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

#include "au3-realtime-effects/RealtimeEffectList.h"
#include "au3-realtime-effects/RealtimeEffectState.h"

namespace au::au3audio {
namespace {
using Buffer = NativeRoutingGraph::Buffer;
using Stereo = std::array<std::vector<float>, 2>;

struct Stage {
    struct Binding {
        std::string parameter;
        NativeEffectParameterBinding target;
        double last = std::numeric_limits<double>::quiet_NaN();
    };
    struct Processor {
        std::shared_ptr<EffectInstance> instance;
        std::unique_ptr<EffectOutputs> outputs;
        std::vector<Binding> bindings;
        std::vector<const float*> inputs;
        std::vector<float*> result;
        std::vector<std::vector<float> > discardedOutputs;
        EffectSettings* settings = nullptr;
        size_t channel = 0;
        bool initialized = false;

        ~Processor()
        {
            if (initialized && instance && settings) {
                instance->RealtimeFinalize(*settings);
            }
        }
    };

    Stage(NativeEffectChain::Specification specification, double sampleRate, size_t preferredBlockSize, size_t channelCount,
          EffectProcessingContext context)
        : spec(std::move(specification)), rate(sampleRate), channels(channelCount), context(context)
    {
        if (!spec.effect) {
            throw std::invalid_argument("An effect in the render chain is not available.");
        }
        try {
            size_t channel = 0;
            while (channel < channels) {
                auto processor = std::make_unique<Processor>();
                processor->settings = &spec.settings;
                processor->instance = spec.effect->MakeInstance();
                if (!processor->instance) {
                    throw std::runtime_error("Could not create an Audacity effect instance.");
                }
                auto& instance = *processor->instance;
                instance.SetBlockSize(preferredBlockSize);
                if (!instance.RealtimeInitialize(spec.settings, rate, preferredBlockSize)) {
                    throw std::runtime_error("Could not initialize an Audacity realtime effect.");
                }
                processor->initialized = true;
                // Some native effects choose their own default in Initialize.
                const auto size = instance.SetBlockSize(preferredBlockSize);
                if (!size || (blockSize && size != blockSize)) {
                    throw std::runtime_error("The effect returned inconsistent audio block sizes.");
                }
                blockSize = size;
                const auto inputs = instance.GetAudioInCount(), outputs = instance.GetAudioOutCount();
                if (!inputs || !outputs || inputs > 256 || outputs > 256) {
                    throw std::runtime_error("The effect does not support audio insert processing.");
                }
                processor->outputs = spec.effect->MakeOutputs();
                if (!instance.RealtimeAddProcessor(spec.settings, processor->outputs.get(), std::min<size_t>(inputs, channels), rate)
                    || !instance.RealtimeResume()) {
                    throw std::runtime_error("Could not activate an Audacity realtime effect.");
                }
                processor->channel = channel;
                processor->inputs.resize(inputs);
                processor->result.resize(outputs);
                processor->discardedOutputs.assign(outputs, std::vector<float>(blockSize));
                for (const auto& lane : spec.automation.Lanes()) {
                    if (!lane.enabled || lane.points.empty()) {
                        continue;
                    }
                    auto binding = bindNativeEffectParameter(*spec.effect, spec.settings, instance, lane.parameter, rate);
                    if (binding.set) {
                        processor->bindings.push_back({ lane.parameter, std::move(binding) });
                    }
                }
                processors.push_back(std::move(processor));
                channel += std::min(inputs, outputs);
            }
            input = { std::vector<float>(blockSize), std::vector<float>(blockSize) };
            output = input;
            dummy.assign(blockSize, 0.0f);
            // Some plugins publish latency only after processing their controls.
            // Prime the selection's automated values before measuring its delay;
            // probing the manual preset would misalign the first rendered audio.
            run(static_cast<int64_t>(std::llround(static_cast<long double>(context.time) * rate)), true);
            // The probe can trigger a VST3 restart/AU property notification.
            // These private offline instances are quiescent here; publish their
            // refreshed delay before caching the routing compensation. Retain
            // queued follow-up reports, with a bound for an unstable plugin.
            for (const auto& processor : processors) {
                unsigned refreshes = 0;
                while (processor->instance->RealtimeLatencyUpdatePending()) {
                    if (++refreshes > 8) {
                        throw std::runtime_error("The effect's processing delay did not stabilize.");
                    }
                    processor->instance->RefreshRealtimeLatency();
                }
            }
            pluginLatency = processors.front()->instance->GetLatency(spec.settings, rate);
            for (const auto& processor : processors) {
                if (processor->instance->GetLatency(spec.settings, rate) != pluginLatency) {
                    throw std::runtime_error("The effect returned different latencies for its channels.");
                }
            }
            reset(0);
        } catch (...) {
            finalize();
            throw;
        }
    }

    ~Stage() { finalize(); }

    void finalize() noexcept
    {
        for (auto& processor : processors) {
            if (processor->initialized) {
                processor->instance->RealtimeFinalize(spec.settings);
                processor->initialized = false;
            }
        }
    }

    void reset(int64_t)
    {
        pullNext = std::numeric_limits<int64_t>::min();
        pullFrames = pullOffset = 0;
        forceMeasure = true;
        pending = 0;
        read = blockSize;
        nextInput = std::numeric_limits<int64_t>::min();
        for (auto& channel : input) {
            std::fill(channel.begin(), channel.end(), 0.0f);
        }
        for (auto& channel : output) {
            std::fill(channel.begin(), channel.end(), 0.0f);
        }
        for (auto& processor : processors) {
            processor->instance->RealtimeSuspend();
            processor->instance->RealtimeResume();
        }
    }

    size_t latency() const { return blockSize - 1 + pluginLatency; }

    void run(int64_t sample, bool automated)
    {
        controlsChanged = false;
        for (auto& channel : output) {
            std::fill(channel.begin(), channel.end(), 0.0f);
        }
        for (auto& processor : processors) {
            auto apply = [&](bool before) {
                if (!automated) {
                    return;
                }
                for (auto& binding : processor->bindings) {
                    if (binding.target.beforeProcessStart == before) {
                        const auto value = spec.automation.ValueAt(binding.parameter, sample / rate, 0.0);
                        controlsChanged |= binding.last != value;
                        binding.last = value;
                        binding.target.set(value);
                    }
                }
            };
            auto& instance = *processor->instance;
            apply(true);
            EffectInstance::MessagePackage package { spec.settings, nullptr };
            if (!instance.RealtimeProcessStart(package)) {
                throw std::runtime_error("The effect could not begin an audio block.");
            }
            try {
                apply(false);
                auto transport = context;
                transport.time = sample / rate;
                transport.sampleRate = rate;
                instance.SetRealtimeProcessingContext(0, transport);
                for (size_t i = 0; i < processor->inputs.size(); ++i) {
                    const auto channel = processor->channel + i;
                    processor->inputs[i] = channel < channels ? input[channel].data() : dummy.data();
                }
                for (size_t i = 0; i < processor->result.size(); ++i) {
                    const auto channel = processor->channel + i;
                    processor->result[i] = channel < channels ? output[channel].data() : processor->discardedOutputs[i].data();
                }
                if (instance.RealtimeProcess(0, spec.settings, processor->inputs.data(), processor->result.data(),
                                             blockSize) != blockSize) {
                    throw std::runtime_error("The effect did not produce the requested audio block.");
                }
            } catch (...) {
                instance.RealtimeProcessEnd(spec.settings);
                throw;
            }
            if (!instance.RealtimeProcessEnd(spec.settings)) {
                throw std::runtime_error("The effect could not finish an audio block.");
            }
        }
        if (channels == 1) {
            std::copy(output[0].begin(), output[0].end(), output[1].begin());
        }
    }

    void process(Buffer buffers, size_t frames, int64_t sample)
    {
        if (sample != nextInput) {
            // Align each plugin's blocks in content time, including negative
            // pre-roll. Otherwise every upstream latency shifts automation's
            // evaluation grid by another block.
            const auto width = static_cast<int64_t>(blockSize);
            pending = static_cast<size_t>((sample % width + width) % width);
            blockSample = sample - static_cast<int64_t>(pending);
            read = blockSize;
            for (auto& channel : input) {
                std::fill(channel.begin(), channel.end(), 0.0f);
            }
            for (auto& channel : output) {
                std::fill(channel.begin(), channel.end(), 0.0f);
            }
        }
        for (size_t frame = 0; frame < frames; ++frame) {
            if (!pending) {
                blockSample = sample + static_cast<int64_t>(frame);
            }
            for (size_t channel = 0; channel < 2; ++channel) {
                input[channel][pending] = buffers[channel][frame];
            }
            if (++pending == blockSize) {
                run(blockSample, true);
                pending = 0;
                read = 0;
            }
            for (size_t channel = 0; channel < 2; ++channel) {
                buffers[channel][frame] = read < blockSize ? output[channel][read] : 0.0f;
            }
            ++read;
        }
        nextInput = sample + static_cast<int64_t>(frames);
    }

    size_t measureCompletedBlock()
    {
        const bool measure = forceMeasure || controlsChanged
                             || std::any_of(processors.begin(), processors.end(), [](const auto& p) {
            return p->instance->RealtimeLatencyUpdatePending();
        });
        const bool mainRequired = measure && std::any_of(processors.begin(), processors.end(), [](const auto& p) {
            return p->instance->LatencyRefreshRequiresMainThread();
        });
        size_t result = pullLatency;
        size_t following = pullLatency;
        std::exception_ptr failure;
        const auto operation = [&] {
            bool first = true;
            for (const auto& processor : processors) {
                auto& instance = *processor->instance;
                const auto reported = instance.LatencyRefreshRequiresMainThread() && !measure
                                      ? pullLatency : instance.GetReportedLatency(spec.settings, rate);
                if (reported > static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) {
                    throw std::runtime_error("The effect's processing delay is out of range.");
                }
                if (!first && result != reported) {
                    throw std::runtime_error("The effect returned different latencies for its channels.");
                }
                first = false;
                result = static_cast<size_t>(reported);
            }
            // Measure before reactivation: a restart may change the upcoming
            // delay, but cannot retag audio that has already been rendered.
            bool refreshed = false;
            for (const auto& processor : processors) {
                unsigned attempts = 0;
                while (processor->instance->RealtimeLatencyUpdatePending()) {
                    if (++attempts > 8) {
                        throw std::runtime_error("The effect's processing delay did not stabilize.");
                    }
                    processor->instance->RefreshRealtimeLatency();
                    refreshed = true;
                }
            }
            following = result;
            if (refreshed) {
                bool firstUpcoming = true;
                for (const auto& processor : processors) {
                    const auto upcoming = processor->instance->GetLatency(spec.settings, rate);
                    if (upcoming > static_cast<uint64_t>(std::numeric_limits<int64_t>::max())
                        || (!firstUpcoming && following != upcoming)) {
                        throw std::runtime_error("The effect returned an invalid upcoming processing delay.");
                    }
                    firstUpcoming = false;
                    following = static_cast<size_t>(upcoming);
                }
            }
        };
        auto* application = QCoreApplication::instance();
        if (mainRequired && application && application->thread() != QThread::currentThread()) {
            const bool invoked = QMetaObject::invokeMethod(application, [&] {
                try { operation();
                } catch (...) { failure = std::current_exception();
                }
            }, Qt::BlockingQueuedConnection);
            if (!invoked) {
                throw std::runtime_error("Cannot refresh native effect delay on the application thread.");
            }
            if (failure) {
                std::rethrow_exception(failure);
            }
        } else {
            operation();
        }
        forceMeasure = false;
        pullLatency = following;
        return result;
    }

    void pull(Buffer buffers, size_t frames, int64_t sample, const NativeRoutingGraph::Processor& upstream)
    {
        if (!frames) {
            return;
        }
        if (frames > static_cast<size_t>(std::numeric_limits<int64_t>::max())
            || sample > std::numeric_limits<int64_t>::max() - static_cast<int64_t>(frames)) {
            throw std::out_of_range("Effect render position is out of range.");
        }
        if (sample != pullNext) {
            reset(sample);
            pullRenderNext = sample;
        }
        size_t written = 0;
        while (written < frames) {
            if (pullOffset == pullFrames) {
                if (blockSize > static_cast<size_t>(std::numeric_limits<int64_t>::max())
                    || pullRenderNext > std::numeric_limits<int64_t>::max() - static_cast<int64_t>(blockSize)) {
                    throw std::out_of_range("Effect input position is out of range.");
                }
                upstream({ input[0].data(), input[1].data() }, blockSize, pullRenderNext);
                run(pullRenderNext, true);
                const auto delay = measureCompletedBlock();
                if (pullRenderNext < std::numeric_limits<int64_t>::min() + static_cast<int64_t>(delay)) {
                    throw std::out_of_range("Effect latency position is out of range.");
                }
                pullPacketStart = pullRenderNext - static_cast<int64_t>(delay);
                pullRenderNext += static_cast<int64_t>(blockSize);
                pullFrames = blockSize;
                pullOffset = 0;
            }
            auto position = pullPacketStart + static_cast<int64_t>(pullOffset);
            if (position < sample) {
                const auto overlap = static_cast<uint64_t>(sample) - static_cast<uint64_t>(position);
                pullOffset += static_cast<size_t>(std::min<uint64_t>(overlap, pullFrames - pullOffset));
                if (pullOffset == pullFrames) {
                    continue;
                }
                position = pullPacketStart + static_cast<int64_t>(pullOffset);
            }
            const bool gap = position > sample;
            const auto available = gap ? static_cast<uint64_t>(position) - static_cast<uint64_t>(sample) : pullFrames - pullOffset;
            const auto count = static_cast<size_t>(std::min<uint64_t>(available, frames - written));
            for (size_t channel = 0; channel < 2; ++channel) {
                if (gap) {
                    std::fill_n(buffers[channel] + written, count, 0.0f);
                } else {
                    std::copy_n(output[channel].data() + pullOffset, count, buffers[channel] + written);
                }
            }
            if (!gap) {
                pullOffset += count;
            }
            sample += static_cast<int64_t>(count);
            written += count;
        }
        pullNext = sample;
    }

    NativeEffectChain::Specification spec;
    double rate;
    size_t channels;
    EffectProcessingContext context;
    size_t blockSize = 0, pluginLatency = 0, pending = 0, read = 0;
    size_t pullLatency = 0, pullFrames = 0, pullOffset = 0;
    int64_t pullNext = std::numeric_limits<int64_t>::min(), pullRenderNext = 0, pullPacketStart = 0;
    bool controlsChanged = false, forceMeasure = true;
    int64_t blockSample = 0;
    int64_t nextInput = std::numeric_limits<int64_t>::min();
    Stereo input, output;
    std::vector<float> dummy;
    std::vector<std::unique_ptr<Processor> > processors;
};
}

struct NativeEffectChain::Impl {
    std::vector<std::unique_ptr<Stage> > stages;
    size_t latency = 0;
    void pull(size_t count, Buffer buffers, size_t frames, int64_t sample, const NativeRoutingGraph::Processor& input)
    {
        if (!count) {
            input(buffers, frames, sample);
            return;
        }
        stages[count - 1]->pull(buffers, frames, sample, [&](Buffer destination, size_t length, int64_t position) {
            pull(count - 1, destination, length, position, input);
        });
    }
};

std::vector<NativeEffectChain::Specification> NativeEffectChain::specifications(const RealtimeEffectList& list, bool automation)
{
    std::vector<Specification> result;
    list.Visit([&](const auto& state, bool active) {
        if (!active) {
            return;
        }
        auto* effect = const_cast<RealtimeEffectState&>(state).GetEffect();
        if (!effect) {
            if (!state.IsEnabled()) {
                return;
            }
            throw std::runtime_error("An effect required for rendering is not available.");
        }
        // Get() includes edits not yet acknowledged by a playback worker.
        // Snapshot on the main thread without waiting for an audio callback.
        const auto access = const_cast<RealtimeEffectState&>(state).GetAccess();
        const auto& settings = access->Get();
        if (!settings.extra.GetActive()) {
            return;
        }
        result.push_back({ effect, settings, automation ? state.Automation() : AutomationData {} });
    });
    return result;
}

NativeEffectChain::NativeEffectChain(std::vector<Specification> effects, double rate, size_t preferredBlockSize, size_t channels,
                                     const EffectProcessingContext& context)
    : m_impl(std::make_unique<Impl>())
{
    if (!preferredBlockSize || !std::isfinite(rate) || rate <= 0.0 || channels < 1 || channels > 2
        || !std::isfinite(context.time)
        || std::abs(static_cast<long double>(context.time) * rate) >= std::numeric_limits<int64_t>::max()) {
        throw std::invalid_argument("Invalid native effect processing format.");
    }
    for (auto& effect : effects) {
        auto stage = std::make_unique<Stage>(std::move(effect), rate, preferredBlockSize, channels, context);
        if (stage->latency() > std::numeric_limits<size_t>::max() - m_impl->latency) {
            throw std::overflow_error("Effect latency is out of range.");
        }
        m_impl->latency += stage->latency();
        m_impl->stages.push_back(std::move(stage));
    }
}

NativeEffectChain::~NativeEffectChain() = default;
void NativeEffectChain::pull(Buffer buffers, size_t frames, int64_t sample, const NativeRoutingGraph::Processor& input)
{
    m_impl->pull(m_impl->stages.size(), buffers, frames, sample, input);
}

void NativeEffectChain::process(NativeRoutingGraph::Buffer buffers, size_t frames, int64_t sample)
{
    size_t preceding = 0;
    for (auto& stage : m_impl->stages) {
        stage->process(buffers, frames, sample - static_cast<int64_t>(preceding));
        preceding += stage->latency();
    }
}

void NativeEffectChain::reset(int64_t sample)
{
    for (auto& stage : m_impl->stages) {
        stage->reset(sample);
    }
}

size_t NativeEffectChain::latency() const { return m_impl->latency; }
}
