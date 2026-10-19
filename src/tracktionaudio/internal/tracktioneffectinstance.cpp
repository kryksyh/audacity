/*
* Audacity: A Digital Audio Editor
*/
#include <tracktion_engine/tracktion_engine.h>

#include "tracktioneffectinstance.h"
#include "audacityexternaleffect.h"
#include "tracktionruntime.h"
#include "tracktionlv2settings.h"

#include <stdexcept>
#include <atomic>
#include <map>
#include <pluginterfaces/vst/ivstcomponent.h>
#include <pluginterfaces/vst/ivsteditcontroller.h>
#include <pluginterfaces/vst/ivstunits.h>

#if JUCE_PLUGINHOST_AU
#include <AudioToolbox/AudioToolbox.h>
#endif

#include "au3-effects/PerTrackEffect.h"
#include "au3-module-manager/ConfigInterface.h"
#include "global/defer.h"
#include "global/modularity/ioc.h"
#include "audioplugins/iaudiopluginsloadguard.h"
#include "effects/effects_base/internal/effectsutils.h"

using namespace au::tracktionaudio;
namespace effects = au::effects;

namespace {
class TracktionEffectInstance final : public TracktionHostedEffectInstance, private juce::AudioProcessorListener
{
public:
    TracktionEffectInstance(TracktionRuntime& runtime, const PerTrackEffect& effect)
        : TracktionHostedEffectInstance(effect), m_runtimeLifetime(runtime.lifetime())
    {
        int blockSize = 8192;
        GetConfig(effect, PluginSettings::Shared, wxT("Options"), wxT("BufferSize"), blockSize, 8192);
        m_blockLimit = std::max(1, blockSize);
        m_blockSize = m_blockLimit;
        GetConfig(effect, PluginSettings::Shared, wxT("Options"), wxT("UseLatency"), m_useLatency, true);
        try {
            m_edit = std::make_unique<tracktion::Edit>(tracktion::Edit::Options {
                    .engine = runtime.engine(),
                    .editState = tracktion::createEmptyEdit(runtime.engine()),
                    .editProjectItemID = {},
                    .role = tracktion::Edit::forRendering,
                    .numAudioTracks = 0,
                    .defaultMasterVolumedB = 0.0f,
                });
            m_plugin = createTracktionExternalProcessor(*m_edit, effect);
            auto* processor = m_plugin->getAudioPluginInstance();
            if (!processor) {
                throw std::runtime_error("Tracktion cannot load the effect.");
            }
            processor->getStateInformation(m_defaultState);
            m_cachedState = m_defaultState;
            if (processor->getLV2Client()) {
                m_lv2Layout = makeTracktionLv2Settings(*processor, &m_defaultState);
            }
            m_name = processor->getName().toStdString();
            processor->addListener(this);
            ensureParameters();
        } catch (const std::exception& error) {
            m_error = error.what();
        }
    }

    ~TracktionEffectInstance() override
    {
        auto* processor = m_plugin ? m_plugin->getAudioPluginInstance() : nullptr;
        if (processor) {
            processor->removeListener(this);
        }
        ProcessFinalize();
    }

    const std::string& name() const override { return m_name; }
    Steinberg::Vst::IComponent* vstComponent() const override
    {
        const auto* processor = m_plugin ? m_plugin->getAudioPluginInstance() : nullptr;
        const auto* client = processor ? processor->getVST3Client() : nullptr;
        return client ? client->getIComponentPtr() : nullptr;
    }

    Steinberg::Vst::IEditController* vstController() const override
    {
        const auto* processor = m_plugin ? m_plugin->getAudioPluginInstance() : nullptr;
        const auto* client = processor ? processor->getVST3Client() : nullptr;
        return client ? client->getIEditControllerPtr() : nullptr;
    }

    ComponentInstanceRecord* audioUnit() const override
    {
#if JUCE_PLUGINHOST_AU
        const auto* processor = m_plugin ? m_plugin->getAudioPluginInstance() : nullptr;
        const auto* client = processor ? processor->getAudioUnitClient() : nullptr;
        return client ? client->getAudioUnitHandle() : nullptr;
#else
        return nullptr;
#endif
    }

