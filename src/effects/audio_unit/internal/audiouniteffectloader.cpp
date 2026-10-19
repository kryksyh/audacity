/*
 * Audacity: A Digital Audio Editor
 */
#include <tracktion_engine/tracktion_engine.h>

#include "audiouniteffectloader.h"

#include <wx/base64.h>
#include <wx/filefn.h>

#include "au3-audio-unit/AudioUnitEffectBase.h"
#include "au3-module-manager/ConfigInterface.h"
#include "au3wrap/internal/wxtypes_convert.h"
#include "effects/effects_base/internal/effectsutils.h"
#include "tracktionaudio/internal/tracktioneffectinstance.h"
#include "tracktionaudio/internal/tracktionplugindescriptions.h"
#include "tracktionaudio/internal/tracktionpluginloader.h"
#include "tracktionaudio/internal/tracktionruntime.h"
#include "global/defer.h"
#include "global/log.h"

using namespace au::effects;
using au::tracktionaudio::TracktionHostedEffectInstance;
using au::tracktionaudio::TracktionRuntime;

namespace {
constexpr auto stateKey = "AudioUnitState";

class TracktionAudioUnitEffect final : public PerTrackEffect
{
    struct Preset {
        wxString name;
        int number = -1;
    };

public:
    TracktionAudioUnitEffect(EffectMeta meta, std::shared_ptr<TracktionRuntime> runtime,
                             std::unique_ptr<juce::AudioPluginInstance> processor)
        : m_meta(std::move(meta)), m_runtime(std::move(runtime)), m_defaults(EffectSettings::Make<AudioUnitEffectSettings>())
    {
        const auto description = processor->getPluginDescription();
        m_meta.version = muse::String::fromStdString(description.version.toStdString());
        m_meta.type = description.numInputChannels == 0 && description.numOutputChannels == 0 ? au::effects::EffectType::Unknown
                      : description.numInputChannels == 0 ? au::effects::EffectType::Generator
                      : description.numOutputChannels == 0 ? au::effects::EffectType::Analyzer : au::effects::EffectType::Processor;
        const auto unit = processor->getAudioUnitClient()->getAudioUnitHandle();
        AudioUnitWrapper::Parameters parameters;
        AudioUnitUtils::GetVariableSizeProperty(unit, kAudioUnitProperty_ParameterList, parameters);
        auto& defaults = AudioUnitWrapper::GetSettings(m_defaults);
        AudioUnitWrapper::FetchSettings(unit, parameters, defaults, true, false);
        juce::MemoryBlock state;
        processor->getStateInformation(state);
        if (state.getSize() != 0) {
            const auto* bytes = static_cast<const unsigned char*>(state.getData());
            defaults.mState = std::make_shared<const AudioUnitEffectSettings::State>(bytes, bytes + state.getSize());
        }
        m_interactive = !processor->getParameters().isEmpty() || processor->hasEditor();
        if (!m_interactive) {
            UInt32 size = 0;
            Boolean writable = false;
            m_interactive = AudioUnitGetPropertyInfo(unit, kAudioUnitProperty_CocoaUI, kAudioUnitScope_Global, 0, &size, &writable) == noErr
                            && size != 0;
            if (!m_interactive) {
                m_interactive = AudioUnitGetPropertyInfo(unit, kAudioUnitProperty_GetUIComponentList, kAudioUnitScope_Global, 0,
                                                         &size, &writable) == noErr && size != 0;
            }
        }
        m_factoryDefaults = m_defaults;
        auto& factoryDefaults = AudioUnitWrapper::GetSettings(m_factoryDefaults);
        factoryDefaults.mState.reset();
        factoryDefaults.ResetValues();
        if (parameters) {
            for (const auto id : parameters) {
                const AudioUnitWrapper::ParameterInfo info { unit, id };
                m_automation = m_automation || (info.mInfo.flags & kAudioUnitParameterFlag_IsWritable) != 0;
                if (info.mName) {
                    factoryDefaults.values[id].emplace(factoryDefaults.Intern(*info.mName), info.mInfo.defaultValue);
                }
            }
        }
        CFArrayRef presets = nullptr;
        UInt32 size = sizeof(presets);
        if (AudioUnitGetProperty(unit, kAudioUnitProperty_FactoryPresets, kAudioUnitScope_Global, 0, &presets, &size) == noErr && presets) {
            DEFER { CFRelease(presets);
            };
            for (CFIndex index = 0; index < CFArrayGetCount(presets); ++index) {
                const auto* preset = static_cast<const AUPreset*>(CFArrayGetValueAtIndex(presets, index));
                m_presets.push_back({ wxString::FromUTF8(juce::String::fromCFString(
                                                             preset->presetName).toStdString()), preset->presetNumber });
            }
        }
    }

