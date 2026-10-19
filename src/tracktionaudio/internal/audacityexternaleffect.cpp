/*
* Audacity: A Digital Audio Editor
*/
#include <tracktion_engine/tracktion_engine.h>
#include <tracktion_graph/tracktion_graph.h>

#include "audacityexternaleffect.h"
#include "tracktionautomation.h"
#include "tracktionplugindescriptions.h"
#include "tracktioneffectinstance.h"
#include "tracktionpluginloader.h"
#include "tracktionlv2settings.h"

#include <atomic>
#include <stdexcept>
#include <set>
#include <wx/base64.h>
#include <wx/tokenzr.h>
#include <pluginterfaces/vst/ivsteditcontroller.h>

#include "audacityeffectregistry.h"
#include "global/defer.h"
#include "au3-realtime-effects/RealtimeEffectList.h"
#include "au3-realtime-effects/RealtimeEffectState.h"
#include "au3-module-manager/ConfigInterface.h"

#if AU_TRACKTION_AUDIO_UNIT
#include "au3-audio-unit/AudioUnitWrapper.h"
#endif

#if JUCE_PLUGINHOST_AU
#include <AudioToolbox/AudioToolbox.h>
#endif

using namespace au::tracktionaudio;

namespace {
juce::ValueTree externalState(tracktion::Edit& edit, const EffectInstanceFactory& effect)
{
    const auto family = effect.GetFamily().Internal();
    juce::String identifier;
    juce::String vst3ID;
    if (family == wxT("VST3")) {
        const auto path = effect.GetPath();
        const auto classID = path.AfterLast(';');
        if (classID.size() != 32 || !path.Contains(';')) {
            throw std::runtime_error("The VST3 identifier is not valid.");
        }
        for (size_t word = 0; word < 4; ++word) {
            unsigned long value = 0;
            const bool valid = classID.Mid(word * 8, 8).ToULong(&value, 16);
            if (!valid) {
                throw std::runtime_error("The VST3 class identifier is not valid.");
            }
        }
        vst3ID = classID.ToStdString();
        identifier = path.BeforeLast(';').ToStdString();
    } else if (family == wxT("LV2")) {
        identifier = effect.GetPath().ToStdString();
        if (!Lv2PluginIdentifier::parse(identifier.toStdString())) {
            throw std::runtime_error("The LV2 identifier is not valid.");
        }
    } else {
        const auto fields = wxSplit(effect.GetPath(), '/');
        if (fields.size() < 4 || fields[0].size() != 4 || fields[1].size() != 4 || fields[2].size() != 4) {
            throw std::runtime_error("The Audio Unit identifier is not valid.");
        }
        identifier = juce::String("AudioUnit:") + fields[1].ToStdString() + "," + fields[2].ToStdString()
                     + "," + fields[0].ToStdString();
    }
    auto& manager = edit.engine.getPluginManager();
    for (auto* format : manager.pluginFormatManager.getFormats()) {
        if (format->getName() != juce::String(family.ToStdString())) {
            continue;
        }
        const auto description = findTracktionPluginDescription(manager, *format, identifier, vst3ID);
        if (description) {
            return tracktion::ExternalPlugin::create(edit.engine, *description);
        }
    }
    throw std::runtime_error("Tracktion cannot find the external effect.");
}

#if JUCE_PLUGINHOST_AU
std::optional<AudioUnitParameterID> audioUnitParameterID(const wxString& key)
{
    const auto end = key.rfind('>');
    const auto separator = key.find_last_of("<,", end);
    unsigned long id = 0;
    const bool valid = end != wxString::npos && separator != wxString::npos && separator + 1 < end
                       && key.substr(separator + 1, end - separator - 1).ToULong(&id, 16);
    return valid ? std::make_optional(static_cast<AudioUnitParameterID>(id)) : std::nullopt;
}

std::optional<int> audioUnitPresetNumber(const CommandParameters& parameters)
{
    wxString key, presetKey;
    long index = 0;
    bool entry = parameters.GetFirstEntry(key, index);
    while (entry) {
        if (key.StartsWith("_PRESET") && key.Mid(7).find_first_not_of('_') == wxString::npos && key.size() > presetKey.size()) {
            presetKey = key;
        }
        entry = parameters.GetNextEntry(key, index);
    }
    long number = 0;
    return !presetKey.empty() && parameters.Read(presetKey, &number) ? std::make_optional(static_cast<int>(number)) : std::nullopt;
}

std::set<wxString> audioUnitParameterIds(const CommandParameters& parameters)
{
    std::set<wxString> ids;
    wxString key;
    long index = 0;
    bool entry = parameters.GetFirstEntry(key, index);
    while (entry) {
        const auto id = audioUnitParameterID(key);
        if (id) {
            ids.insert(wxString::Format("%u", static_cast<unsigned>(*id)));
        }
        entry = parameters.GetNextEntry(key, index);
    }
    return ids;
}

std::optional<wxString> audioUnitState(const CommandParameters& parameters)
{
    return parameters.HasEntry("AudioUnitState") ? std::make_optional(parameters.Read("AudioUnitState")) : std::nullopt;
}

bool applyAudioUnitState(juce::AudioPluginInstance& processor, const wxString& state)
{
    const auto decoded = wxBase64Decode(state);
    if (decoded.GetDataLen() == 0) {
        return false;
    }
    const juce::MemoryBlock bytes(decoded.GetData(), decoded.GetDataLen());
    return processor.getAudioUnitClient()->restoreState(bytes);
}

void applyAudioUnitPreset(AudioUnit unit, const CommandParameters& parameters)
{
    const auto number = audioUnitPresetNumber(parameters);
    if (!number) {
        return;
    }
    CFArrayRef presets = nullptr;
    UInt32 size = sizeof(presets);
    const auto error = AudioUnitGetProperty(unit, kAudioUnitProperty_FactoryPresets, kAudioUnitScope_Global, 0, &presets, &size);
    if (error != noErr || !presets) {
        return;
    }
    for (CFIndex preset = 0; preset < CFArrayGetCount(presets); ++preset) {
        const auto* value = static_cast<const AUPreset*>(CFArrayGetValueAtIndex(presets, preset));
        if (value->presetNumber == *number) {
            AudioUnitSetProperty(unit, kAudioUnitProperty_PresentPreset, kAudioUnitScope_Global, 0, value, sizeof(*value));
            break;
        }
    }
    CFRelease(presets);
}

#endif

using Vst3State = std::pair<std::optional<wxString>, std::optional<wxString> >;

Vst3State vst3State(const CommandParameters& parameters)
{
    const auto readState = [&](const char* key) -> std::optional<wxString> {
        return parameters.HasEntry(key) ? std::make_optional(parameters.Read(key)) : std::nullopt;
    };
    return { readState("ProcessorState"), readState("ControllerState") };
}

bool applyVst3State(juce::AudioPluginInstance& processor, const Vst3State& states)
{
    juce::XmlElement xml("VST3PluginState");
    const auto addState = [&](const char* tag, const std::optional<wxString>& encoded) {
        if (encoded) {
            const auto decoded = wxBase64Decode(*encoded);
            if (!encoded->empty() && decoded.GetDataLen() == 0) {
                return false;
            }
            const juce::MemoryBlock data(decoded.GetData(), decoded.GetDataLen());
            xml.createNewChildElement(tag)->addTextElement(data.toBase64Encoding());
        }
        return true;
    };
    if (!addState("IComponent", states.first) || !addState("IEditController", states.second)) {
        return false;
    }
    juce::MemoryBlock state;
    juce::AudioProcessor::copyXmlToBinary(xml, state);
    return processor.getVST3Client()->restoreState(state);
}

std::set<wxString> vst3ParameterIds(const CommandParameters& parameters)
{
    std::set<wxString> ids;
    wxStringTokenizer changes(parameters.Read("Parameters"), ";");
    while (changes.HasMoreTokens()) {
        const auto change = changes.GetNextToken();
        double value = 0.0;
        const bool valid = change.Contains('=') && change.AfterFirst('=').ToDouble(&value);
        if (valid) {
            ids.insert(change.BeforeFirst('='));
        }
    }
    return ids;
}

void applyVst3Parameters(juce::AudioPluginInstance& processor, const CommandParameters& parameters)
{
    wxStringTokenizer changes(parameters.Read("Parameters"), ";");
    while (changes.HasMoreTokens()) {
        const auto change = changes.GetNextToken();
        const auto id = change.BeforeFirst('=');
        double value = 0.0;
        const bool valid = change.Contains('=') && change.AfterFirst('=').ToDouble(&value);
        if (!valid) {
            continue;
        }
        for (auto* parameter : processor.getParameters()) {
            const auto* hosted = dynamic_cast<const juce::HostedAudioProcessorParameter*>(parameter);
            if (hosted && hosted->getParameterID() == juce::String(id.ToStdString())) {
                parameter->setValue(static_cast<float>(value));
                break;
            }
        }
    }
}

#if JUCE_PLUGINHOST_AU
void applyAudioUnitParameters(juce::AudioPluginInstance& processor, const CommandParameters& parameters)
{
    const auto unit = processor.getAudioUnitClient()->getAudioUnitHandle();
    // A parameter can enable other parameters later in the list.
    for (int pass = 0; pass < 2; ++pass) {
        wxString key;
        long index = 0;
        bool entry = parameters.GetFirstEntry(key, index);
        while (entry) {
            const auto id = audioUnitParameterID(key);
            double value = 0.0;
            if (id && parameters.Read(key, &value)) {
                AudioUnitParameterInfo info {};
                UInt32 size = sizeof(info);
                const auto error = AudioUnitGetProperty(unit, kAudioUnitProperty_ParameterInfo, kAudioUnitScope_Global, *id, &info, &size);
                DEFER {
                    if (info.flags & kAudioUnitParameterFlag_CFNameRelease) {
                        if (info.cfNameString) {
                            CFRelease(info.cfNameString);
                        }
                        if (info.unit == kAudioUnitParameterUnit_CustomUnit && info.unitName) {
                            CFRelease(info.unitName);
                        }
                    }
                };
                juce::AudioProcessorParameter* target = nullptr;
                for (auto* parameter : processor.getParameters()) {
                    const auto* hosted = dynamic_cast<const juce::HostedAudioProcessorParameter*>(parameter);
                    if (hosted && hosted->getParameterID() == juce::String(static_cast<juce::int64>(*id))) {
                        target = parameter;
                        break;
                    }
                }
                if (target && error == noErr && info.maxValue > info.minValue) {
                    target->setValue(static_cast<float>((value - info.minValue) / (info.maxValue - info.minValue)));
                } else {
                    AudioUnitSetParameter(unit, *id, kAudioUnitScope_Global, 0, static_cast<AudioUnitParameterValue>(value), 0);
                }
            }
            entry = parameters.GetNextEntry(key, index);
        }
    }
}

bool applyAudioUnitSettings(juce::AudioPluginInstance& processor, const CommandParameters& parameters)
{
    const auto state = audioUnitState(parameters);
    if (state) {
        if (!applyAudioUnitState(processor, *state)) {
            return false;
        }
    } else {
        applyAudioUnitPreset(processor.getAudioUnitClient()->getAudioUnitHandle(), parameters);
    }
    applyAudioUnitParameters(processor, parameters);
    return true;
}

#endif

class AudacityExternalEffectPlugin final : public tracktion::ExternalPlugin, private juce::Timer
{
    struct Processor {
        tracktion::ExternalPlugin::Ptr instance;
        std::optional<Vst3State> lastVstState;
        std::optional<std::optional<int> > lastAudioUnitPreset;
        std::optional<std::optional<wxString> > lastAudioUnitState;
        std::optional<EffectSettings> lastLv2Settings;
        std::set<wxString> parameterIds;
        std::optional<juce::MemoryBlock> defaultState;
        juce::AudioBuffer<float> audio;
    };

public:
    AudacityExternalEffectPlugin(tracktion::PluginCreationInfo info, std::shared_ptr<RealtimeEffectState> state,
                                 const RealtimeEffectList& list, int channels, bool live,
                                 std::shared_ptr<TracktionHostedEffectInstance> editor)
        : ExternalPlugin(info, editor ? &editor->tracktionPlugin() : nullptr, editor), m_editor(std::move(editor)),
        m_effect(std::move(state)), m_list(list), m_channels(channels),
        m_live(live), m_listActive(list.IsActive())
    {
        GetConfig(*m_effect->GetEffect(), PluginSettings::Shared, wxT("Options"), wxT("UseLatency"), m_useLatency, true);
        if (live || isInitialisingAsync()) {
            startTimer(20);
        }
    }