    bool openAudioUnitEditor() override
    {
#if JUCE_PLUGINHOST_AU
        auto* processor = m_plugin ? m_plugin->getAudioPluginInstance() : nullptr;
        if (processor) {
            const juce::ScopedLock lock(processor->getCallbackLock());
            auto* client = processor->getAudioUnitClient();
            return client && client->prepareForEditor();
        }
#endif
        return false;
    }

    void closeAudioUnitEditor() override
    {
#if JUCE_PLUGINHOST_AU
        auto* processor = m_plugin ? m_plugin->getAudioPluginInstance() : nullptr;
        if (processor) {
            const juce::ScopedLock lock(processor->getCallbackLock());
            auto* client = processor->getAudioUnitClient();
            if (client) {
                client->editorClosed();
            }
        }
#endif
    }

    bool prepareLv2Editor(double sampleRate) override
    {
        auto* processor = m_plugin ? m_plugin->getAudioPluginInstance() : nullptr;
        if (!processor || !processor->getLV2Client()) {
            return false;
        }
        if (m_playbackActive || m_initialised) {
            return true;
        }
        const juce::ScopedLock lock(processor->getCallbackLock());
        const bool applying = m_applyingSettings.exchange(true);
        DEFER { m_applyingSettings.store(applying);
        };
        try {
            DEFER { m_plugin->baseClassDeinitialise();
            };
            m_plugin->baseClassInitialise({ tracktion::TimePosition(), sampleRate, m_edit->engine.getDeviceManager().getBlockSize() });
            return true;
        } catch (const std::exception& error) {
            m_error = error.what();
            return false;
        }
    }

    bool loadSettings(const EffectSettings& settings, bool defaults) override
    {
        auto* processor = m_plugin ? m_plugin->getAudioPluginInstance() : nullptr;
        if (!processor) {
            return false;
        }
        const juce::ScopedLock lock(processor->getCallbackLock());
        if (settingsMatchProcessor(settings)) {
            return true;
        }
        const bool applying = m_applyingSettings.exchange(true);
        DEFER { m_applyingSettings.store(applying);
        };
        juce::MemoryBlock previous;
        processor->getStateInformation(previous);
        if (defaults) {
            processor->setStateInformation(m_defaultState.getData(), static_cast<int>(m_defaultState.getSize()));
        }
        const bool result = applyAudacityExternalEffectSettings(*m_plugin, mProcessor, settings);
        if (!result) {
            if (previous.getSize() != 0) {
                processor->setStateInformation(previous.getData(), static_cast<int>(previous.getSize()));
            }
            return false;
        }
        juce::MemoryBlock flushed;
        processor->getStateInformation(flushed);
        m_stateRevision.fetch_add(1);
        m_cachedState = std::move(flushed);
        m_cachedStateRevision = m_stateRevision.load();
        rememberSettings(settings);
        m_settingsDirty.store(false);
        return result;
    }

    bool loadParameterSettings(const EffectSettings& settings) override
    {
        auto* processor = m_plugin ? m_plugin->getAudioPluginInstance() : nullptr;
        if (!processor) {
            return false;
        }
        const juce::ScopedLock lock(processor->getCallbackLock());
        const bool applying = m_applyingSettings.exchange(true);
        DEFER { m_applyingSettings.store(applying);
        };
        const bool result = applyAudacityExternalEffectParameters(*m_plugin, mProcessor, settings);
        if (result) {
            if (audioUnit() || processor->getLV2Client()) {
                processor->getStateInformation(m_cachedState);
                m_cachedStateRevision = m_stateRevision.load();
            }
            rememberSettings(settings);
            m_settingsDirty.store(false);
        }
        return result;
    }