    PluginPath GetPath() const override { return au::au3::wxFromPath(m_meta.path); }
    ComponentInterfaceSymbol GetSymbol() const override { return au::au3::wxFromString(m_meta.title); }
    VendorSymbol GetVendor() const override { return au::au3::wxFromString(m_meta.vendor); }
    wxString GetVersion() const override { return au::au3::wxFromString(m_meta.version); }
    TranslatableString GetDescription() const override { return TranslatableString("audio-unit", "n/a"); }
    EffectFamilySymbol GetFamily() const override { return AUDIOUNITEFFECTS_FAMILY; }
    ::EffectType GetType() const override
    {
        return m_meta.type == au::effects::EffectType::Generator ? EffectTypeGenerate
               : m_meta.type == au::effects::EffectType::Analyzer ? EffectTypeAnalyze
               : m_meta.type == au::effects::EffectType::Processor ? EffectTypeProcess : EffectTypeNone;
    }

    bool IsInteractive() const override { return m_interactive; }
    bool IsDefault() const override { return false; }
    bool SupportsAutomation() const override { return m_automation; }
    bool CanExportPresets() const override { return true; }
    bool HasOptions() const override { return true; }
    RealtimeSince RealtimeSupport() const override
    {
        return GetType() == EffectTypeProcess ? RealtimeSince::After_3_1 : RealtimeSince::Never;
    }

    EffectSettings MakeSettings() const override { return m_defaults; }
    bool CopySettingsContents(const EffectSettings&, EffectSettings&) const override { return true; }
    bool VisitSettings(SettingsVisitor&, EffectSettings&) override { return false; }
    bool VisitSettings(ConstSettingsVisitor&, const EffectSettings&) const override { return false; }

    std::shared_ptr<::EffectInstance> MakeInstance() const override
    {
        return au::tracktionaudio::createTracktionEffectInstance(*m_runtime, *this);
    }

    bool SaveSettings(const EffectSettings& settings, CommandParameters& parameters) const override
    {
        const auto& values = AudioUnitWrapper::GetSettings(settings);
        if (values.mState) {
            parameters.Write(stateKey, wxBase64Encode(values.mState->data(), values.mState->size()));
        }
        if (values.mPresetNumber) {
            parameters.Write(AudioUnitEffectBase::ChoosePresetKey(settings), *values.mPresetNumber);
        }
        for (const auto& [id, value] : values.values) {
            if (value) {
                parameters.Write(value->first, value->second);
            }
        }
        return true;
    }

    bool LoadSettings(const CommandParameters& parameters, EffectSettings& settings) const override
    {
        auto loaded = MakeSettings();
        loaded.extra = settings.extra;
        auto& values = AudioUnitWrapper::GetSettings(loaded);
        values.ResetValues();
        values.mState.reset();
        values.mPresetNumber.reset();
        if (parameters.HasEntry(stateKey)) {
            const auto decoded = wxBase64Decode(parameters.Read(stateKey));
            if (decoded.GetDataLen() == 0) {
                return false;
            }
            const auto* bytes = static_cast<const unsigned char*>(decoded.GetData());
            values.mState = std::make_shared<const AudioUnitEffectSettings::State>(bytes, bytes + decoded.GetDataLen());
        }
        const auto key = AudioUnitEffectBase::FindPresetKey(parameters);
        int number = 0;
        if (!key.empty() && parameters.Read(key, &number)) {
            if (!values.mState) {
                const auto preset = std::find_if(m_presets.begin(), m_presets.end(), [number](const auto& value) {
                    return value.number == number;
                });
                if (preset == m_presets.end() || !LoadFactoryPreset(static_cast<int>(preset - m_presets.begin()), loaded)) {
                    return false;
                }
            } else {
                values.mPresetNumber = number;
            }
        }
        wxString name;
        long index = 0;
        bool entry = parameters.GetFirstEntry(name, index);
        while (entry) {
            const auto id = AudioUnitWrapper::ParameterInfo::ParseKey(name);
            float value = 0.0f;
            if (id && parameters.Read(name, &value)) {
                auto& current = AudioUnitWrapper::GetSettings(loaded);
                current.values[*id].emplace(current.Intern(name), value);
            }
            entry = parameters.GetNextEntry(name, index);
        }
        settings = std::move(loaded);
        return true;
    }