    ~AudacityExternalEffectPlugin() override
    {
        stopTimer();
        if (m_editorPlaybackRegistered) {
            m_editor->endPlayback();
        }
    }

    bool canBeDisabled() override { return false; }
    double getLatencySeconds() override { return m_useLatency ? ExternalPlugin::getLatencySeconds() : 0.0; }
    int getNumOutputChannelsGivenInputs(int) override { return m_channels; }

    BusLayout getBusses() const override
    {
        auto layout = ExternalPlugin::getBusses();
        const auto channels = tracktion::ChannelConfiguration::canonical(m_channels);
        if (!layout.inputs.empty()) {
            layout.inputs.front() = channels;
        }
        if (!layout.outputs.empty()) {
            layout.outputs.front() = channels;
        }
        return layout;
    }

    void getChannelNames(juce::StringArray* inputs, juce::StringArray* outputs) override
    {
        ExternalPlugin::getChannelNames(inputs, outputs);
        const auto* processor = getAudioPluginInstance();
        const auto channels = juce::AudioChannelSet::canonicalChannelSet(m_channels);
        const auto replaceMain = [&](juce::StringArray* names, int count) {
            if (names) {
                names->removeRange(0, count);
                for (int channel = 0; channel < channels.size(); ++channel) {
                    names->insert(channel, juce::AudioChannelSet::getChannelTypeName(channels.getTypeOfChannel(channel)));
                }
            }
        };
        replaceMain(inputs, processor ? processor->getMainBusNumInputChannels() : 0);
        replaceMain(outputs, processor ? processor->getMainBusNumOutputChannels() : 0);
    }