    bool loadVst3PresetFile(const std::string& path) override
    {
        auto* processor = m_plugin ? m_plugin->getAudioPluginInstance() : nullptr;
        auto* client = processor ? processor->getVST3Client() : nullptr;
        juce::MemoryBlock preset;
        if (!client || !juce::File(path).loadFileAsData(preset)) {
            return false;
        }
        const juce::ScopedLock lock(processor->getCallbackLock());
        const bool applying = m_applyingSettings.exchange(true);
        DEFER { m_applyingSettings.store(applying);
        };
        juce::MemoryBlock previous;
        processor->getStateInformation(previous);
        const bool result = client->setPreset(preset);
        if (result) {
            processor->getStateInformation(m_cachedState);
            // Update the JUCE parameter cache after the VST3 preset is loaded.
            processor->setStateInformation(m_cachedState.getData(), static_cast<int>(m_cachedState.getSize()));
            m_stateRevision.fetch_add(1);
            m_cachedStateRevision = m_stateRevision.load();
            m_lastSettings.reset();
            m_parameterInfoDirty.store(true);
            m_settingsDirty.store(true);
        } else {
            processor->setStateInformation(previous.getData(), static_cast<int>(previous.getSize()));
        }
        return result;
    }

    bool saveSettings(EffectSettings& settings) override
    {
        return saveSettings(settings, true);
    }

    bool savePlaybackSettings(EffectSettings& settings) override
    {
        return saveSettings(settings, !m_playbackActive);
    }

    void beginPlayback(std::function<void()> editorIdleService) override
    {
        m_playbackActive = true;
        m_editorIdleService = std::move(editorIdleService);
    }

    void endPlayback() override
    {
        m_playbackActive = false;
        m_editorIdleService = {};
    }

    void processEditorIdle() override
    {
        auto* processor = m_plugin ? m_plugin->getAudioPluginInstance() : nullptr;
        auto* client = processor ? processor->getLV2Client() : nullptr;
        if (!client || m_initialised) {
            return;
        }
        if (m_playbackActive) {
            const auto service = m_editorIdleService;
            if (service) {
                service();
            }
        } else {
            client->processUiIdle();
        }
    }

    bool saveSettings(EffectSettings& settings, bool complete)
    {
        processEditorIdle();
        auto* processor = m_plugin ? m_plugin->getAudioPluginInstance() : nullptr;
        if (!processor) {
            return false;
        }
        const juce::ScopedLock lock(processor->getCallbackLock());
        ensureParameters();
        const bool applying = m_applyingSettings.exchange(true);
        DEFER { m_applyingSettings.store(applying);
        };
        if (complete || m_cachedStateRevision != m_stateRevision.load()) {
            processor->getStateInformation(m_cachedState);
            m_cachedStateRevision = m_stateRevision.load();
        }
        const bool result = processor->getLV2Client() ? captureTracktionLv2Settings(*processor, settings, &m_cachedState)
                            : m_plugin->isVST3() ? storeAudacityVst3EffectSettings(*m_plugin, mProcessor, settings, &m_cachedState)
                            : storeAudacityAudioUnitEffectSettings(*m_plugin, settings, &m_cachedState);
        if (result) {
            rememberSettings(settings);
        }
        return result;
    }

    bool takeSettingsChanges() override { return m_settingsDirty.exchange(false); }
    bool hasSettingsChanges() const override { return m_settingsDirty.load(); }
    uint64_t stateRevision() const override { return m_stateRevision.load(); }
    tracktion::ExternalPlugin& tracktionPlugin() const override { return *m_plugin; }
    const juce::MemoryBlock& defaultState() const override { return m_defaultState; }

    bool settingsMatchProcessor(const EffectSettings& settings) const override
    {
        const auto value = serializedSettings(settings);
        return value && m_lastSettings && *value == *m_lastSettings
               && m_snapshotParameterRevision == m_parameterRevision.load() && m_snapshotStateRevision == m_stateRevision.load();
    }

    effects::ParameterInfoList parameters() const override
    {
        const auto* processor = m_plugin ? m_plugin->getAudioPluginInstance() : nullptr;
        if (processor && processor->getLV2Client()) {
            return tracktionLv2Parameters(*processor, m_lv2Layout);
        }
        effects::ParameterInfoList result;
        ensureParameters();
        const auto order = m_parameterOrder;
        for (const auto& id : order) {
            const auto info = parameter(id);
            if (info.isValid() && !info.isHidden) {
                result.push_back(info);
            }
        }
        return result;
    }

