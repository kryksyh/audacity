/*
* Audacity: A Digital Audio Editor
*/
#include <juce_audio_processors/juce_audio_processors.h>
#include <array>

namespace {
constexpr int INPUT_CHANNELS = AU_TEST_PLUGIN_CHANNEL_MODE == 4 ? 0 : AU_TEST_PLUGIN_CHANNEL_MODE == 1
                               || AU_TEST_PLUGIN_CHANNEL_MODE == 3 ? 1 : 2;
constexpr int OUTPUT_CHANNELS = AU_TEST_PLUGIN_CHANNEL_MODE == 1 ? 1 : 2;

class TestProcessor final : public juce::AudioProcessor
{
public:
    TestProcessor()
        : AudioProcessor(AU_TEST_PLUGIN_CHANNEL_MODE == 4 ? BusesProperties().withOutput("Output", juce::AudioChannelSet::stereo(), true)
                         : BusesProperties().withInput("Input", juce::AudioChannelSet::canonicalChannelSet(INPUT_CHANNELS), true)
                         .withOutput("Output", juce::AudioChannelSet::canonicalChannelSet(OUTPUT_CHANNELS), true))
    {
        m_gain = new juce::AudioParameterFloat({ "gain", 1 }, "Gain", juce::NormalisableRange<float> { 0.0f, 1.0f }, 0.5f);
        addParameter(m_gain);
        juce::NormalisableRange<float> frequencyRange { 20.0f, 20000.0f };
        frequencyRange.setSkewForCentre(1000.0f);
        addParameter(new juce::AudioParameterFloat({ "frequency", 1 }, "Frequency", frequencyRange, 1000.0f,
                                                   juce::AudioParameterFloatAttributes().withLabel("Hz").withAutomatable(false)));
        addParameter(new juce::AudioParameterChoice({ "mode", 1 }, "Mode", { "A", "B", "C" }, 0));
        const auto meter = juce::AudioParameterFloatAttributes().withCategory(juce::AudioProcessorParameter::otherMeter);
        m_output = new juce::AudioParameterFloat({ "output", 1 }, "Output", juce::NormalisableRange<float> { 0.0f, 1.0f }, 0.0f, meter);
        m_restores = new juce::AudioParameterFloat({ "restores", 1 }, "State Restores",
                                                   juce::NormalisableRange<float> { 0.0f, 1024.0f }, 0.0f, meter);
        addParameter(m_output);
        addParameter(m_restores);
        m_stateReads = new juce::AudioParameterFloat({ "state_reads", 1 }, "State Reads",
                                                     juce::NormalisableRange<float> { 0.0f, 1024.0f }, 0.0f, meter);
        addParameter(m_stateReads);
        setLatencySamples(AU_TEST_PLUGIN_CHANNEL_MODE == 4 ? 0 : 33);
    }

    const juce::String getName() const override { return JucePlugin_Name; }
    bool acceptsMidi() const override { return AU_TEST_PLUGIN_CHANNEL_MODE == 4; }
    bool producesMidi() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }
    bool hasEditor() const override { return false; }
    juce::AudioProcessorEditor* createEditor() override { return nullptr; }
    int getNumPrograms() override { return 2; }
    int getCurrentProgram() override { return m_program; }
    void setCurrentProgram(int program) override
    {
        m_program = program;
        m_gain->setValueNotifyingHost(program == 0 ? 0.5f : 0.25f);
    }

    const juce::String getProgramName(int program) override { return program == 0 ? "Half" : "Quarter"; }
    void changeProgramName(int, const juce::String&) override {}
    bool isBusesLayoutSupported(const BusesLayout& layout) const override
    {
        const auto output = layout.getMainOutputChannelSet();
        const auto input = layout.getMainInputChannelSet();
        if (AU_TEST_PLUGIN_CHANNEL_MODE == 0) {
            return (output == juce::AudioChannelSet::mono() || output == juce::AudioChannelSet::stereo()) && output == input;
        }
        return input.size() == INPUT_CHANNELS && output.size() == OUTPUT_CHANNELS;
    }

