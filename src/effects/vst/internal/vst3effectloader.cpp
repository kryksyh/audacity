/*
* Audacity: A Digital Audio Editor
*/
#include <tracktion_engine/tracktion_engine.h>

#include "vst3effectloader.h"

#include <QDirIterator>
#include <QRegularExpression>

#include "au3-vst3/VST3Wrapper.h"
#include "au3wrap/internal/wxtypes_convert.h"
#include "effects/effects_base/internal/effectsutils.h"
#include "tracktionaudio/internal/tracktioneffectinstance.h"
#include "tracktionaudio/internal/tracktionplugindescriptions.h"
#include "tracktionaudio/internal/tracktionruntime.h"
#include "global/defer.h"
#include "global/log.h"

using namespace au::effects;
using au::tracktionaudio::TracktionHostedEffectInstance;
using au::tracktionaudio::TracktionRuntime;

namespace {
class TracktionVst3Effect final : public PerTrackEffect
{
    struct Preset {
        wxString name;
        juce::File file;
        int program = -1;
    };

public:
    TracktionVst3Effect(EffectMeta meta, juce::PluginDescription description, std::shared_ptr<TracktionRuntime> runtime)
        : m_meta(std::move(meta)), m_description(std::move(description)), m_runtime(std::move(runtime)) {}

    PluginPath GetPath() const override { return au::au3::wxFromPath(m_meta.path); }
    ComponentInterfaceSymbol GetSymbol() const override { return au::au3::wxFromString(m_meta.title); }
    VendorSymbol GetVendor() const override { return au::au3::wxFromString(m_meta.vendor); }
    wxString GetVersion() const override { return au::au3::wxFromString(m_meta.version); }
    TranslatableString GetDescription() const override
    {
        return TranslatableString("vst3", "SubCategories: %1").arg(wxString::FromUTF8(m_description.category.toStdString()));
    }

    ::EffectType GetType() const override
    {
        return m_meta.type == au::effects::EffectType::Generator ? EffectTypeGenerate
               : m_meta.type == au::effects::EffectType::Processor ? EffectTypeProcess : EffectTypeNone;
    }

    EffectFamilySymbol GetFamily() const override { return TranslatableString("vst3", "VST3"); }
    bool IsInteractive() const override { return true; }
    bool IsDefault() const override { return false; }
    RealtimeSince RealtimeSupport() const override
    {
        return GetType() == EffectTypeProcess ? RealtimeSince::After_3_1 : RealtimeSince::Never;
    }

    bool SupportsAutomation() const override { return true; }
    bool CanExportPresets() const override { return true; }
    bool HasOptions() const override { return true; }

    EffectSettings MakeSettings() const override { return VST3Wrapper::MakeSettings(); }
    bool SaveSettings(const EffectSettings& settings, CommandParameters& parameters) const override
    {
        VST3Wrapper::SaveSettings(settings, parameters);
        return true;
    }

    bool LoadSettings(const CommandParameters& parameters, EffectSettings& settings) const override
    {
        VST3Wrapper::LoadSettings(parameters, settings);
        return true;
    }

    bool CopySettingsContents(const EffectSettings& source, EffectSettings& destination) const override
    {
        VST3Wrapper::CopySettingsContents(source, destination);
        return true;
    }

    OptionalMessage LoadUserPreset(const RegistryPath& name, EffectSettings& settings) const override
    {
        return VST3Wrapper::LoadUserPreset(*this, name, settings);
    }

    bool SaveUserPreset(const RegistryPath& name, const EffectSettings& settings) const override
    {
        VST3Wrapper::SaveUserPreset(*this, name, settings);
        return true;
    }

    std::shared_ptr<::EffectInstance> MakeInstance() const override
    {
        return au::tracktionaudio::createTracktionEffectInstance(*m_runtime, *this);
    }