    bool canSidechain() override
    {
        const auto* processor = getAudioPluginInstance();
        return !isInRack() && processor && processor->getTotalNumInputChannels() > processor->getMainBusNumInputChannels();
    }

    void initialiseFully() override
    {
        ExternalPlugin::initialiseFully();
        auto* processor = getAudioPluginInstance();
        if (processor && m_processors.empty()) {
            auto layout = processor->getBusesLayout();
            const auto channels = juce::AudioChannelSet::canonicalChannelSet(m_channels);
            if (!layout.inputBuses.isEmpty()) {
                layout.inputBuses.getReference(0) = channels;
            }
            if (!layout.outputBuses.isEmpty()) {
                layout.outputBuses.getReference(0) = channels;
            }
            if (processor->checkBusesLayoutSupported(layout)) {
                setBusesLayout(layout);
            }
            m_inputs = processor->getMainBusNumInputChannels();
            m_outputs = processor->getMainBusNumOutputChannels();
            if (m_outputs <= 0) {
                throw std::runtime_error("The effect has no main audio output.");
            }
            m_routing = m_inputs != m_channels || m_outputs != m_channels;
            const int count = (m_channels + m_outputs - 1) / m_outputs;
            m_processors.clear();
            m_processors.resize(count);
            for (int index = 0; index < count; ++index) {
                auto& entry = m_processors[index];
                if (index > 0) {
                    const auto plugin = edit.getPluginCache().createNewPlugin(tracktion::ExternalPlugin::xmlTypeName, desc);
                    entry.instance = dynamic_cast<tracktion::ExternalPlugin*>(plugin.get());
                    if (!entry.instance) {
                        throw std::runtime_error("Cannot create a Tracktion effect processor.");
                    }
                    waitForTracktionPluginLoad(*entry.instance);
                    auto* instance = entry.instance ? entry.instance->getAudioPluginInstance() : nullptr;
                    if (instance) {
                        entry.instance->setBusesLayout(processor->getBusesLayout());
                    }
                }
                auto* instance = instanceFor(entry).getAudioPluginInstance();
                if (instance) {
                    if (index == 0 && m_editor) {
                        entry.defaultState = m_editor->defaultState();
                    } else {
                        juce::MemoryBlock state;
                        instance->getStateInformation(state);
                        entry.defaultState = std::move(state);
                    }
                }
            }
            m_lastParameters.reset();
        }
        if (!updateSettings()) {
            throw std::runtime_error("Tracktion cannot restore the effect settings.");
        }
    }

