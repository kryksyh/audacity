/*
* Audacity: A Digital Audio Editor
*/
#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <iterator>
#include <pthread.h>

#include <pluginterfaces/base/ibstream.h>
#include <pluginterfaces/vst/ivstevents.h>
#include <pluginterfaces/vst/ivstmessage.h>
#include <pluginterfaces/vst/ivstparameterchanges.h>
#include <pluginterfaces/vst/ivstprocesscontext.h>
#include <public.sdk/source/main/pluginfactory.h>
#include <public.sdk/source/vst/vstaudioeffect.h>
#include <public.sdk/source/vst/vsteditcontroller.h>

using namespace Steinberg;
using namespace Steinberg::Vst;

namespace {
std::atomic<uint64_t> latencyQueries { 0 };
std::atomic<uint64_t> offMainLatencyQueries { 0 };
std::atomic<uint64_t> processedFrames { 0 }, noteOns { 0 }, pressureEvents { 0 };
std::atomic<uint64_t> pressureValues { 0 };
std::atomic<bool> refuseNextRestartResume { false };
const FUID processorId(0xF5A81297, 0xA6724B21, 0x8E434FA1, 0xA4050C01);
const FUID controllerId(0xF5A81297, 0xA6724B21, 0x8E434FA1, 0xA4050C02);
const FUID audioEffectId(0xF5A81297, 0xA6724B21, 0x8E434FA1, 0xA4050C03);
const FUID restartInstrumentId(0xF5A81297, 0xA6724B21, 0x8E434FA1, 0xA4050C04);
const FUID programInstrumentId(0xF5A81297, 0xA6724B21, 0x8E434FA1, 0xA4050C05);
const FUID programControllerId(0xF5A81297, 0xA6724B21, 0x8E434FA1, 0xA4050C06);
const FUID rootProgramInstrumentId(0xF5A81297, 0xA6724B21, 0x8E434FA1, 0xA4050C07);
const FUID rootProgramControllerId(0xF5A81297, 0xA6724B21, 0x8E434FA1, 0xA4050C08);
std::atomic<uint64_t> offMainProgramQueries { 0 };
std::atomic<uint64_t> processorStateLoads { 0 };
std::atomic<uint64_t> midiControllerUpdates { 0 }, offMainMidiControllerUpdates { 0 };

class FixtureProcessor final : public AudioEffect
{
public:
    uint32 PLUGIN_API getLatencySamples() override
    {
        ++latencyQueries;
        if (!pthread_main_np()) {
            ++offMainLatencyQueries;
        }
        return m_latency.load();
    }

    tresult PLUGIN_API notify(IMessage* message) override
    {
        if (message && std::strcmp(message->getMessageID(), "latency") == 0) {
            int64 samples = 0;
            if (message->getAttributes()->getInt("samples", samples) == kResultOk) {
                m_latency = static_cast<uint32>(samples);
            }
            return kResultOk;
        }
        if (message && std::strcmp(message->getMessageID(), "arm-latency-on-resume") == 0) {
            int64 samples = 0;
            if (message->getAttributes()->getInt("samples", samples) == kResultOk) {
                m_latencyOnResume = samples;
            }
            return kResultOk;
        }
        if (message && std::strcmp(message->getMessageID(), "arm-latency-on-activate") == 0) {
            int64 samples = 0;
            if (message->getAttributes()->getInt("samples", samples) == kResultOk) {
                m_latencyOnActivate = samples;
            }
            return kResultOk;
        }
        return AudioEffect::notify(message);
    }

    tresult PLUGIN_API setActive(TBool active) override
    {
        const auto result = AudioEffect::setActive(active);
        if (result == kResultOk) {
            m_active = active;
        }
        if (!active) {
            m_delayed = {};
            m_delayHead = 0;
            m_down = false;
            m_velocity = 0;
            m_keys = {};
            m_voices = {};
            m_voiceSum = 0;
            if (m_polyphonic) {
                m_gain = 0.5;
                m_sustain = false;
            }
        }
        const auto next = active ? m_latencyOnActivate.exchange(-1) : -1;
        if (next >= 0) {
            notifyLatency(next);
        }
        return result;
    }