    effects::ParameterInfo parameter(const muse::String& id) const override
    {
        auto* processor = m_plugin ? m_plugin->getAudioPluginInstance() : nullptr;
        if (processor && processor->getLV2Client()) {
            return tracktionLv2Parameter(*processor, m_lv2Layout, id);
        }
        auto* controller = vstController();
        auto* value = findParameter(id);
        bool valid = false;
        const auto number = id.toUInt(&valid);
        if (!value || !valid) {
            return {};
        }
        const auto& binding = m_parameters.at(id);
        if (binding.audioUnitInfo) {
            auto result = *binding.audioUnitInfo;
            result.currentValue = result.minValue + (result.maxValue - result.minValue) * value->getValue();
            result.currentValueString = parameterValueString(id, value->getValue());
            return result;
        }
        if (!controller) {
            return {};
        }
        const auto info = binding.info;
        if (info.flags & Steinberg::Vst::ParameterInfo::kIsProgramChange) {
            return {};
        }
        const auto text = [](const Steinberg::Vst::TChar* input) {
            return muse::String(input);
        };
        effects::ParameterInfo result;
        result.id = id;
        result.name = text(info.title);
        result.units = text(info.units);
        const auto units = Steinberg::FUnknownPtr<Steinberg::Vst::IUnitInfo>(controller);
        if (result.units.empty() && units && info.unitId != Steinberg::Vst::kRootUnitId) {
            for (Steinberg::int32 index = 0; index < units->getUnitCount(); ++index) {
                Steinberg::Vst::UnitInfo unit {};
                if (units->getUnitInfo(index, unit) == Steinberg::kResultOk && unit.id == info.unitId) {
                    result.units = text(unit.name);
                    break;
                }
            }
        }
        result.isReadOnly = (info.flags & Steinberg::Vst::ParameterInfo::kIsReadOnly) != 0;
        result.isHidden = (info.flags & Steinberg::Vst::ParameterInfo::kIsHidden) != 0;
        result.canAutomate = (info.flags & Steinberg::Vst::ParameterInfo::kCanAutomate) != 0;
        result.type = result.isReadOnly ? effects::ParameterType::ReadOnly
                      : info.flags & Steinberg::Vst::ParameterInfo::kIsList ? effects::ParameterType::Dropdown
                      : info.stepCount == 1 ? effects::ParameterType::Toggle
                      : info.stepCount > 1 ? effects::ParameterType::Dropdown : effects::ParameterType::Slider;
        result.minValue = controller->normalizedParamToPlain(number, 0.0);
        result.maxValue = controller->normalizedParamToPlain(number, 1.0);
        result.defaultValue = controller->normalizedParamToPlain(number, info.defaultNormalizedValue);
        result.currentValue = controller->normalizedParamToPlain(number, value->getValue());
        result.currentValueString = parameterValueString(id, value->getValue());
        result.stepCount = info.stepCount;
        if (info.stepCount > 0) {
            result.stepSize = (result.maxValue - result.minValue) / info.stepCount;
        }
        if (result.type == effects::ParameterType::Dropdown && info.stepCount > 0) {
            for (int index = 0; index <= info.stepCount; ++index) {
                const double normalized = double(index) / info.stepCount;
                result.enumValues.push_back(parameterValueString(id, normalized));
                result.enumIndices.push_back(controller->normalizedParamToPlain(number, normalized));
            }
        }
        return result;
    }

    double parameterValue(const muse::String& id) const override
    {
        auto* processor = m_plugin ? m_plugin->getAudioPluginInstance() : nullptr;
        if (processor && processor->getLV2Client()) {
            return tracktionLv2Parameter(*processor, m_lv2Layout, id).currentValue;
        }
        auto* controller = vstController();
        const auto* value = findParameter(id);
        bool valid = false;
        const auto number = id.toUInt(&valid);
        if (!value || !valid) {
            return 0.0;
        }
        const auto& binding = m_parameters.at(id);
        if (binding.audioUnitInfo) {
            const auto& info = *binding.audioUnitInfo;
            return info.minValue + (info.maxValue - info.minValue) * value->getValue();
        }
        return controller ? controller->normalizedParamToPlain(number, value->getValue()) : 0.0;
    }