    void initialise(const tracktion::PluginInitialisationInfo& info) override
    {
        const juce::ScopedLock lock(m_settingsLock);
        const juce::ScopedLock processorGuard(callbackLock());
        if (m_editor) {
            m_editor->beginPlayback();
            m_editorPlaybackRegistered = true;
        }
        ExternalPlugin::initialise(info);
        auto* processor = getAudioPluginInstance();
        m_blockSize = info.blockSizeSamples;
        m_auxInputs = processor ? processor->getTotalNumInputChannels() - m_inputs : 0;
        m_auxOutputs = processor ? processor->getTotalNumOutputChannels() - m_outputs : 0;
        const int audioChannels = std::max(m_inputs + m_auxInputs, m_outputs + m_auxOutputs);
        m_editorIdleAudio.setSize(audioChannels, 0);
        m_inputAudio.setSize(m_channels + m_auxInputs, m_blockSize);
        for (auto& entry : m_processors) {
            if (entry.instance) {
                applyTracktionAutomation(*entry.instance, m_effect->Automation());
                entry.instance->baseClassInitialise(info);
            }
            entry.audio.setSize(audioChannels, m_blockSize);
        }
        m_delaySamples = juce::roundToInt(getLatencySeconds() * info.sampleRate);
        m_delayChannels = std::max(m_channels, getNumOutputChannelsGivenInputs(m_channels));
        m_bypassDelay.setLatencyNumSamples(m_delaySamples);
        m_bypassDelay.prepareToPlay(info.sampleRate, info.blockSizeSamples, m_delayChannels);
        if (m_editor) {
            m_editor->beginPlayback([this]() { serviceEditorIdle(); });
        }
    }

    void deinitialise() override
    {
        if (m_editor) {
            m_editor->endPlayback();
            m_editorPlaybackRegistered = false;
        }
        syncEditorSettings();
        const juce::ScopedLock lock(m_settingsLock);
        const juce::ScopedLock processorGuard(callbackLock());
        for (auto& entry : m_processors) {
            if (entry.instance) {
                entry.instance->baseClassDeinitialise();
            }
        }
        ExternalPlugin::deinitialise();
    }

    void reset() override
    {
        const juce::ScopedLock lock(m_settingsLock);
        const juce::ScopedLock processorGuard(callbackLock());
        ExternalPlugin::reset();
        for (auto& entry : m_processors) {
            if (entry.instance) {
                entry.instance->reset();
            }
        }
    }