    RegistryPaths GetFactoryPresets() const override
    {
        RegistryPaths names;
        for (const auto& preset : m_presets) {
            names.push_back(preset.name);
        }
        return names;
    }

    OptionalMessage LoadFactoryPreset(int index, EffectSettings& settings) const override
    {
        if (index < 0 || static_cast<size_t>(index) >= m_presets.size()) {
            return {};
        }
        const auto instance = std::dynamic_pointer_cast<TracktionHostedEffectInstance>(MakeInstance());
        if (!instance || !instance->Init()) {
            return {};
        }
        const auto unit = instance->audioUnit();
        CFArrayRef presets = nullptr;
        UInt32 size = sizeof(presets);
        if (AudioUnitGetProperty(unit, kAudioUnitProperty_FactoryPresets, kAudioUnitScope_Global, 0, &presets,
                                 &size) != noErr || !presets) {
            return {};
        }
        DEFER { CFRelease(presets);
        };
        for (CFIndex item = 0; item < CFArrayGetCount(presets); ++item) {
            const auto* preset = static_cast<const AUPreset*>(CFArrayGetValueAtIndex(presets, item));
            if (preset->presetNumber == m_presets[index].number) {
                if (AudioUnitSetProperty(unit, kAudioUnitProperty_PresentPreset, kAudioUnitScope_Global, 0, preset,
                                         sizeof(*preset)) != noErr) {
                    return {};
                }
                auto loaded = MakeSettings();
                loaded.extra = settings.extra;
                if (!instance->saveSettings(loaded)) {
                    return {};
                }
                settings = std::move(loaded);
                return { nullptr };
            }
        }
        return {};
    }

    OptionalMessage LoadFactoryDefaults(EffectSettings& settings) const override
    {
        if (auto message = LoadUserPreset(FactoryDefaultsGroup(), settings)) {
            return message;
        }
        auto defaults = m_factoryDefaults;
        defaults.extra = settings.extra;
        settings = std::move(defaults);
        return { nullptr };
    }

    OptionalMessage LoadUserPreset(const RegistryPath& group, EffectSettings& settings) const override
    {
        wxString parameters;
        if (GetConfig(*this, PluginSettings::Private, group, wxT("Parameters"), parameters, wxEmptyString) && !parameters.empty()) {
            if (auto message = LoadSettingsFromString(parameters, settings)) {
                return message;
            }
        }
        wxString encoded;
        if (!GetConfig(*this, PluginSettings::Private, group, wxT("Data"), encoded, wxEmptyString)) {
            return {};
        }
        CommandParameters stored;
        stored.Write(stateKey, encoded);
        auto loaded = MakeSettings();
        loaded.extra = settings.extra;
        if (!LoadSettings(stored, loaded)) {
            return {};
        }
        const auto instance = std::dynamic_pointer_cast<TracktionHostedEffectInstance>(MakeInstance());
        if (!instance || !instance->Init() || !instance->loadSettings(loaded, true) || !instance->saveSettings(loaded)) {
            return {};
        }
        settings = std::move(loaded);
        return { nullptr };
    }

