/*
* Audacity: A Digital Audio Editor
*/
#include <tracktion_engine/tracktion_engine.h>
#include <tracktion_graph/tracktion_graph.h>

#include "audacityeffectregistry.h"
#include "audacityexternaleffect.h"

#include <stdexcept>

#include "au3-components/ComponentInterfaceSymbol.h"
#include "au3-realtime-effects/RealtimeEffectList.h"
#include "au3-realtime-effects/RealtimeEffectState.h"

using namespace au::tracktionaudio;

struct AudacityEffectRegistry::Binding
{
    Binding(std::shared_ptr<RealtimeEffectState> state, const RealtimeEffectList& effects, const ChannelGroup* owner, int channelCount,
            bool followListChanges)
        : effect(std::move(state)), list(effects), group(owner), channels(channelCount),
        live(followListChanges), activeSnapshot(effects.IsActive())
    {
    }

    std::shared_ptr<RealtimeEffectState> effect;
    const RealtimeEffectList& list;
    const ChannelGroup* group;
    int channels;
    bool live;
    bool activeSnapshot;
};

namespace {
class AudacityEffectPlugin final : public tracktion::Plugin
{
public:
    AudacityEffectPlugin(tracktion::PluginCreationInfo info, std::shared_ptr<AudacityEffectRegistry::Binding> binding)
        : tracktion::Plugin(info), m_binding(std::move(binding))
    {
    }

    juce::String getName() const override { return "Audacity Effect"; }
    juce::String getSelectableDescription() override { return getName(); }
    juce::String getPluginType() override { return AUDACITY_EFFECT_PLUGIN_TYPE; }
    bool canBeDisabled() override { return false; }
    bool producesAudioWhenNoAudioInput() override { return true; }
    bool noTail() override { return false; }
    int getNumOutputChannelsGivenInputs(int) override { return m_binding->channels; }
    BusLayout getBusses() const override
    {
        const auto channels = tracktion::ChannelConfiguration::discreteChannels(m_binding->channels);
        return BusLayout::singleInOut(channels, channels);
    }

    void initialise(const tracktion::PluginInitialisationInfo& info) override
    {
        auto& effect = *m_binding->effect;
        m_instance = effect.Initialize(info.sampleRate, info.blockSizeSamples);
        if (!m_instance) {
            throw std::runtime_error("Cannot initialise an Audacity effect.");
        }
        if (!effect.AddGroup(m_binding->group, m_binding->channels, info.sampleRate, info.blockSizeSamples)) {
            effect.Finalize();
            m_instance.reset();
            throw std::runtime_error("Cannot initialise an Audacity effect processor.");
        }
        m_sampleRate = info.sampleRate;
        m_blockSize = info.blockSizeSamples;
        m_scratch.setSize(m_binding->channels + 1, m_blockSize);
        m_inputs.resize(m_binding->channels);
        m_outputs.resize(m_binding->channels);
        m_latency = static_cast<int>(m_instance->GetLatency(effect.GetSettings(), m_sampleRate));
        m_bypassDelay.setLatencyNumSamples(m_latency);
        m_bypassDelay.prepareToPlay(m_sampleRate, m_blockSize, m_binding->channels);
    }

    void deinitialise() override
    {
        if (m_instance) {
            m_binding->effect->Finalize();
        }
        m_instance.reset();
    }

    double getLatencySeconds() override { return m_latency / m_sampleRate; }
    double getTailLength() const override { return m_instance ? m_instance->GetTailSize() / m_sampleRate : 0.0; }