    tresult PLUGIN_API setProcessing(TBool processing) override
    {
        if (m_polyphonic && processing && refuseNextRestartResume.exchange(false)) {
            return kResultFalse;
        }
        const auto result = AudioEffect::setProcessing(processing);
        const auto next = processing ? m_latencyOnResume.exchange(-1) : -1;
        if (next >= 0) {
            notifyLatency(next);
        }
        return result;
    }

    tresult PLUGIN_API setupProcessing(ProcessSetup& setup) override
    {
        // Native hosts must deactivate a failed processor before configuring
        // it again; accepting this would conceal an incomplete host cleanup.
        if (m_polyphonic && m_active) {
            return kResultFalse;
        }
        return AudioEffect::setupProcessing(setup);
    }

    explicit FixtureProcessor(bool audioEffect = false, bool polyphonic = false)
        : m_audioEffect(audioEffect), m_polyphonic(polyphonic) { setControllerClass(controllerId); }
    static FUnknown* create(void*) { return static_cast<IAudioProcessor*>(new FixtureProcessor); }
    static FUnknown* createEffect(void*) { return static_cast<IAudioProcessor*>(new FixtureProcessor(true)); }
    static FUnknown* createRestartInstrument(void*) { return static_cast<IAudioProcessor*>(new FixtureProcessor(false, true)); }

    tresult PLUGIN_API initialize(FUnknown* context) override
    {
        const auto result = AudioEffect::initialize(context);
        if (result != kResultOk) {
            return result;
        }
        addAudioOutput(STR16("Output"), SpeakerArr::kStereo);
        if (m_audioEffect) {
            addAudioInput(STR16("Input"), SpeakerArr::kStereo);
        } else {
            addEventInput(STR16("MIDI"), 16);
        }
        return kResultOk;
    }

    tresult PLUGIN_API setState(IBStream* stream) override
    {
        ++processorStateLoads;
        int32 count = 0;
        return stream->read(&m_gain, sizeof(m_gain), &count);
    }

    tresult PLUGIN_API getState(IBStream* stream) override
    {
        int32 count = 0;
        return stream->write(&m_gain, sizeof(m_gain), &count);
    }