    bool setParameterValue(const muse::String& id, double plain) override
    {
        auto* processor = m_plugin ? m_plugin->getAudioPluginInstance() : nullptr;
        if (processor && processor->getLV2Client()) {
            return setTracktionLv2Parameter(*processor, m_lv2Layout, id, plain);
        }
        auto* controller = vstController();
        auto* value = findParameter(id);
        bool valid = false;
        const auto number = id.toUInt(&valid);
        if (!value || !valid) {
            return false;
        }
        const auto& binding = m_parameters.at(id);
        if (binding.audioUnitInfo) {
            const auto& info = *binding.audioUnitInfo;
            if (info.isReadOnly || info.minValue == info.maxValue) {
                return false;
            }
            value->setValueNotifyingHost(static_cast<float>(std::clamp((plain - info.minValue) / (info.maxValue - info.minValue), 0.0,
                                                                       1.0)));
            return true;
        }
        if (!controller) {
            return false;
        }
        value->setValueNotifyingHost(static_cast<float>(std::clamp(controller->plainParamToNormalized(number, plain), 0.0, 1.0)));
        return true;
    }

    muse::String parameterValueString(const muse::String& id, double normalized) const override
    {
        auto* processor = m_plugin ? m_plugin->getAudioPluginInstance() : nullptr;
        if (processor && processor->getLV2Client()) {
            return tracktionLv2ParameterValueString(*processor, m_lv2Layout, id, normalized);
        }
        auto* controller = vstController();
        bool valid = false;
        const auto number = id.toUInt(&valid);
        Steinberg::Vst::String128 text {};
        if (controller && valid && controller->getParamStringByValue(number, normalized, text) == Steinberg::kResultOk) {
            return muse::String(text);
        }
        const auto* value = findParameter(id);
        if (value && audioUnit()) {
            return muse::String::fromStdString(value->getText(static_cast<float>(normalized), 1024).toStdString());
        }
        return muse::String::number(normalized);
    }

    void beginParameterGesture(const muse::String& id) override
    {
        auto* value = findParameter(id);
        if (value) {
            value->beginChangeGesture();
        }
    }

    void endParameterGesture(const muse::String& id) override
    {
        auto* value = findParameter(id);
        if (value) {
            value->endChangeGesture();
        }
    }

    bool Init() override { return m_error.empty(); }
    std::string GetLastError() const override { return m_error; }
    size_t GetBlockSize() const override { return m_blockSize; }
    size_t SetBlockSize(size_t maximum) override
    {
        m_blockSize = std::min(maximum, m_blockLimit);
        return m_blockSize;
    }

    unsigned GetAudioInCount() const override
    {
        const auto* processor = m_plugin ? m_plugin->getAudioPluginInstance() : nullptr;
        return processor ? processor->getMainBusNumInputChannels() : 0;
    }

    unsigned GetAudioOutCount() const override
    {
        const auto* processor = m_plugin ? m_plugin->getAudioPluginInstance() : nullptr;
        return processor ? processor->getMainBusNumOutputChannels() : 0;
    }

    SampleCount GetLatency(const EffectSettings&, double) const override
    {
        const auto* processor = m_plugin ? m_plugin->getAudioPluginInstance() : nullptr;
        return m_useLatency && processor ? processor->getLatencySamples() : 0;
    }