    void applyToBuffer(const tracktion::PluginRenderContext& context) override
    {
        const juce::ScopedLock lock(m_settingsLock);
        const juce::ScopedLock processorGuard(callbackLock());
        if (!context.destBuffer) {
            ExternalPlugin::applyToBuffer(context);
            return;
        }
        for (int offset = 0; offset < context.bufferNumSamples; offset += m_blockSize) {
            const int frames = std::min(m_blockSize, context.bufferNumSamples - offset);
            const int start = context.bufferStartSample + offset;
            juce::AudioBuffer<float> audio(context.destBuffer->getArrayOfWritePointers(),
                                           std::min(m_delayChannels, context.destBuffer->getNumChannels()), start, frames);
            const auto view = tracktion::graph::toBufferView(audio);
            if (m_delaySamples > 0) {
                m_bypassDelay.writeAudio(view);
            }
            if (m_active.load(std::memory_order_relaxed)) {
                tracktion::PluginRenderContext block(context);
                block.bufferStartSample = start;
                block.bufferNumSamples = frames;
                block.editTime = { context.getEditTimeAtSample(offset), context.getEditTimeAtSample(offset + frames) };
                block.referenceSampleRange = { context.referenceSampleRange.getStart() + offset,
                                               context.referenceSampleRange.getStart() + offset + frames };
                if (m_routing) {
                    processChannels(block);
                } else {
                    ExternalPlugin::applyToBuffer(block);
                }
                if (m_delaySamples > 0) {
                    m_bypassDelay.clearAudio(frames);
                }
            } else if (m_delaySamples > 0) {
                m_bypassDelay.readAudioOverwriting(view);
            }
        }
    }

private:
    void serviceEditorIdle()
    {
        const juce::ScopedLock lock(m_settingsLock);
        const juce::ScopedLock processorGuard(callbackLock());
        const auto& settings = m_effect->GetAccess()->Get();
        const bool active = settings.extra.GetActive() && m_list.IsActive();
        m_active.store(active, std::memory_order_relaxed);
        const bool paused = edit.getTransport().isPaused();
        auto* processor = getAudioPluginInstance();
        auto* client = processor ? processor->getLV2Client() : nullptr;
        if (!client || (active && !paused)) {
            return;
        }
        const auto time = edit.getTransport().getPosition();
        const auto rate = client->getInstanceSampleRate();
        const auto sample = tracktion::toSamples(time, rate);
        tracktion::PluginRenderContext context(&m_editorIdleAudio, 0, 0, nullptr, 0.0, { time, time },
                                               edit.getTransport().isPlaying() && !paused, false, false, false);
        context.referenceSampleRange = { sample, sample };
        context.referenceSampleRate = rate;
        ExternalPlugin::applyToBuffer(context);
    }

    const juce::CriticalSection& callbackLock() const
    {
        const auto* processor = getAudioPluginInstance();
        return processor ? processor->getCallbackLock() : m_settingsLock;
    }

    tracktion::ExternalPlugin& instanceFor(Processor& processor)
    {
        return processor.instance ? *processor.instance : *this;
    }

    void processChannels(const tracktion::PluginRenderContext& context)
    {
        const int frames = context.bufferNumSamples;
        const int available = context.destBuffer->getNumChannels();
        m_inputAudio.clear();
        for (int channel = 0; channel < std::min(m_inputAudio.getNumChannels(), available); ++channel) {
            m_inputAudio.copyFrom(channel, 0, *context.destBuffer, channel, context.bufferStartSample, frames);
        }
        int input = 0;
        int output = 0;
        for (auto& entry : m_processors) {
            entry.audio.clear();
            for (int channel = 0; channel < m_inputs; ++channel) {
                entry.audio.copyFrom(channel, 0, m_inputAudio, (input + channel) % m_channels, 0, frames);
            }
            for (int channel = 0; channel < m_auxInputs; ++channel) {
                entry.audio.copyFrom(m_inputs + channel, 0, m_inputAudio, m_channels + channel, 0, frames);
            }
            tracktion::PluginRenderContext block(context);
            block.destBuffer = &entry.audio;
            block.bufferStartSample = 0;
            if (output > 0) {
                block.bufferForMidiMessages = nullptr;
            }
            if (entry.instance) {
                entry.instance->updateAutomatableParamPosition(block.editTime.getStart());
                entry.instance->applyToBuffer(block);
            } else {
                ExternalPlugin::applyToBuffer(block);
            }
            for (int channel = 0; channel < std::min(m_outputs, m_channels - output); ++channel) {
                if (output + channel < available) {
                    context.destBuffer->copyFrom(output + channel, context.bufferStartSample, entry.audio, channel, 0, frames);
                }
            }
            if (output == 0) {
                for (int channel = 0; channel < m_auxOutputs && m_channels + channel < available; ++channel) {
                    context.destBuffer->copyFrom(m_channels + channel, context.bufferStartSample,
                                                 entry.audio, m_outputs + channel, 0, frames);
                }
            }
            input = (input + m_inputs) % m_channels;
            output += m_outputs;
        }
    }

    void timerCallback() override { updateSettings(); }