    tresult PLUGIN_API process(ProcessData& data) override
    {
        if (data.numSamples > 0) {
            processedFrames += static_cast<uint64_t>(data.numSamples);
        }
        const auto parametersAt = [&](int32 sample) {
            if (!data.inputParameterChanges) {
                return;
            }
            for (int32 parameter = 0; parameter < data.inputParameterChanges->getParameterCount(); ++parameter) {
                auto* queue = data.inputParameterChanges->getParameterData(parameter);
                for (int32 point = 0; point < queue->getPointCount(); ++point) {
                    int32 offset = 0;
                    ParamValue value = 0;
                    queue->getPoint(point, offset, value);
                    if (offset != sample) {
                        continue;
                    }
                    if (queue->getParameterId() == 0 || queue->getParameterId() == 3) {
                        m_gain = value;
                    } else if (queue->getParameterId() == 1) {
                        m_sustain = value >= 0.5;
                        if (!m_sustain && !m_down) {
                            m_velocity = 0;
                        }
                        if (!m_sustain && m_polyphonic) {
                            for (size_t channel = 0; channel < 16; ++channel) {
                                for (size_t note = 0; note < 128; ++note) {
                                    if (!m_keys[channel][note]) {
                                        setVoice(channel, note, 0);
                                    }
                                }
                            }
                        }
                    } else if (queue->getParameterId() == 2) {
                        const auto delay = static_cast<uint32>(std::clamp(value, 0.0, 1.0) * 31.0 + 0.5);
                        if (delay != m_latency.load()) {
                            notifyLatency(delay);
                        }
                    }
                }
            }
        };
        if (!data.numSamples) {
            if (data.numInputs != getBusCount(kAudio, kInput) || data.numOutputs != getBusCount(kAudio, kOutput)
                || (data.numOutputs && !data.outputs)) {
                return kResultFalse;
            }
            parametersAt(0);
            return kResultOk;
        }
        if (data.numOutputs == 0) {
            return kResultOk;
        }
        if (m_audioEffect) {
            for (int32 sample = 0; sample < data.numSamples; ++sample) {
                parametersAt(sample);
                const auto delay = std::min<uint32>(m_latency.load(), 31);
                for (int32 channel = 0; channel < data.outputs[0].numChannels; ++channel) {
                    auto& history = m_delayed[std::min<int32>(channel, 1)];
                    history[m_delayHead] = data.numInputs && channel < data.inputs[0].numChannels
                                           ? static_cast<float>(data.inputs[0].channelBuffers32[channel][sample] * m_gain) : 0.0f;
                    data.outputs[0].channelBuffers32[channel][sample] = history[(m_delayHead + history.size() - delay) % history.size()];
                }
                m_delayHead = (m_delayHead + 1) % m_delayed[0].size();
            }
            data.outputs[0].silenceFlags = 0;
            return kResultOk;
        }
        bool reportContext = !data.inputEvents;
        bool reportClock = false;
        if (data.inputEvents) {
            for (int32 i = 0; i < data.inputEvents->getEventCount(); ++i) {
                Event event {};
                data.inputEvents->getEvent(i, event);
                // An otherwise unused test note exposes the instrument's
                // transport packet through the same SDK process boundary.
                if (event.type == Event::kNoteOnEvent && event.noteOn.pitch == 127) {
                    reportContext = true;
                }
                if (event.type == Event::kNoteOnEvent && event.noteOn.pitch == 126) {
                    reportClock = true;
                }
            }
        }
        if (reportClock && data.processContext) {
            const auto& context = *data.processContext;
            const double values[] { static_cast<double>(context.projectTimeSamples), static_cast<double>(context.continousTimeSamples),
                                    (context.state & ProcessContext::kContTimeValid) ? 1.0 : 0.0,
                                    (context.state & ProcessContext::kPlaying) ? 1.0 : 0.0 };
            for (int32 channel = 0; channel < data.outputs[0].numChannels; ++channel) {
                for (int32 frame = 0; frame < data.numSamples; ++frame) {
                    data.outputs[0].channelBuffers32[channel][frame] = values[frame % 4];
                }
            }
            return kResultOk;
        }
        if (reportContext && data.processContext) {
            // Original effect instances use this fixture without a MIDI list.
            // Return the host packet in audio so tests cross the SDK boundary.
            const auto& context = *data.processContext;
            const double values[] { context.tempo, static_cast<double>(context.timeSigNumerator),
                                    static_cast<double>(context.timeSigDenominator), static_cast<double>(context.projectTimeSamples),
                                    context.projectTimeMusic, context.barPositionMusic, context.sampleRate,
                                    (context.state & ProcessContext::kPlaying) ? 1.0 : 0.0,
                                    (context.state & ProcessContext::kCycleActive) ? 1.0 : 0.0,
                                    context.cycleStartMusic, context.cycleEndMusic,
                                    (context.state & ProcessContext::kCycleValid) ? 1.0 : 0.0 };
            for (int32 channel = 0; channel < data.outputs[0].numChannels; ++channel) {
                for (int32 sample = 0; sample < data.numSamples; ++sample) {
                    data.outputs[0].channelBuffers32[channel][sample] = values[sample % std::size(values)];
                }
            }
            return kResultOk;
        }
        if (!getEventInput(0)->isActive()) {
            return kResultOk;
        }
        for (int32 sample = 0; sample < data.numSamples; ++sample) {
            parametersAt(sample);
            if (data.inputEvents) {
                for (int32 index = 0; index < data.inputEvents->getEventCount(); ++index) {
                    Event event {};
                    data.inputEvents->getEvent(index, event);
                    if (event.sampleOffset != sample) {
                        continue;
                    }
                    if (event.type == Event::kNoteOnEvent) {
                        ++noteOns;
                        if (m_polyphonic) {
                            m_keys[event.noteOn.channel][event.noteOn.pitch] = true;
                            setVoice(event.noteOn.channel, event.noteOn.pitch, event.noteOn.velocity);
                        } else {
                            m_down = true;
                            m_velocity = event.noteOn.velocity;
                        }
                    } else if (event.type == Event::kNoteOffEvent) {
                        if (m_polyphonic) {
                            m_keys[event.noteOff.channel][event.noteOff.pitch] = false;
                            if (!m_sustain) {
                                setVoice(event.noteOff.channel, event.noteOff.pitch, 0);
                            }
                        } else {
                            m_down = false;
                            if (!m_sustain) {
                                m_velocity = 0;
                            }
                        }
                    } else if (event.type == Event::kPolyPressureEvent) {
                        ++pressureEvents;
                        pressureValues += static_cast<uint64_t>(event.polyPressure.pressure * 127.0f + 0.5f);
                    }
                }
            }
            for (int32 channel = 0; channel < data.outputs[0].numChannels; ++channel) {
                auto& history = m_delayed[std::min<int32>(channel, 1)];
                history[m_delayHead] = static_cast<float>(m_gain * (m_polyphonic ? m_voiceSum : m_velocity));
                const auto delay = std::min<uint32>(m_latency.load(), 31);
                data.outputs[0].channelBuffers32[channel][sample] = history[(m_delayHead + history.size() - delay) % history.size()];
            }
            m_delayHead = (m_delayHead + 1) % m_delayed[0].size();
        }
        data.outputs[0].silenceFlags = 0;
        return kResultOk;
    }

private:
    void setVoice(size_t channel, size_t note, float value)
    {
        m_voiceSum += value - m_voices[channel][note];
        m_voices[channel][note] = value;
    }