    RegistryPaths GetFactoryPresets() const override
    {
        if (!m_presets) {
            const auto instance = std::dynamic_pointer_cast<TracktionHostedEffectInstance>(MakeInstance());
            if (!instance || !instance->Init()) {
                return {};
            }
            std::vector<Preset> presets;
#if JUCE_MAC
            QString root = "/Library/Audio/Presets";
#elif JUCE_WINDOWS
            QString root = QString::fromStdString(juce::File::getSpecialLocation(juce::File::commonApplicationDataDirectory)
                                                  .getChildFile("VST3 Presets").getFullPathName().toStdString());
#else
            QString root = "/usr/local/share/vst3/presets";
#endif
            const QRegularExpression invalid("[\\\\*?/:<>|]");
            root += "/" + QString::fromStdString(au::au3::wxFromString(m_meta.vendor).Trim().ToStdString()).replace(invalid, "_")
                    + "/" + QString::fromStdString(au::au3::wxFromString(m_meta.title).Trim().ToStdString()).replace(invalid, "_");
            QDirIterator files(root, QDir::Files, QDirIterator::Subdirectories);
            while (files.hasNext()) {
                const juce::File file(files.next().toStdString());
                presets.push_back({ wxString::FromUTF8(file.getFileNameWithoutExtension().toStdString()), file, -1 });
            }
            auto& plugin = instance->tracktionPlugin();
            for (int index = 0; index < plugin.getNumPrograms(); ++index) {
                const auto name = plugin.getProgramName(index);
                if (name.isNotEmpty() || plugin.getNumPrograms() > 1) {
                    presets.push_back({ name.isNotEmpty() ? wxString::FromUTF8(name.toStdString())
                                        : wxString::Format("Unit %03d - %04d", 0, index), {}, index });
                }
            }
            m_presets = std::move(presets);
        }
        RegistryPaths names;
        for (const auto& preset : *m_presets) {
            names.push_back(preset.name);
        }
        return names;
    }

    OptionalMessage LoadFactoryPreset(int index, EffectSettings& settings) const override
    {
        GetFactoryPresets();
        if (!m_presets || index < 0 || static_cast<size_t>(index) >= m_presets->size()) {
            return {};
        }
        const auto instance = std::dynamic_pointer_cast<TracktionHostedEffectInstance>(MakeInstance());
        auto initial = MakeSettings();
        if (!instance || !instance->Init()) {
            return {};
        }
        const auto frames = instance->SetBlockSize(512);
        if (!instance->ProcessInitialize(initial, 44100.0, nullptr)) {
            return {};
        }
        DEFER { instance->ProcessFinalize();
        };
        const auto& preset = (*m_presets)[index];
        if (preset.program >= 0) {
            instance->tracktionPlugin().setCurrentProgram(preset.program, false);
        } else {
            if (!instance->loadVst3PresetFile(preset.file.getFullPathName().toStdString())) {
                return {};
            }
        }
        juce::AudioBuffer<float> silence(std::max(1u, std::max(instance->GetAudioInCount(), instance->GetAudioOutCount())), frames);
        silence.clear();
        if (instance->ProcessBlock(initial, silence.getArrayOfReadPointers(), silence.getArrayOfWritePointers(), frames) != frames
            || !instance->saveSettings(settings)) {
            return {};
        }
        return { nullptr };
    }

private:
    EffectMeta m_meta;
    juce::PluginDescription m_description;
    std::shared_ptr<TracktionRuntime> m_runtime;
    mutable std::optional<std::vector<Preset> > m_presets;
};
}

Vst3EffectLoader::Vst3EffectLoader(std::shared_ptr<TracktionRuntime> runtime)
    : m_runtime(std::move(runtime)) {}

Vst3EffectLoader::~Vst3EffectLoader() = default;

void Vst3EffectLoader::deinit()
{
    m_runtime.reset();
}

EffectFamily Vst3EffectLoader::family() const { return EffectFamily::VST3; }

bool Vst3EffectLoader::ensurePluginIsLoaded(const EffectId& effectId)
{
    if (m_effects.contains(effectId)) {
        return true;
    }
    const auto plugins = knownPlugins()->pluginInfoList();
    const auto found = std::find_if(plugins.begin(), plugins.end(), [&](const auto& info) {
        return info.meta.id == effectId.toStdString();
    });
    if (found == plugins.end() || found->state != muse::audioplugins::AudioPluginState::Validated
        || !utils::isFamilyType(found->meta, family())) {
        return false;
    }
    const juce::String path(utils::parseEffectPath(effectId));
    const auto classID = path.fromLastOccurrenceOf(";", false, false);
    if (classID.length() != 32 || !classID.containsOnly("0123456789abcdefABCDEF")) {
        return false;
    }
    const auto guard = loadGuard().get();
    if (guard && !guard->beginLoad(effectId.toStdString())) {
        return false;
    }
    DEFER {
        if (guard) {
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
            if (format->getName() != "VST3") {
                continue;
            }
            const auto description
                = au::tracktionaudio::findTracktionPluginDescription(manager, *format, found->path.toStdString(), classID);
            if (!description) {
                return false;
            }
            auto meta = utils::museToAuEffectMeta(found->path, found->meta, found->state);
            meta.path = found->path.toStdString() + ";" + description->uniqueIdString.toStdString();
            m_effects.emplace(effectId, std::make_unique<TracktionVst3Effect>(std::move(meta), *description, m_runtime));
            return true;
        }
    } catch (const std::exception& error) {
        LOGE() << error.what();
    }
    return false;
}

Effect* Vst3EffectLoader::effect(const EffectId& effectId) const
{
    const auto found = m_effects.find(effectId);
    return found == m_effects.end() ? nullptr : found->second.get();
}