    bool updateSettings()
    {
        syncEditorSettings();
        const auto access = m_effect->GetAccess();
        const auto& settings = access->Get();
        {
            const juce::ScopedLock lock(m_settingsLock);
            const juce::ScopedLock processorGuard(callbackLock());
            m_active.store(settings.extra.GetActive() && (m_live ? m_list.IsActive() : m_listActive), std::memory_order_relaxed);
        }
        auto* processor = getAudioPluginInstance();
        if (!processor) {
            return false;
        }
        if (m_processors.empty()) {
            initialiseFully();
            return m_lastParameters.has_value();
        }
        CommandParameters parameters;
        if (!m_effect->GetEffect()->SaveSettings(settings, parameters)) {
            return false;
        }
        wxString serialized;
        parameters.GetParameters(serialized);
        if (m_lastParameters && *m_lastParameters == serialized) {
            if (m_editor && m_editor->settingsMatchProcessor(settings)) {
                m_lastEditorStateRevision = m_editor->stateRevision();
            }
            return true;
        }
        const juce::ScopedLock lock(m_settingsLock);
        const juce::ScopedLock processorGuard(callbackLock());
        const bool editorSnapshot = m_editor && m_editor->settingsMatchProcessor(settings);
        const bool parameterUpdate = editorSnapshot && m_lastEditorStateRevision
                                     && *m_lastEditorStateRevision == m_editor->stateRevision();
        const auto states = vst3State(parameters);
        const auto parameterIds = vst3ParameterIds(parameters);
        bool ready = true;
        for (auto& entry : m_processors) {
            auto* instance = instanceFor(entry).getAudioPluginInstance();
            if (!instance) {
                ready = false;
                continue;
            }
            if (isVST3()) {
                const bool primary = &instanceFor(entry) == this;
                const bool removed = !std::includes(parameterIds.begin(), parameterIds.end(),
                                                    entry.parameterIds.begin(), entry.parameterIds.end());
                if (primary && m_editor) {
                    if (!editorSnapshot) {
                        const bool parametersOnly = entry.lastVstState && *entry.lastVstState == states && !removed;
                        const bool restored = parametersOnly ? m_editor->loadParameterSettings(settings)
                                              : m_editor->loadSettings(settings, true);
                        if (!restored) {
                            ready = false;
                            continue;
                        }
                    }
                    entry.lastVstState = states;
                } else if (parameterUpdate && entry.lastVstState && !removed) {
                    applyVst3Parameters(*instance, parameters);
                    entry.lastVstState = states;
                } else {
                    if (removed) {
                        entry.lastVstState.reset();
                    }
                    if (!applyVst3Settings(entry, *instance, parameters)) {
                        ready = false;
                        continue;
                    }
                }
                entry.parameterIds = parameterIds;
            }
            if (instance->getLV2Client()) {
                const bool stateUnchanged = entry.lastLv2Settings && sameTracktionLv2State(*entry.lastLv2Settings, settings);
                const bool primary = &instanceFor(entry) == this;
                bool restored = true;
                if (primary && m_editor) {
                    if (!editorSnapshot) {
                        restored = stateUnchanged ? m_editor->loadParameterSettings(settings) : m_editor->loadSettings(settings, true);
                    }
                } else {
                    if (!stateUnchanged && !parameterUpdate && entry.defaultState) {
                        restored = instance->getLV2Client()->restoreState(*entry.defaultState);
                    }
                    if (restored) {
                        restored = applyTracktionLv2Settings(*instance, settings, !stateUnchanged && !parameterUpdate);
                    }
                }
                if (restored) {
                    entry.lastLv2Settings = settings;
                } else {
                    ready = false;
                }
            }
#if JUCE_PLUGINHOST_AU
            auto* client = instance->getAudioUnitClient();
            if (client) {
                const auto preset = audioUnitPresetNumber(parameters);
                const auto state = audioUnitState(parameters);
                const auto ids = audioUnitParameterIds(parameters);
                const bool removed = !std::includes(ids.begin(), ids.end(), entry.parameterIds.begin(), entry.parameterIds.end());
                const bool parametersOnly = entry.lastAudioUnitPreset && *entry.lastAudioUnitPreset == preset
                                            && entry.lastAudioUnitState && *entry.lastAudioUnitState == state && !removed;
                const bool primary = &instanceFor(entry) == this;
                if (primary && m_editor) {
                    if (!editorSnapshot) {
                        const bool restored = parametersOnly ? m_editor->loadParameterSettings(settings)
                                              : m_editor->loadSettings(settings, true);
                        if (!restored) {
                            ready = false;
                            continue;
                        }
                    }
                } else {
                    if (!parametersOnly && (!parameterUpdate || removed)) {
                        if (state) {
                            if (!applyAudioUnitState(*instance, *state)) {
                                ready = false;
                                continue;
                            }
                        } else {
                            if (entry.defaultState) {
                                instance->setStateInformation(entry.defaultState->getData(),
                                                              static_cast<int>(entry.defaultState->getSize()));
                            }
                            applyAudioUnitPreset(client->getAudioUnitHandle(), parameters);
                        }
                    }
                    applyAudioUnitParameters(*instance, parameters);
                }
                entry.lastAudioUnitPreset = preset;
                entry.lastAudioUnitState = state;
                entry.parameterIds = ids;
            }
#endif
        }
        if (ready) {
            m_lastParameters = std::move(serialized);
            if (m_editor) {
                m_lastEditorStateRevision = m_editor->stateRevision();
            }
            if (!m_live) {
                stopTimer();
            }
        }
        return ready;
    }

    void syncEditorSettings()
    {
        if (!m_editor || !m_lastParameters) {
            return;
        }
        const auto access = m_effect->GetAccess();
        const auto& settings = access->Get();
        CommandParameters parameters;
        if (!m_effect->GetEffect()->SaveSettings(settings, parameters)) {
            return;
        }
        wxString serialized;
        parameters.GetParameters(serialized);
        if (serialized == *m_lastParameters && !m_editor->settingsMatchProcessor(settings)) {
            access->ModifySettings([this](EffectSettings& updated) {
                m_editor->savePlaybackSettings(updated);
                return nullptr;
            });
            access->Flush();
        }
    }