    void notifyLatency(int64 next)
    {
        m_latency = static_cast<uint32>(next);
        auto message = owned(allocateMessage());
        if (message) {
            message->setMessageID("latency-notification");
            message->getAttributes()->setInt("samples", next);
            sendMessage(message);
        }
    }

    std::atomic<uint32> m_latency { 0 };
    std::atomic<int64> m_latencyOnResume { -1 };
    std::atomic<int64> m_latencyOnActivate { -1 };
    bool m_audioEffect = false;
    bool m_polyphonic = false;
    bool m_active = false;
    std::array<std::array<bool, 128>, 16> m_keys {};
    std::array<std::array<float, 128>, 16> m_voices {};
    float m_voiceSum = 0;
    std::array<std::array<float, 32>, 2> m_delayed {};
    size_t m_delayHead = 0;
    double m_gain = 0.5;
    float m_velocity = 0;
    bool m_down = false;
    bool m_sustain = false;
};

class FixtureController final : public EditController, public IMidiMapping
{
public:
    static FUnknown* create(void*) { return static_cast<IEditController*>(new FixtureController); }
    tresult PLUGIN_API notify(IMessage* message) override
    {
        if (message && (std::strcmp(message->getMessageID(), "arm-latency-on-resume") == 0
                        || std::strcmp(message->getMessageID(), "arm-latency-on-activate") == 0)) {
            sendMessage(message);
            return kResultOk;
        }
        if (message && std::strcmp(message->getMessageID(), "latency-notification") == 0) {
            int64 samples = 0;
            if (message->getAttributes()->getInt("samples", samples) == kResultOk) {
                setParamNormalized(2, samples / 31.0);
            }
            return kResultOk;
        }
        return EditController::notify(message);
    }

    tresult PLUGIN_API initialize(FUnknown* context) override
    {
        const auto result = EditController::initialize(context);
        if (result == kResultOk) {
            parameters.addParameter(STR16("Gain"), nullptr, 0, 0.5, ParameterInfo::kCanAutomate, 0);
            parameters.addParameter(STR16("Sustain"), nullptr, 1, 0.0, ParameterInfo::kCanAutomate, 1);
            parameters.addParameter(STR16("Lookahead"), nullptr, 31, 0.0, ParameterInfo::kCanAutomate, 2);
            parameters.addParameter(STR16("MIDI CC 0|41"), nullptr, 0, 0.0, ParameterInfo::kNoFlags, 3);
            parameters.addParameter(STR16("Output meter"), nullptr, 0, 0.25, ParameterInfo::kIsReadOnly, 4);
        }
        return result;
    }