    void applyToBuffer(const tracktion::PluginRenderContext& context) override
    {
        if (!context.destBuffer || !m_instance) {
            return;
        }
        auto& effect = *m_binding->effect;
        const bool listActive = m_binding->live ? m_binding->list.IsActive() : m_binding->activeSnapshot;
        const bool running = listActive && (context.isPlaying || context.isRendering || context.isScrubbing);
        effect.ProcessStart(running);
        const bool active = running && effect.IsActive();
        for (int offset = 0; offset < context.bufferNumSamples; offset += m_blockSize) {
            const int count = std::min(m_blockSize, context.bufferNumSamples - offset);
            const int start = context.bufferStartSample + offset;
            for (int channel = 0; channel < m_binding->channels; ++channel) {
                m_inputs[channel] = context.destBuffer->getReadPointer(channel, start);
                m_outputs[channel] = m_scratch.getWritePointer(channel);
            }
            juce::AudioBuffer<float> audio(context.destBuffer->getArrayOfWritePointers(), m_binding->channels, start, count);
            const auto view = tracktion::graph::toBufferView(audio);
            if (m_latency > 0) {
                m_bypassDelay.writeAudio(view);
            }
            effect.Process(m_binding->group, m_binding->channels, m_inputs.data(), m_outputs.data(),
                           m_scratch.getWritePointer(m_binding->channels), count);
            if (!active && m_latency > 0) {
                m_bypassDelay.readAudioOverwriting(view);
            } else {
                for (int channel = 0; channel < m_binding->channels; ++channel) {
                    audio.copyFrom(channel, 0, m_outputs[channel], count);
                }
                if (m_latency > 0) {
                    m_bypassDelay.clearAudio(count);
                }
            }
        }
        effect.ProcessEnd();
    }

private:
    std::shared_ptr<AudacityEffectRegistry::Binding> m_binding;
    std::shared_ptr<EffectInstance> m_instance;
    juce::AudioBuffer<float> m_scratch;
    std::vector<const float*> m_inputs;
    std::vector<float*> m_outputs;
    tracktion::graph::LatencyProcessor m_bypassDelay;
    double m_sampleRate = 44100.0;
    int m_blockSize = 512;
    int m_latency = 0;
};

class AudacityEffectPluginType final : public tracktion::PluginManager::BuiltInType
{
public:
    explicit AudacityEffectPluginType(std::shared_ptr<AudacityEffectRegistry> registry)
        : BuiltInType(AUDACITY_EFFECT_PLUGIN_TYPE), m_registry(std::move(registry))
    {
    }

    tracktion::Plugin::Ptr create(tracktion::PluginCreationInfo info) override
    {
        const auto binding = m_registry->find(info.state.getProperty(AUDACITY_EFFECT_STATE_PROPERTY).toString());
        if (binding && supportsAudacityExternalEffect(*binding->effect->GetEffect())) {
            return createAudacityExternalEffect(info, binding->effect, binding->list, binding->channels, binding->live);
        }
        return binding ? new AudacityEffectPlugin(info, binding) : nullptr;
    }

private:
    std::shared_ptr<AudacityEffectRegistry> m_registry;
};
}

juce::String AudacityEffectRegistry::add(const std::shared_ptr<RealtimeEffectState>& state, const RealtimeEffectList& list,
                                         const ChannelGroup* group, int channels, bool followListChanges)
{
    const auto* effect = state->GetEffect();
    if (!effect || !effect->SupportsRealtime()
        || (effect->GetFamily().Internal() != wxT("Audacity") && !supportsAudacityExternalEffect(*effect))) {
        return {};
    }
    const auto id = juce::Uuid().toString();
    m_bindings.emplace(id, std::make_shared<Binding>(state, list, group, channels, followListChanges));
    return id;
}

void AudacityEffectRegistry::remove(const juce::String& id)
{
    m_bindings.erase(id);
}

std::shared_ptr<AudacityEffectRegistry::Binding> AudacityEffectRegistry::find(const juce::String& id) const
{
    const auto binding = m_bindings.find(id);
    return binding == m_bindings.end() ? nullptr : binding->second;
}

void au::tracktionaudio::registerAudacityEffectPlugins(tracktion::Engine& engine, const std::shared_ptr<AudacityEffectRegistry>& registry)
{
    engine.getPluginManager().registerBuiltInType(std::make_unique<AudacityEffectPluginType>(registry));
}
