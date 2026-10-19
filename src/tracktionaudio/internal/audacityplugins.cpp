/*
* Audacity: A Digital Audio Editor
*/
#include <tracktion_engine/tracktion_engine.h>

#include "audacityplugins.h"

#include <array>

#include "au3-mixer/Envelope.h"
#include "au3-playable-track/TrackAutomation.h"
#include "audacityaudiosource.h"
#include "audiosourceregistry.h"

using namespace au::tracktionaudio;

namespace {
class AudacityPlaybackControlsPlugin final : public tracktion::Plugin
{
public:
    inline static const auto xmlTypeName = AUDACITY_PLAYBACK_CONTROLS_PLUGIN_TYPE;

    explicit AudacityPlaybackControlsPlugin(tracktion::PluginCreationInfo info)
        : tracktion::Plugin(info), controls(std::make_shared<AudacityPlaybackControls>())
    {
    }

    juce::String getName() const override { return "Audacity Playback Controls"; }
    juce::String getSelectableDescription() override { return getName(); }
    juce::String getPluginType() override { return xmlTypeName; }
    void initialise(const tracktion::PluginInitialisationInfo&) override {}
    void deinitialise() override {}
    int getNumOutputChannelsGivenInputs(int inputs) override { return inputs; }
    BusLayout getBusses() const override { return BusLayout::singlePassThrough(); }
    bool producesAudioWhenNoAudioInput() override { return true; }
    bool canBeDisabled() override { return false; }

    void applyToBuffer(const tracktion::PluginRenderContext& context) override
    {
        if (!context.destBuffer) {
            return;
        }
        if (edit.getTransport().isPaused()) {
            context.destBuffer->clear(context.bufferStartSample, context.bufferNumSamples);
            return;
        }
        const double requestedEnd = controls->endTime.load(std::memory_order_relaxed);
        const double endTime = std::min(requestedEnd, controls->silentLoopStart.load(std::memory_order_relaxed));
        int audibleSamples = context.bufferNumSamples;
        if (context.isPlaying && endTime <= context.getEditTimeAtSample(context.bufferNumSamples).inSeconds()) {
            int first = 0;
            int last = context.bufferNumSamples;
            while (first < last) {
                const int middle = first + (last - first) / 2;
                if (context.getEditTimeAtSample(middle).inSeconds() < endTime) {
                    first = middle + 1;
                } else {
                    last = middle;
                }
            }
            audibleSamples = first;
            context.destBuffer->clear(context.bufferStartSample + audibleSamples, context.bufferNumSamples - audibleSamples);
            if (requestedEnd <= endTime) {
                int unset = -1;
                controls->endSampleOffset.compare_exchange_strong(unset, audibleSamples, std::memory_order_release);
            }
        }
        context.destBuffer->applyGain(context.bufferStartSample, audibleSamples, controls->volume.load(std::memory_order_relaxed));
    }

    const std::shared_ptr<AudacityPlaybackControls> controls;
};

class AudacityTrackControlsPlugin final : public tracktion::Plugin
{
public:
    inline static const auto xmlTypeName = AUDACITY_TRACK_CONTROLS_PLUGIN_TYPE;

    explicit AudacityTrackControlsPlugin(tracktion::PluginCreationInfo info)
        : tracktion::Plugin(info)
    {
        m_leftGain.referTo(state, AUDACITY_LEFT_GAIN_PROPERTY, getUndoManager(), 1.0f);
        m_rightGain.referTo(state, AUDACITY_RIGHT_GAIN_PROPERTY, getUndoManager(), 1.0f);
        m_volume.referTo(state, AUDACITY_VOLUME_PROPERTY, getUndoManager(), 1.0f);
        m_pan.referTo(state, AUDACITY_PAN_PROPERTY, getUndoManager(), 0.0f);
        addAutomatableParameter(m_volumeParameter
                                    = new tracktion::AutomatableParameter("volume", "Volume", *this,
                                                                          { 0.0f, static_cast<float>(TrackAutomation::MaximumVolume) }));
        addAutomatableParameter(m_panParameter = new tracktion::AutomatableParameter("pan", "Pan", *this, { -1.0f, 1.0f }));
    }

    juce::String getName() const override { return "Audacity Track Controls"; }
    juce::String getSelectableDescription() override { return getName(); }
    juce::String getPluginType() override { return xmlTypeName; }
    void initialise(const tracktion::PluginInitialisationInfo& info) override
    {
        for (auto& gain : m_gains) {
            gain.reset(info.sampleRate, 0.02);
        }
        m_gains[0].setCurrentAndTargetValue(m_leftGain.get());
        m_gains[1].setCurrentAndTargetValue(m_rightGain.get());
    }

    void deinitialise() override {}
    int getNumOutputChannelsGivenInputs(int inputs) override { return inputs; }
    BusLayout getBusses() const override { return BusLayout::singlePassThrough(); }
    bool producesAudioWhenNoAudioInput() override { return false; }