    bool SaveUserPreset(const RegistryPath& group, const EffectSettings& settings) const override
    {
        const auto instance = std::dynamic_pointer_cast<TracktionHostedEffectInstance>(MakeInstance());
        if (!instance || !instance->Init() || !instance->loadSettings(settings, true)) {
            return false;
        }
        auto stored = MakeSettings();
        if (!instance->saveSettings(stored)) {
            return false;
        }
        wxString parameters;
        if (!SaveSettingsAsString(stored, parameters)) {
            return false;
        }
        const auto name = juce::String(wxFileNameFromPath(group).ToStdString()).toCFString();
        DEFER { CFRelease(name);
        };
        const AUPreset preset { -1, name };
        if (AudioUnitSetProperty(instance->audioUnit(), kAudioUnitProperty_PresentPreset, kAudioUnitScope_Global, 0,
                                 &preset, sizeof(preset)) != noErr || !instance->saveSettings(stored)) {
            return false;
        }
        const auto& state = AudioUnitWrapper::GetSettings(stored).mState;
        if (!state) {
            return false;
        }
        const auto encoded = wxBase64Encode(state->data(), state->size());
        const bool saved = SetConfig(*this, PluginSettings::Private, group, wxT("Parameters"), parameters);
        return SetConfig(*this, PluginSettings::Private, group, wxT("Data"), encoded) || saved;
    }

private:
    EffectMeta m_meta;
    std::shared_ptr<TracktionRuntime> m_runtime;
    EffectSettings m_defaults;
    EffectSettings m_factoryDefaults;
    std::vector<Preset> m_presets;
    bool m_interactive = false;
    bool m_automation = false;
};
}

AudioUnitEffectLoader::AudioUnitEffectLoader(std::shared_ptr<TracktionRuntime> runtime)
    : m_runtime(std::move(runtime)) {}

AudioUnitEffectLoader::~AudioUnitEffectLoader() = default;
void AudioUnitEffectLoader::deinit() { m_runtime.reset(); }
EffectFamily AudioUnitEffectLoader::family() const { return EffectFamily::AudioUnit; }

bool AudioUnitEffectLoader::ensurePluginIsLoaded(const EffectId& effectId)
{
    if (m_effects.contains(effectId)) {
        return true;
    }
    const auto plugins = knownPlugins()->pluginInfoList();
    const auto found
        = std::find_if(plugins.begin(), plugins.end(), [&](const auto& info) { return info.meta.id == effectId.toStdString(); });
    if (found == plugins.end() || found->state != muse::audioplugins::AudioPluginState::Validated
        || !utils::isFamilyType(found->meta, family())) {
        return false;
    }
    const auto fields = juce::StringArray::fromTokens(juce::String(found->path.toStdString()), "/", "");
    if (fields.size() < 4 || fields[0].length() != 4 || fields[1].length() != 4 || fields[2].length() != 4) {
        return false;
    }
    const auto guard = loadGuard().get();
    if (guard && !guard->beginLoad(effectId.toStdString())) {
        return false;
    }
    DEFER { if (guard) {
                guard->endLoad(effectId.toStdString());
            }
    };
    try {
        if (!m_runtime) {
            auto runtime = std::make_shared<TracktionRuntime>();
            runtime->init((globalConfiguration()->userAppDataPath() + "/tracktion/plugins/cache").toStdString(),
                          (globalConfiguration()->appConfigPath() + "/tracktion/plugins").toStdString());
            m_runtime = std::move(runtime);
        }
        au::tracktionaudio::restoreTracktionPluginDescriptions(m_runtime->engine(), plugins);
        auto& manager = m_runtime->engine().getPluginManager();
        for (auto* format : manager.pluginFormatManager.getFormats()) {
            if (format->getName() != "AudioUnit") {
                continue;
            }
            const auto identifier = "AudioUnit:" + fields[1] + "," + fields[2] + "," + fields[0];
            const auto description = au::tracktionaudio::findTracktionPluginDescription(manager, *format, identifier, {});
            if (!description) {
                return false;
            }
            juce::String error;
            auto processor = au::tracktionaudio::loadTracktionPlugin(manager.pluginFormatManager, *description, error);
            if (!processor || !processor->getAudioUnitClient()) {
                LOGE() << error.toStdString();
                return false;
            }
            auto meta = utils::museToAuEffectMeta(found->path, found->meta, found->state);
            m_effects.emplace(effectId, std::make_unique<TracktionAudioUnitEffect>(std::move(meta), m_runtime, std::move(processor)));
            return true;
        }
    } catch (const std::exception& error) {
        LOGE() << error.what();
    }
    return false;
}

Effect* AudioUnitEffectLoader::effect(const EffectId& effectId) const
{
    const auto found = m_effects.find(effectId);
    return found == m_effects.end() ? nullptr : found->second.get();
}