    tresult PLUGIN_API setParamNormalized(ParamID id, ParamValue value) override
    {
        const auto previous = getParamNormalized(id);
        const auto result = EditController::setParamNormalized(id, value);
        // Like native instruments with hidden MIDI bridge parameters, update
        // the visible knob only when the host also notifies the controller.
        if (id == 3) {
            ++midiControllerUpdates;
            if (!pthread_main_np()) {
                ++offMainMidiControllerUpdates;
            }
            EditController::setParamNormalized(0, value);
        }
        if (id == 2 && value != previous) {
            auto message = owned(allocateMessage());
            if (message) {
                message->setMessageID("latency");
                message->getAttributes()->setInt("samples", static_cast<int64>(value * 31.0 + 0.5));
                sendMessage(message);
            }
            if (componentHandler) {
                componentHandler->restartComponent(kLatencyChanged);
            }
        }
        return result;
    }

    tresult PLUGIN_API setComponentState(IBStream* stream) override
    {
        double gain = 0.5;
        int32 count = 0;
        const auto result = stream->read(&gain, sizeof(gain), &count);
        if (result == kResultOk) {
            setParamNormalized(0, gain);
        }
        return result;
    }

    tresult PLUGIN_API getMidiControllerAssignment(int32 bus, int16, CtrlNumber controller, ParamID& parameter) override
    {
        if (bus == 0 && controller == 64) {
            parameter = 1;
            return kResultTrue;
        }
        if (bus == 0 && controller == 11) {
            parameter = 0; // An ordinary saved/automatable knob, unlike virtual CC 41.
            return kResultTrue;
        }
        if (bus == 0 && controller == 41) {
            parameter = 3;
            return kResultTrue;
        }
        parameter = 0; // Failed queries do not promise a meaningful out value.
        return kResultFalse;
    }

    tresult PLUGIN_API queryInterface(const TUID iid, void** object) override
    {
        if (FUnknownPrivate::iidEqual(iid, IMidiMapping::iid)) {
            *object = static_cast<IMidiMapping*>(this);
            addRef();
            return kResultOk;
        }
        return EditController::queryInterface(iid, object);
    }

    uint32 PLUGIN_API addRef() override { return EditController::addRef(); }
    uint32 PLUGIN_API release() override { return EditController::release(); }
};

// Separate classes keep program-list coverage independent of the original
// gain/latency fixtures and their parameter and factory-preset contracts.
class ProgramProcessor final : public AudioEffect
{
public:
    explicit ProgramProcessor(bool rootOnly = false)
        : values{rootOnly ? 0.0 : 0.5, 0.0, 1.0 / 3}
    { setControllerClass(rootOnly ? rootProgramControllerId : programControllerId); }
    static FUnknown* create(void*) { return static_cast<IAudioProcessor*>(new ProgramProcessor); }
    static FUnknown* createRootOnly(void*) { return static_cast<IAudioProcessor*>(new ProgramProcessor(true)); }
    tresult PLUGIN_API initialize(FUnknown* context) override
    {
        const auto result = AudioEffect::initialize(context);
        if (result == kResultOk) {
            addAudioOutput(STR16("Output"), SpeakerArr::kStereo);
            addEventInput(STR16("MIDI"), 16);
        }
        return result;
    }

    tresult PLUGIN_API setState(IBStream* stream) override { int32 count = 0; return stream->read(values.data(), sizeof(values), &count); }
    tresult PLUGIN_API getState(IBStream* stream) override { int32 count = 0; return stream->write(values.data(), sizeof(values), &count); }
    tresult PLUGIN_API setActive(TBool active) override
    {
        if (!active) {
            values = {};
            sounding = false;
        }
        return AudioEffect::setActive(active);
    }

