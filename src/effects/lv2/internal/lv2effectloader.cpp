/*
 * Audacity: A Digital Audio Editor
 */
#include <tracktion_engine/tracktion_engine.h>

#include "lv2effectloader.h"

#include "au3-effects/PerTrackEffect.h"
#include "au3-module-manager/ConfigInterface.h"
#include "au3wrap/internal/wxtypes_convert.h"
#include "effects/effects_base/internal/effectsutils.h"
#include "tracktionaudio/internal/tracktioneffectinstance.h"
#include "tracktionaudio/internal/tracktionlv2settings.h"
#include "tracktionaudio/internal/tracktionlv2ui.h"
#include "tracktionaudio/internal/tracktionplugindescriptions.h"
#include "tracktionaudio/internal/tracktionpluginloader.h"
#include "tracktionaudio/internal/tracktionruntime.h"
#include "global/defer.h"
#include "global/log.h"

using namespace au::effects;
using namespace au::tracktionaudio;

namespace {
class TracktionLv2Effect final : public PerTrackEffect
{
public:
    TracktionLv2Effect(EffectMeta meta, std::shared_ptr<TracktionRuntime> runtime, std::unique_ptr<juce::AudioPluginInstance> processor)
        : m_meta(std::move(meta)), m_runtime(std::move(runtime)), m_defaults(makeTracktionLv2Settings(*processor))
    {
        const auto description = processor->getPluginDescription();
        m_meta.type = description.numInputChannels == 0 && description.numOutputChannels == 0 ? au::effects::EffectType::Tool
                      : description.numInputChannels == 0 ? au::effects::EffectType::Generator
                      : description.numOutputChannels == 0 ? au::effects::EffectType::Analyzer : au::effects::EffectType::Processor;
        m_interactive = !processor->getLV2Client()->getControlPorts().empty();
        m_vendorUi = TracktionLv2Ui::isSupported(*processor);
        for (int index = 0; index < processor->getNumPrograms(); ++index) {
            m_presets.push_back(wxString::FromUTF8(processor->getProgramName(index).toStdString()));
        }
    }

    PluginPath GetPath() const override { return au::au3::wxFromPath(m_meta.path); }
    ComponentInterfaceSymbol GetSymbol() const override { return au::au3::wxFromString(m_meta.title); }
    VendorSymbol GetVendor() const override { return au::au3::wxFromString(m_meta.vendor); }
    wxString GetVersion() const override { return wxT("1.0"); }
    TranslatableString GetDescription() const override { return XO("n/a"); }
    EffectFamilySymbol GetFamily() const override { return wxT("LV2"); }
    ::EffectType GetType() const override
    {
        return m_meta.type == au::effects::EffectType::Generator ? EffectTypeGenerate
               : m_meta.type == au::effects::EffectType::Analyzer ? EffectTypeAnalyze
               : m_meta.type == au::effects::EffectType::Processor ? EffectTypeProcess : EffectTypeTool;
    }

    bool IsInteractive() const override { return m_interactive; }
    bool vendorUiSupported() const { return m_vendorUi; }
    bool IsDefault() const override { return false; }
    bool SupportsAutomation() const override { return true; }
    bool CanExportPresets() const override { return false; }
    bool HasOptions() const override { return true; }
    RealtimeSince RealtimeSupport() const override
    {
        return GetType() == EffectTypeProcess ? RealtimeSince::Always : RealtimeSince::Never;
    }

    EffectSettings MakeSettings() const override { return m_defaults; }
    bool CopySettingsContents(const EffectSettings& source, EffectSettings& destination) const override
    {
        const auto* src = source.cast<TracktionLv2Settings>();
        auto* dst = destination.cast<TracktionLv2Settings>();
        if (!src || !dst || src->values.size() != dst->values.size()) {
            return false;
        }
        std::copy(src->values.begin(), src->values.end(), dst->values.begin());
        dst->state = src->state;
        return true;
    }

    bool VisitSettings(SettingsVisitor&, EffectSettings&) override { return false; }
    bool VisitSettings(ConstSettingsVisitor&, const EffectSettings&) const override { return false; }

    std::shared_ptr<::EffectInstance> MakeInstance() const override
    {
        return createTracktionEffectInstance(*m_runtime, *this);
    }

    bool SaveSettings(const EffectSettings& settings, CommandParameters& parameters) const override
    {
        return saveTracktionLv2Settings(settings, parameters);
    }

    bool LoadSettings(const CommandParameters& parameters, EffectSettings& settings) const override
    {
        auto updated = settings;
        if (!loadTracktionLv2Settings(parameters, updated)) {
            return false;
        }
        const auto instance = std::dynamic_pointer_cast<TracktionHostedEffectInstance>(MakeInstance());
        if (!instance || !instance->Init() || !instance->loadSettings(updated, false)) {
            return false;
        }
        settings = std::move(updated);
        return true;
    }