    bool ProcessInitialize(EffectSettings& settings, double sampleRate, ChannelNames) override
    {
        ProcessFinalize();
        auto* processor = m_plugin ? m_plugin->getAudioPluginInstance() : nullptr;
        if (!processor) {
            return false;
        }
        try {
            processor->setStateInformation(m_defaultState.getData(), static_cast<int>(m_defaultState.getSize()));
            processor->setNonRealtime(true);
            if (!applyAudacityExternalEffectSettings(*m_plugin, mProcessor, settings)) {
                m_error = "Tracktion cannot apply the effect settings.";
                return false;
            }
            m_sampleRate = sampleRate;
            m_position = 0;
            m_initialised = true;
            m_plugin->baseClassInitialise({ tracktion::TimePosition::fromSeconds(mProcessor.mT0), sampleRate,
                                            static_cast<int>(m_blockSize) });
            m_plugin->reset();
            m_audio.setSize(std::max(processor->getTotalNumInputChannels(), processor->getTotalNumOutputChannels()), m_blockSize);
            m_error.clear();
            return true;
        } catch (const std::exception& error) {
            m_error = error.what();
            ProcessFinalize();
            return false;
        }
    }

    bool ProcessFinalize() noexcept override
    {
        if (m_initialised) {
            m_plugin->baseClassDeinitialise();
            m_initialised = false;
        }
        return true;
    }

    size_t ProcessBlock(EffectSettings&, const float* const* input, float* const* output, size_t frames) override
    {
        const int count = static_cast<int>(frames);
        m_audio.clear(0, count);
        for (unsigned channel = 0; channel < GetAudioInCount(); ++channel) {
            m_audio.copyFrom(channel, 0, input[channel], count);
        }
        const auto start = tracktion::TimePosition::fromSeconds(mProcessor.mT0 + m_position / m_sampleRate);
        const auto end = start + tracktion::TimeDuration::fromSamples(count, m_sampleRate);
        tracktion::PluginRenderContext context(&m_audio, 0, count, nullptr, 0.0, { start, end }, false, false, true, false);
        context.referenceSampleRange = { m_position, m_position + count };
        context.referenceSampleRate = m_sampleRate;
        m_plugin->applyToBuffer(context);
        for (unsigned channel = 0; channel < GetAudioOutCount(); ++channel) {
            std::copy_n(m_audio.getReadPointer(channel), count, output[channel]);
        }
        m_position += count;
        return frames;
    }

private:
    std::optional<wxString> serializedSettings(const EffectSettings& settings) const
    {
        CommandParameters parameters;
        if (!mProcessor.SaveSettings(settings, parameters)) {
            return std::nullopt;
        }
        wxString result;
        parameters.GetParameters(result);
        return result;
    }

    void rememberSettings(const EffectSettings& settings)
    {
        m_lastSettings = serializedSettings(settings);
        m_snapshotParameterRevision = m_parameterRevision.load();
        m_snapshotStateRevision = m_stateRevision.load();
    }

    enum ParameterFlag : uint8_t {
        ReadOnlyParameter = 1,
        FullStateParameter = 2
    };

    struct ParameterBinding {
        juce::AudioProcessorParameter* parameter;
        Steinberg::Vst::ParameterInfo info;
        std::optional<effects::ParameterInfo> audioUnitInfo;
    };