    bool applyVst3Settings(Processor& entry, juce::AudioPluginInstance& processor, const CommandParameters& parameters)
    {
        if (!entry.defaultState) {
            juce::MemoryBlock state;
            processor.getStateInformation(state);
            entry.defaultState = std::move(state);
        }
        const auto states = vst3State(parameters);
        if (!entry.lastVstState || *entry.lastVstState != states) {
            if (!states.first && !states.second) {
                processor.setStateInformation(entry.defaultState->getData(), static_cast<int>(entry.defaultState->getSize()));
            } else {
                juce::MemoryBlock previous;
                processor.getStateInformation(previous);
                if (!applyVst3State(processor, states)) {
                    processor.setStateInformation(previous.getData(), static_cast<int>(previous.getSize()));
                    return false;
                }
            }
            entry.lastVstState = states;
        }
        applyVst3Parameters(processor, parameters);
        return true;
    }

    std::shared_ptr<TracktionHostedEffectInstance> m_editor;
    std::shared_ptr<RealtimeEffectState> m_effect;
    const RealtimeEffectList& m_list;
    int m_channels;
    bool m_live;
    bool m_listActive;
    bool m_useLatency = true;
    bool m_editorPlaybackRegistered = false;
    std::atomic<bool> m_active { true };
    std::optional<wxString> m_lastParameters;
    std::optional<uint64_t> m_lastEditorStateRevision;
    std::vector<Processor> m_processors;
    juce::AudioBuffer<float> m_inputAudio;
    juce::AudioBuffer<float> m_editorIdleAudio;
    juce::CriticalSection m_settingsLock;
    tracktion::graph::LatencyProcessor m_bypassDelay;
    int m_delaySamples = 0;
    int m_delayChannels = 0;
    int m_blockSize = 512;
    int m_inputs = 0;
    int m_outputs = 0;
    int m_auxInputs = 0;
    int m_auxOutputs = 0;
    bool m_routing = false;
};
}

bool au::tracktionaudio::supportsAudacityExternalEffect(const EffectInstanceFactory& effect)
{
    const auto family = effect.GetFamily().Internal();
    return family == wxT("VST3") || (JUCE_PLUGINHOST_AU && family == wxT("AudioUnit"))
           || (JUCE_PLUGINHOST_LV2 && family == wxT("LV2"));
}

tracktion::Plugin* au::tracktionaudio::createAudacityExternalEffect(tracktion::PluginCreationInfo info,
                                                                    const std::shared_ptr<RealtimeEffectState>& state,
                                                                    const RealtimeEffectList& list, int channels, bool live)
{
    std::shared_ptr<TracktionHostedEffectInstance> editor;
    if (live) {
        editor = std::dynamic_pointer_cast<TracktionHostedEffectInstance>(state->GetInstance());
        if (editor && !editor->Init()) {
            throw std::runtime_error(editor->GetLastError());
        }
        if (editor && editor->hasSettingsChanges()) {
            const auto access = state->GetAccess();
            access->ModifySettings([&editor](EffectSettings& settings) {
                editor->saveSettings(settings);
                return nullptr;
            });
            access->Flush();
        }
    }
    auto nativeState = externalState(info.edit, *state->GetEffect());
    nativeState.setProperty(tracktion::IDs::type, info.state[tracktion::IDs::type], nullptr);
    nativeState.setProperty(AUDACITY_EFFECT_STATE_PROPERTY, info.state[AUDACITY_EFFECT_STATE_PROPERTY], nullptr);
    auto plugin = std::make_unique<AudacityExternalEffectPlugin>(tracktion::PluginCreationInfo { info.edit, nativeState, info.isNewPlugin },
                                                                 state, list, channels, live, std::move(editor));
    waitForTracktionPluginLoad(*plugin);
    plugin->initialiseFully();
    return plugin.release();
}

tracktion::ExternalPlugin* au::tracktionaudio::createTracktionExternalProcessor(tracktion::Edit& edit, const EffectInstanceFactory& effect)
{
    const auto state = externalState(edit, effect);
    auto plugin = std::make_unique<tracktion::ExternalPlugin>(tracktion::PluginCreationInfo { edit, state, true });
    waitForTracktionPluginLoad(*plugin);
    return plugin.release();
}

bool au::tracktionaudio::applyAudacityExternalEffectSettings(tracktion::ExternalPlugin& plugin, const EffectInstanceFactory& effect,
                                                             const EffectSettings& settings)
{
    auto* processor = plugin.getAudioPluginInstance();
    if (processor && processor->getLV2Client()) {
        return applyTracktionLv2Settings(*processor, settings, true);
    }
    CommandParameters parameters;
    if (!processor || !effect.SaveSettings(settings, parameters)) {
        return false;
    }
    if (plugin.isVST3()) {
        const auto states = vst3State(parameters);
        if ((states.first || states.second) && !applyVst3State(*processor, states)) {
            return false;
        }
        applyVst3Parameters(*processor, parameters);
    }
#if JUCE_PLUGINHOST_AU
    auto* client = processor->getAudioUnitClient();
    if (client) {
        return applyAudioUnitSettings(*processor, parameters);
    }
#endif
    return true;
}