    tresult PLUGIN_API process(ProcessData& data) override
    {
        const auto apply = [&](int32 sample) {
            if (!data.inputParameterChanges) {
                return;
            }
            for (int32 parameter = 0; parameter < data.inputParameterChanges->getParameterCount(); ++parameter) {
                auto* queue = data.inputParameterChanges->getParameterData(parameter);
                const auto id = queue->getParameterId();
                if (id < 10 || id > 12) {
                    continue;
                }
                for (int32 point = 0; point < queue->getPointCount(); ++point) {
                    int32 offset = 0;
                    ParamValue value = 0;
                    queue->getPoint(point, offset, value);
                    if (offset == sample) {
                        values[id - 10] = value;
                    }
                }
            }
        };
        if (!data.numSamples) {
            apply(0);
            return kResultOk;
        }
        for (int32 sample = 0; sample < data.numSamples; ++sample) {
            apply(sample);
            if (data.inputEvents) {
                for (int32 index = 0; index < data.inputEvents->getEventCount(); ++index) {
                    Event event {};
                    data.inputEvents->getEvent(index, event);
                    if (event.sampleOffset != sample) {
                        continue;
                    }
                    if (event.type == Event::kNoteOnEvent) {
                        sounding = true;
                    }
                    if (event.type == Event::kNoteOffEvent) {
                        sounding = false;
                    }
                }
            }
            if (data.numOutputs) {
                for (int32 channel = 0; channel < data.outputs[0].numChannels; ++channel) {
                    data.outputs[0].channelBuffers32[channel][sample] = sounding ? 1.0
                                                                        + values[std::min<int32>(channel, 1)] + 10.0 * values[2] : 0.0;
                }
            }
        }
        return kResultOk;
    }

private:
    std::array<double, 3> values {};
    bool sounding = false;
};

class ProgramController final : public EditControllerEx1
{
public:
    explicit ProgramController(bool rootOnly = false)
        : rootOnly(rootOnly) {}
    static FUnknown* create(void*) { return static_cast<IEditController*>(new ProgramController); }
    static FUnknown* createRootOnly(void*) { return static_cast<IEditController*>(new ProgramController(true)); }
    tresult PLUGIN_API initialize(FUnknown* context) override
    {
        const auto result = EditControllerEx1::initialize(context);
        if (result != kResultOk) {
            return result;
        }
        addUnit(new Unit(STR16("Root"), kRootUnitId, kNoParentUnitId, 102));
        addUnit(new Unit(STR16("Part A"), 10, kRootUnitId, 100));
        addUnit(new Unit(STR16("Part B"), 11, kRootUnitId, 101));
        addUnit(new Unit(STR16("No programs"), 13, kRootUnitId));
        for (int i = 0; i < 3; ++i) {
            auto* list = new ProgramList(STR16("Programs"), 100 + i, i == 2 ? kRootUnitId : 10 + i);
            const auto count = i == 0 ? 3 : i == 1 ? 1 : 4;
            for (int program = 0; program < count; ++program) {
                list->addProgram(STR16("Preset"));
            }
            addProgramList(list);
        }
        if (!rootOnly) {
            parameters.addParameter(STR16(
                                        "Part A preset"), nullptr, 2, 0.5, ParameterInfo::kIsProgramChange | ParameterInfo::kIsHidden, 10,
                                    10);
            parameters.addParameter(STR16("Part B preset"), nullptr, 0, 0.0, ParameterInfo::kIsProgramChange, 11, 11);
        }
        parameters.addParameter(STR16("Root preset"), nullptr, 3, 1.0 / 3, ParameterInfo::kIsProgramChange, 12, kRootUnitId);
        return result;
    }

    tresult PLUGIN_API setComponentState(IBStream* stream) override
    {
        std::array<double, 3> values {};
        int32 count = 0;
        const auto result = stream->read(values.data(), sizeof(values), &count);
        if (result == kResultOk) {
            for (int i = 0; i < 3; ++i) {
                if (parameters.getParameter(10 + i)) {
                    setParamNormalized(10 + i, values[i]);
                }
            }
        }
        return result;
    }

    tresult PLUGIN_API getUnitByBus(MediaType type, BusDirection direction, int32 bus, int32 channel, UnitID& id) override
    {
        if (!pthread_main_np()) {
            ++offMainProgramQueries;
        }
        if (type == kEvent && direction == kInput && bus == 0) {
            if (channel == 0 || channel == 1) {
                id = 10 + channel;
                return kResultTrue;
            }
            if (channel == 3) {
                id = 999;
                return kResultTrue;
            }
            if (channel == 4) {
                id = 13;
                return kResultTrue;
            }
        }
        id = 10; // Failure must not accidentally select this part.
        return kResultFalse;
    }

    tresult PLUGIN_API queryInterface(const TUID iid, void** object) override
    {
        if (rootOnly && FUnknownPrivate::iidEqual(iid, IUnitInfo::iid)) {
            *object = nullptr;
            return kNoInterface;
        }
        return EditControllerEx1::queryInterface(iid, object);
    }

private:
    bool rootOnly = false;
};
}