    void prepareToPlay(double, int) override
    {
        m_delay.setSize(2, 33);
        m_delay.clear();
        m_position = 0;
        m_notes.fill(0.0f);
        m_held.fill(false);
        m_sustain = false;
    }

    void releaseResources() override {}
    void processBlock(juce::AudioBuffer<float>& audio, juce::MidiBuffer& midi) override
    {
        const float gain = m_gain->get();
        if (AU_TEST_PLUGIN_CHANNEL_MODE == 4) {
            audio.clear();
            auto event = midi.cbegin();
            for (int sample = 0; sample < audio.getNumSamples(); ++sample) {
                while (event != midi.cend() && (*event).samplePosition <= sample) {
                    const auto message = (*event).getMessage();
                    if (message.isNoteOn()) {
                        m_notes[message.getNoteNumber()] = message.getFloatVelocity();
                        m_held[message.getNoteNumber()] = true;
                    } else if (message.isNoteOff()) {
                        m_held[message.getNoteNumber()] = false;
                        if (!m_sustain) {
                            m_notes[message.getNoteNumber()] = 0.0f;
                        }
                    } else if (message.isSustainPedalOn() || message.isSustainPedalOff()) {
                        m_sustain = message.isSustainPedalOn();
                        if (!m_sustain) {
                            for (size_t note = 0; note < m_notes.size(); ++note) {
                                if (!m_held[note]) {
                                    m_notes[note] = 0.0f;
                                }
                            }
                        }
                    } else if (message.isAllNotesOff() || message.isAllSoundOff()) {
                        m_notes.fill(0.0f);
                        m_held.fill(false);
                    }
                    ++event;
                }
                float value = 0.0f;
                for (float velocity : m_notes) {
                    value += velocity * gain;
                }
                for (int channel = 0; channel < audio.getNumChannels(); ++channel) {
                    audio.setSample(channel, sample, value);
                }
            }
            return;
        }
        for (int sample = 0; sample < audio.getNumSamples(); ++sample) {
            const float monoInput = audio.getSample(0, sample);
            for (int channel = 0; channel < audio.getNumChannels(); ++channel) {
                const float input = AU_TEST_PLUGIN_CHANNEL_MODE == 3 && channel == 1 ? -monoInput
                                    : AU_TEST_PLUGIN_CHANNEL_MODE == 2 && channel == 1 ? -audio.getSample(channel, sample)
                                    : audio.getSample(channel, sample);
                audio.setSample(channel, sample, m_delay.getSample(channel, m_position) * gain);
                m_delay.setSample(channel, m_position, input);
            }
            m_position = (m_position + 1) % 33;
        }
        m_output->setValueNotifyingHost(audio.getMagnitude(0, audio.getNumSamples()));
    }

    void getStateInformation(juce::MemoryBlock& data) override
    {
        m_stateReads->setValueNotifyingHost(m_stateReads->convertTo0to1(static_cast<float>(++m_stateReadCount)));
        juce::MemoryOutputStream stream(data, false);
        stream.writeInt(m_program);
        stream.writeFloat(m_gain->get());
    }

    void setStateInformation(const void* data, int size) override
    {
        m_restores->setValueNotifyingHost(m_restores->convertTo0to1(static_cast<float>(++m_restoreCount)));
        juce::MemoryInputStream stream(data, static_cast<size_t>(size), false);
        m_program = stream.readInt();
        m_gain->setValueNotifyingHost(stream.readFloat());
    }

private:
    juce::AudioParameterFloat* m_gain = nullptr;
    juce::AudioParameterFloat* m_output = nullptr;
    juce::AudioParameterFloat* m_restores = nullptr;
    juce::AudioParameterFloat* m_stateReads = nullptr;
    int m_restoreCount = 0;
    int m_stateReadCount = 0;
    juce::AudioBuffer<float> m_delay;
    int m_position = 0;
    int m_program = 0;
    std::array<float, 128> m_notes {};
    std::array<bool, 128> m_held {};
    bool m_sustain = false;
};
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new TestProcessor();
}