    void ensureParameters() const
    {
        auto* processor = m_plugin ? m_plugin->getAudioPluginInstance() : nullptr;
        if (!processor) {
            return;
        }
        const auto& current = processor->getParameters();
        const bool sameParameters = current.size() == static_cast<int>(m_parameterInstances.size())
                                    && std::equal(current.begin(), current.end(), m_parameterInstances.begin());
        if (!m_parameterInfoDirty.exchange(false) && sameParameters) {
            return;
        }
        m_parameterInstances.assign(current.begin(), current.end());
        m_parameters.clear();
        m_parameterOrder.clear();
        auto readOnly = std::make_shared<std::vector<uint8_t> >(processor->getParameters().size(), 1);
        auto* controller = vstController();
        if (controller) {
            std::map<muse::String, Steinberg::Vst::ParameterInfo> metadata;
            for (Steinberg::int32 index = 0; index < controller->getParameterCount(); ++index) {
                Steinberg::Vst::ParameterInfo info {};
                if (controller->getParameterInfo(index, info) == Steinberg::kResultOk) {
                    metadata.emplace(muse::String::number(static_cast<size_t>(info.id)), info);
                }
            }
            for (auto* candidate : processor->getParameters()) {
                const auto* hosted = dynamic_cast<const juce::HostedAudioProcessorParameter*>(candidate);
                const auto id = hosted ? muse::String::fromStdString(hosted->getParameterID().toStdString()) : muse::String {};
                const auto found = metadata.find(id);
                if (found != metadata.end()) {
                    m_parameters.emplace(id, ParameterBinding { candidate, found->second, {} });
                    m_parameterOrder.push_back(id);
                    readOnly->at(candidate->getParameterIndex()) = (found->second.flags & Steinberg::Vst::ParameterInfo::kIsReadOnly) != 0;
                }
            }
        }
#if JUCE_PLUGINHOST_AU
        const auto unit = audioUnit();
        if (unit) {
            for (auto* candidate : processor->getParameters()) {
                const auto* hosted = dynamic_cast<const juce::HostedAudioProcessorParameter*>(candidate);
                if (!hosted) {
                    continue;
                }
                const auto id = hosted->getParameterID();
                AudioUnitParameterInfo raw {};
                UInt32 size = sizeof(raw);
                const auto error = AudioUnitGetProperty(unit, kAudioUnitProperty_ParameterInfo, kAudioUnitScope_Global,
                                                        static_cast<UInt32>(id.getLargeIntValue()), &raw, &size);
                DEFER {
                    if (raw.flags & kAudioUnitParameterFlag_CFNameRelease) {
                        if (raw.cfNameString) {
                            CFRelease(raw.cfNameString);
                        }
                        if (raw.unit == kAudioUnitParameterUnit_CustomUnit && raw.unitName) {
                            CFRelease(raw.unitName);
                        }
                    }
                };
                if (error != noErr) {
                    continue;
                }
                effects::ParameterInfo info;
                info.id = muse::String::fromStdString(id.toStdString());
                info.name = muse::String::fromStdString(candidate->getName(1024).toStdString());
                info.units = muse::String::fromStdString(candidate->getLabel().toStdString());
                info.minValue = raw.minValue;
                info.maxValue = raw.maxValue;
                info.defaultValue = raw.defaultValue;
                info.isReadOnly = !(raw.flags & kAudioUnitParameterFlag_IsWritable);
                info.type = candidate->isBoolean() ? effects::ParameterType::Toggle : effects::ParameterType::Numeric;
                if (candidate->isDiscrete()) {
                    info.stepCount = candidate->getNumSteps() - 1;
                    info.stepSize = info.stepCount > 0 ? (info.maxValue - info.minValue) / info.stepCount : 0.0;
                }
                readOnly->at(candidate->getParameterIndex()) = info.isReadOnly;
                const auto parameterId = info.id;
                m_parameterOrder.push_back(parameterId);
                m_parameters.emplace(parameterId, ParameterBinding { candidate, {}, std::move(info) });
            }
        }
#endif
        if (processor->getLV2Client()) {
            for (auto* candidate : processor->getParameters()) {
                const auto* hosted = dynamic_cast<const juce::HostedAudioProcessorParameter*>(candidate);
                const auto id = hosted ? hosted->getParameterID() : juce::String{};
                readOnly->at(candidate->getParameterIndex()) = FullStateParameter;
                if (id.startsWith("sym:")) {
                    const auto symbol = muse::String::fromStdString(id.substring(4).toStdString());
                    m_parameters.emplace(symbol, ParameterBinding { candidate, {}, {} });
                    m_parameterOrder.push_back(symbol);
                    readOnly->at(candidate->getParameterIndex()) = 0;
                }
            }
        }
        const std::shared_ptr<const std::vector<uint8_t> > flags = std::move(readOnly);
        m_retiredParameterFlags.push_back(flags);
        std::atomic_store(&m_parameterFlags, flags);
        std::erase_if(m_retiredParameterFlags, [](const auto& flags) { return flags.use_count() == 1; });
    }

    juce::AudioProcessorParameter* findParameter(const muse::String& id) const
    {
        ensureParameters();
        const auto found = m_parameters.find(id);
        return found == m_parameters.end() ? nullptr : found->second.parameter;
    }