BEGIN_FACTORY_DEF("Audacity", "https://www.audacityteam.org", "")
DEF_CLASS2(INLINE_UID_FROM_FUID(processorId), PClassInfo::kManyInstances, kVstAudioEffectClass,
           "Native Instrument Fixture", Vst::kDistributable, "Instrument|Synth", "1.0", kVstVersionString, FixtureProcessor::create)
DEF_CLASS2(INLINE_UID_FROM_FUID(audioEffectId), PClassInfo::kManyInstances, kVstAudioEffectClass,
           "Native Variable Delay Fixture", Vst::kDistributable, "Fx|Delay", "1.0", kVstVersionString, FixtureProcessor::createEffect)
DEF_CLASS2(INLINE_UID_FROM_FUID(
               restartInstrumentId), PClassInfo::kManyInstances, kVstAudioEffectClass,
           "Native Restart Instrument Fixture", Vst::kDistributable, "Instrument|Synth", "1.0", kVstVersionString,
           FixtureProcessor::createRestartInstrument)
DEF_CLASS2(INLINE_UID_FROM_FUID(controllerId), PClassInfo::kManyInstances, kVstComponentControllerClass,
           "Native Instrument Fixture Controller", 0, "", "1.0", kVstVersionString, FixtureController::create)
DEF_CLASS2(INLINE_UID_FROM_FUID(programInstrumentId), PClassInfo::kManyInstances, kVstAudioEffectClass,
           "Native Program Instrument Fixture", Vst::kDistributable, "Instrument|Synth", "1.0", kVstVersionString, ProgramProcessor::create)
DEF_CLASS2(INLINE_UID_FROM_FUID(programControllerId), PClassInfo::kManyInstances, kVstComponentControllerClass,
           "Native Program Fixture Controller", 0, "", "1.0", kVstVersionString, ProgramController::create)
DEF_CLASS2(INLINE_UID_FROM_FUID(
               rootProgramInstrumentId), PClassInfo::kManyInstances, kVstAudioEffectClass,
           "Native Root Program Fixture", Vst::kDistributable, "Instrument|Synth", "1.0", kVstVersionString,
           ProgramProcessor::createRootOnly)
DEF_CLASS2(INLINE_UID_FROM_FUID(rootProgramControllerId), PClassInfo::kManyInstances, kVstComponentControllerClass,
           "Native Root Program Controller", 0, "", "1.0", kVstVersionString, ProgramController::createRootOnly)
END_FACTORY

extern "C" __attribute__((visibility("default"))) bool bundleEntry(void*) { return true; }
extern "C" __attribute__((visibility("default"))) bool bundleExit() { return true; }
extern "C" __attribute__((visibility("default"))) uint64_t nativeFixtureOffMainProgramQueries() { return offMainProgramQueries.load(); }
extern "C" __attribute__((visibility("default"))) uint64_t nativeFixtureProcessorStateLoads() { return processorStateLoads.load(); }
extern "C" __attribute__((visibility("default"))) uint64_t nativeFixtureMidiControllerUpdates() { return midiControllerUpdates.load(); }
extern "C" __attribute__((visibility("default"))) uint64_t nativeFixtureOffMainMidiControllerUpdates()
{
    return offMainMidiControllerUpdates.load();
}

extern "C" __attribute__((visibility("default"))) uint64_t nativeFixtureLatencyQueries() { return latencyQueries.load(); }
extern "C" __attribute__((visibility("default"))) uint64_t nativeFixtureOffMainLatencyQueries() { return offMainLatencyQueries.load(); }
extern "C" __attribute__((visibility("default"))) uint64_t nativeFixtureProcessedFrames() { return processedFrames.load(); }
extern "C" __attribute__((visibility("default"))) uint64_t nativeFixtureNoteOns() { return noteOns.load(); }
extern "C" __attribute__((visibility("default"))) uint64_t nativeFixturePressureEvents() { return pressureEvents.load(); }
extern "C" __attribute__((visibility("default"))) uint64_t nativeFixturePressureValues() { return pressureValues.load(); }
extern "C" __attribute__((visibility("default"))) void nativeFixtureRefuseNextResume(bool value) { refuseNextRestartResume = value; }