    RegistryPaths GetFactoryPresets() const override { return m_presets; }
    OptionalMessage LoadFactoryPreset(int index, EffectSettings& settings) const override
    {
        if (index < 0 || static_cast<size_t>(index) >= m_presets.size()) {
            return {};
        }
        const auto instance = std::dynamic_pointer_cast<TracktionHostedEffectInstance>(MakeInstance());
        if (!instance || !instance->Init() || !instance->loadSettings(settings, false)) {
            return {};
        }
        auto* processor = instance->tracktionPlugin().getAudioPluginInstance();
        auto* client = processor ? processor->getLV2Client() : nullptr;
        auto updated = settings;
        if (!client || !client->loadFactoryPreset(index) || !instance->saveSettings(updated)) {
            return {};
        }
        settings = std::move(updated);
        return { nullptr };
    }

    OptionalMessage LoadFactoryDefaults(EffectSettings& settings) const override
    {
        if (auto message = LoadUserPreset(FactoryDefaultsGroup(), settings)) {
            return message;
        }
        auto defaults = MakeSettings();
        defaults.extra = settings.extra;
        settings = std::move(defaults);
        return { nullptr };
    }

    OptionalMessage LoadUserPreset(const RegistryPath& group, EffectSettings& settings) const override
    {
        wxString encoded;
        CommandParameters parameters;
        if (!GetConfig(*this, PluginSettings::Private, group, wxT("Parameters"), encoded, wxEmptyString)
            || !parameters.SetParameters(encoded) || !LoadSettings(parameters, settings)) {
            return {};
        }
        return { nullptr };
    }

    bool SaveUserPreset(const RegistryPath& group, const EffectSettings& settings) const override
    {
        CommandParameters parameters;
        wxString encoded;
        return SaveSettings(settings, parameters) && parameters.GetParameters(encoded)
               && SetConfig(*this, PluginSettings::Private, group, wxT("Parameters"), encoded);
    }

private:
    EffectMeta m_meta;
    std::shared_ptr<TracktionRuntime> m_runtime;
    EffectSettings m_defaults;
    RegistryPaths m_presets;
    bool m_interactive = false;
    bool m_vendorUi = false;
};
}

bool au::effects::tracktionLv2VendorUiSupported(const EffectInstanceFactory& effect)
{
    const auto* native = dynamic_cast<const TracktionLv2Effect*>(&effect);
    return native && native->vendorUiSupported();
}

Lv2EffectLoader::Lv2EffectLoader(std::shared_ptr<TracktionRuntime> runtime)
    : m_runtime(std::move(runtime)) {}

Lv2EffectLoader::~Lv2EffectLoader() = default;
void Lv2EffectLoader::deinit() { m_runtime.reset(); }
EffectFamily Lv2EffectLoader::family() const { return EffectFamily::LV2; }

bool Lv2EffectLoader::ensurePluginIsLoaded(const EffectId& effectId)
{
    if (m_effects.contains(effectId)) {
        return true;
    }
    const auto plugins = knownPlugins()->pluginInfoList();
    const auto found
        = std::find_if(plugins.begin(), plugins.end(), [&](const auto& info) { return info.meta.id == effectId.toStdString(); });
    if (found == plugins.end() || found->state != muse::audioplugins::AudioPluginState::Validated
        || !utils::isFamilyType(found->meta, family()) || !Lv2PluginIdentifier::parse(found->path.toStdString())) {
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
        restoreTracktionPluginDescriptions(m_runtime->engine(), plugins);
        auto& manager = m_runtime->engine().getPluginManager();
        for (auto* format : manager.pluginFormatManager.getFormats()) {
            if (format->getName() != "LV2") {
                continue;
            }
            const auto description = findTracktionPluginDescription(manager, *format, found->path.toStdString(), {});
            if (!description) {
                return false;
            }
            juce::String error;
            auto processor = loadTracktionPlugin(manager.pluginFormatManager, *description, error);
            if (!processor || !processor->getLV2Client()) {
                LOGE() << error.toStdString();
                return false;
            }
            auto meta = utils::museToAuEffectMeta(found->path, found->meta, found->state);
            m_effects.emplace(effectId, std::make_unique<TracktionLv2Effect>(std::move(meta), m_runtime, std::move(processor)));
            return true;
        }
    } catch (const std::exception& error) {
        LOGE() << error.what();
    }
    return false;
}

Effect* Lv2EffectLoader::effect(const EffectId& effectId) const
{
    const auto found = m_effects.find(effectId);
    return found == m_effects.end() ? nullptr : found->second.get();
}