    void applyToBuffer(const tracktion::PluginRenderContext& context) override
    {
        if (!context.destBuffer) {
            return;
        }

        m_gains[0].setTargetValue(m_leftGain.get());
        m_gains[1].setTargetValue(m_rightGain.get());
        const bool automated = m_volumeParameter->hasAutomationPoints() || m_panParameter->hasAutomationPoints();
        for (int sample = 0; sample < context.bufferNumSamples; ++sample) {
            float gains[] = { m_gains[0].getNextValue(), m_gains[1].getNextValue() };
            if (automated) {
                const auto time = context.getEditTimeAtSample(sample);
                const float volume = m_volumeParameter->getCurve().getValueAt(time, m_volume.get());
                const float pan = m_panParameter->getCurve().getValueAt(time, m_pan.get());
                gains[0] = volume * (pan > 0.0f ? 1.0f - pan : 1.0f);
                gains[1] = volume * (pan < 0.0f ? 1.0f + pan : 1.0f);
            }
            for (int channel = 0; channel < context.destBuffer->getNumChannels(); ++channel) {
                auto* samples = context.destBuffer->getWritePointer(channel, context.bufferStartSample);
                samples[sample] *= gains[channel % 2];
            }
        }
    }

private:
    juce::CachedValue<tracktion::AtomicWrapper<float> > m_leftGain;
    juce::CachedValue<tracktion::AtomicWrapper<float> > m_rightGain;
    std::array<juce::LinearSmoothedValue<float>, 2> m_gains;
    juce::CachedValue<tracktion::AtomicWrapper<float> > m_volume, m_pan;
    tracktion::AutomatableParameter::Ptr m_volumeParameter, m_panParameter;
};

class AudacityEnvelopePlugin final : public tracktion::Plugin
{
public:
    AudacityEnvelopePlugin(tracktion::PluginCreationInfo info, std::shared_ptr<const AudacityAudioSource> source)
        : tracktion::Plugin(info), m_source(std::move(source))
    {
    }

    juce::String getName() const override { return "Audacity Envelope"; }
    juce::String getSelectableDescription() override { return getName(); }
    juce::String getPluginType() override { return AUDACITY_ENVELOPE_PLUGIN_TYPE; }
    void initialise(const tracktion::PluginInitialisationInfo&) override {}
    void deinitialise() override {}
    int getNumOutputChannelsGivenInputs(int inputs) override { return inputs; }
    BusLayout getBusses() const override { return BusLayout::singlePassThrough(); }
    bool producesAudioWhenNoAudioInput() override { return false; }

    void applyToBuffer(const tracktion::PluginRenderContext& context) override
    {
        if (!context.destBuffer || context.bufferNumSamples == 0) {
            return;
        }

        const auto envelope = m_source->envelope();
        if (envelope->IsTrivial()) {
            return;
        }

        std::array<double, 1024> gains;
        const double step = context.editTime.getLength().inSeconds() / context.bufferNumSamples;
        for (int offset = 0; offset < context.bufferNumSamples; offset += static_cast<int>(gains.size())) {
            const int count = std::min<int>(gains.size(), context.bufferNumSamples - offset);
            envelope->GetValues(gains.data(), count, context.editTime.getStart().inSeconds() + offset * step, step);
            for (int channel = 0; channel < context.destBuffer->getNumChannels(); ++channel) {
                auto* samples = context.destBuffer->getWritePointer(channel, context.bufferStartSample + offset);
                for (int sample = 0; sample < count; ++sample) {
                    samples[sample] *= static_cast<float>(gains[sample]);
                }
            }
        }
    }

private:
    const std::shared_ptr<const AudacityAudioSource> m_source;
};

class AudacityEnvelopePluginType final : public tracktion::PluginManager::BuiltInType
{
public:
    explicit AudacityEnvelopePluginType(std::shared_ptr<AudioSourceRegistry> sources)
        : BuiltInType(AUDACITY_ENVELOPE_PLUGIN_TYPE), m_sources(std::move(sources))
    {
    }

    tracktion::Plugin::Ptr create(tracktion::PluginCreationInfo info) override
    {
        const auto source = m_sources->find(info.state.getProperty(AUDACITY_SOURCE_ID_PROPERTY).toString());
        return source ? new AudacityEnvelopePlugin(info, source) : nullptr;
    }

private:
    std::shared_ptr<AudioSourceRegistry> m_sources;
};
}

void au::tracktionaudio::registerAudacityPlugins(tracktion::Engine& engine, const std::shared_ptr<AudioSourceRegistry>& sources)
{
    engine.getPluginManager().createBuiltInType<AudacityTrackControlsPlugin>();
    engine.getPluginManager().createBuiltInType<AudacityPlaybackControlsPlugin>();
    engine.getPluginManager().registerBuiltInType(std::make_unique<AudacityEnvelopePluginType>(sources));
}

std::shared_ptr<AudacityPlaybackControls> au::tracktionaudio::audacityPlaybackControls(tracktion::Plugin& plugin)
{
    return dynamic_cast<AudacityPlaybackControlsPlugin&>(plugin).controls;
}