    void audioProcessorParameterChanged(juce::AudioProcessor*, int index, float) override
    {
        const auto flags = std::atomic_load(&m_parameterFlags);
        const auto parameterFlags = flags && index >= 0 && static_cast<size_t>(index) < flags->size() ? (*flags)[index] : 0;
        if (!(parameterFlags & ReadOnlyParameter) && !m_applyingSettings.load()) {
            m_settingsDirty.store(true);
            m_parameterRevision.fetch_add(1);
            if (parameterFlags & FullStateParameter) {
                m_stateRevision.fetch_add(1);
            }
        }
    }

    void audioProcessorChanged(juce::AudioProcessor*, const ChangeDetails& details) override
    {
        if (details.parameterInfoChanged || details.programChanged) {
            m_parameterInfoDirty.store(true);
        }
        if (!m_applyingSettings.load() && (details.nonParameterStateChanged || details.programChanged)) {
            bool changed = details.nonParameterStateChanged;
            if (audioUnit()) {
                auto* processor = m_plugin->getAudioPluginInstance();
                const juce::ScopedLock lock(processor->getCallbackLock());
                juce::MemoryBlock current;
                processor->getStateInformation(current);
                changed = changed || current != m_cachedState;
                if (!changed) {
                    return;
                }
            }
            m_stateRevision.fetch_add(1);
            if (changed) {
                m_settingsDirty.store(true);
            }
        }
    }

    std::shared_ptr<void> m_runtimeLifetime;
    std::unique_ptr<tracktion::Edit> m_edit;
    tracktion::ExternalPlugin::Ptr m_plugin;
    juce::MemoryBlock m_defaultState;
    juce::MemoryBlock m_cachedState;
    uint64_t m_cachedStateRevision = 0;
    EffectSettings m_lv2Layout;
    bool m_playbackActive = false;
    std::function<void()> m_editorIdleService;
    juce::AudioBuffer<float> m_audio;
    size_t m_blockLimit = 8192;
    size_t m_blockSize = 8192;
    int64_t m_position = 0;
    double m_sampleRate = 44100.0;
    bool m_useLatency = true;
    bool m_initialised = false;
    std::string m_error;
    std::string m_name;
    std::atomic<bool> m_applyingSettings { false };
    std::atomic<bool> m_settingsDirty { false };
    std::atomic<uint64_t> m_stateRevision { 0 };
    std::atomic<uint64_t> m_parameterRevision { 0 };
    uint64_t m_snapshotParameterRevision = 0;
    uint64_t m_snapshotStateRevision = 0;
    std::optional<wxString> m_lastSettings;
    mutable std::atomic<bool> m_parameterInfoDirty { true };
    mutable std::map<muse::String, ParameterBinding> m_parameters;
    mutable std::vector<muse::String> m_parameterOrder;
    mutable std::vector<juce::AudioProcessorParameter*> m_parameterInstances;
    mutable std::shared_ptr<const std::vector<uint8_t> > m_parameterFlags;
    mutable std::vector<std::shared_ptr<const std::vector<uint8_t> > > m_retiredParameterFlags;
};
}

std::shared_ptr<EffectInstance> au::tracktionaudio::createTracktionEffectInstance(TracktionRuntime& runtime,
                                                                                  const EffectInstanceFactory& effect)
{
    const auto* perTrack = dynamic_cast<const PerTrackEffect*>(&effect);
    if (!perTrack || !supportsAudacityExternalEffect(effect)) {
        return nullptr;
    }
    const auto guard = muse::modularity::globalIoc()->resolve<muse::audioplugins::IAudioPluginsLoadGuard>("tracktionaudio");
    const auto id = effects::utils::effectId(&effect).toStdString();
    if (guard && !guard->beginLoad(id)) {
        return nullptr;
    }
    DEFER {
        if (guard) {
            guard->endLoad(id);
        }
    };
    return std::make_shared<TracktionEffectInstance>(runtime, *perTrack);
}