bool au::tracktionaudio::applyAudacityExternalEffectParameters(tracktion::ExternalPlugin& plugin, const EffectInstanceFactory& effect,
                                                               const EffectSettings& settings)
{
    auto* processor = plugin.getAudioPluginInstance();
    if (processor && processor->getLV2Client()) {
        return applyTracktionLv2Settings(*processor, settings, false);
    }
    CommandParameters parameters;
    if (!processor || !effect.SaveSettings(settings, parameters)) {
        return false;
    }
    if (plugin.isVST3()) {
        applyVst3Parameters(*processor, parameters);
        return true;
    }
#if JUCE_PLUGINHOST_AU
    if (processor->getAudioUnitClient()) {
        applyAudioUnitParameters(*processor, parameters);
        return true;
    }
#endif
    return false;
}

bool au::tracktionaudio::storeAudacityVst3EffectSettings(tracktion::ExternalPlugin& plugin, const EffectInstanceFactory& effect,
                                                         EffectSettings& settings, const juce::MemoryBlock* suppliedState)
{
    auto* processor = plugin.getAudioPluginInstance();
    auto* client = processor ? processor->getVST3Client() : nullptr;
    if (!client) {
        return false;
    }
    juce::MemoryBlock saved;
    if (!suppliedState) {
        processor->getStateInformation(saved);
        suppliedState = &saved;
    }
    const auto xml = juce::AudioProcessor::getXmlFromBinary(suppliedState->getData(), static_cast<int>(suppliedState->getSize()));
    if (!xml) {
        return false;
    }
    CommandParameters parameters;
    for (const auto& entry : { std::pair { "IComponent", "ProcessorState" }, { "IEditController", "ControllerState" } }) {
        const auto* state = xml->getChildByName(entry.first);
        if (state) {
            juce::MemoryBlock data;
            if (!data.fromBase64Encoding(state->getAllSubText())) {
                return false;
            }
            parameters.Write(entry.second, wxBase64Encode(data.getData(), data.getSize()));
        }
    }
    wxString changes;
    auto* controller = client->getIEditControllerPtr();
    std::set<juce::String> readOnly;
    if (controller) {
        for (Steinberg::int32 index = 0; index < controller->getParameterCount(); ++index) {
            Steinberg::Vst::ParameterInfo info {};
            if (controller->getParameterInfo(index, info) == Steinberg::kResultOk
                && (info.flags & Steinberg::Vst::ParameterInfo::kIsReadOnly)) {
                readOnly.emplace(static_cast<juce::int64>(info.id));
            }
        }
    }
    for (const auto* parameter : processor->getParameters()) {
        const auto* hosted = dynamic_cast<const juce::HostedAudioProcessorParameter*>(parameter);
        if (hosted && !readOnly.contains(hosted->getParameterID())) {
            changes += wxString::FromUTF8(hosted->getParameterID().toStdString()) + "="
                       + wxString::Format("%.17g", static_cast<double>(parameter->getValue())) + ";";
        }
    }
    if (!changes.empty()) {
        parameters.Write("Parameters", changes);
    }
    return effect.LoadSettings(parameters, settings);
}

bool au::tracktionaudio::storeAudacityAudioUnitEffectSettings(tracktion::ExternalPlugin& plugin, EffectSettings& settings,
                                                              const juce::MemoryBlock* suppliedState)
{
#if AU_TRACKTION_AUDIO_UNIT
    auto* processor = plugin.getAudioPluginInstance();
    auto* client = processor ? processor->getAudioUnitClient() : nullptr;
    if (!client) {
        return false;
    }
    const auto unit = client->getAudioUnitHandle();
    AudioUnitWrapper::Parameters parameters;
    const auto error = AudioUnitUtils::GetVariableSizeProperty(unit, kAudioUnitProperty_ParameterList, parameters);
    if (error != noErr && !processor->getParameters().isEmpty()) {
        return false;
    }
    juce::MemoryBlock state;
    if (!suppliedState) {
        processor->getStateInformation(state);
        suppliedState = &state;
    }
    auto& values = AudioUnitWrapper::GetSettings(settings);
    const auto previous = values.mState;
    values.ResetValues();
    if (!AudioUnitWrapper::FetchSettings(unit, parameters, values, true, true)) {
        return false;
    }
    if (suppliedState->getSize() != 0) {
        const auto* bytes = static_cast<const unsigned char*>(suppliedState->getData());
        if (previous && previous->size() == suppliedState->getSize()
            && std::equal(previous->begin(), previous->end(), bytes)) {
            values.mState = previous;
        } else {
            values.mState = std::make_shared<const AudioUnitEffectSettings::State>(bytes, bytes + suppliedState->getSize());
        }
    }
    return true;
#else
    (void)plugin;
    (void)settings;
    (void)suppliedState;
    return false;
#endif
}
