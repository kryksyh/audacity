/*
* Audacity: A Digital Audio Editor
*/
#include "plugineffectsprovider.h"

#include "au3-strings/TranslatableString.h"
#include "au3-strings/wxArrayStringEx.h"

#include "plugineffectadapter.h"
#include "pluginhost/plugin.h"

using namespace au::effects;

PluginEffectsProvider::PluginEffectsProvider(pluginhost::Plugin& plugin)
    : m_module(plugin)
{
}

PluginPath PluginEffectsProvider::GetPath() const
{
    return {};
}

ComponentInterfaceSymbol PluginEffectsProvider::GetSymbol() const
{
    return ComponentInterfaceSymbol { wxString::FromUTF8(m_module.desc().id),
                                      Verbatim(wxString::FromUTF8(m_module.desc().name)) };
}

VendorSymbol PluginEffectsProvider::GetVendor() const
{
    return VendorSymbol { Verbatim(wxString::FromUTF8(m_module.desc().vendor)) };
}

wxString PluginEffectsProvider::GetVersion() const
{
    return wxString::FromUTF8(m_module.desc().version);
}

TranslatableString PluginEffectsProvider::GetDescription() const
{
    return Verbatim(wxString::FromUTF8(m_module.desc().description ? m_module.desc().description : ""));
}

bool PluginEffectsProvider::Initialize()
{
    return m_module.effectFactory() != nullptr;
}

void PluginEffectsProvider::Terminate()
{
}

EffectFamilySymbol PluginEffectsProvider::GetOptionalFamilySymbol()
{
    return {};
}

const FileExtensions& PluginEffectsProvider::GetFileExtensions()
{
    static const FileExtensions empty;
    return empty;
}

FilePath PluginEffectsProvider::InstallPath()
{
    return {};
}

void PluginEffectsProvider::AutoRegisterPlugins(PluginManagerInterface&)
{
}

PluginPaths PluginEffectsProvider::FindModulePaths(PluginManagerInterface&, BasicUI::ProgressDialog*) const
{
    PluginPaths paths;
    const auplug_effect_factory_t* factory = m_module.effectFactory();
    if (!factory) {
        return paths;
    }
    const uint32_t count = factory->count();
    for (uint32_t i = 0; i < count; ++i) {
        if (const auplug_effect_desc_t* desc = factory->get_descriptor(i)) {
            paths.push_back(PluginEffectAdapter::pathForId(desc->id));
        }
    }
    return paths;
}

const auplug_effect_desc_t* PluginEffectsProvider::descriptorForPath(const PluginPath& path) const
{
    const auplug_effect_factory_t* factory = m_module.effectFactory();
    if (!factory) {
        return nullptr;
    }
    const uint32_t count = factory->count();
    for (uint32_t i = 0; i < count; ++i) {
        const auplug_effect_desc_t* desc = factory->get_descriptor(i);
        if (desc && PluginEffectAdapter::pathForId(desc->id) == path) {
            return desc;
        }
    }
    return nullptr;
}

unsigned PluginEffectsProvider::DiscoverPluginsAtPath(const PluginPath& path, TranslatableString& errMsg,
                                                  const RegistrationCallback& callback)
{
    errMsg = {};
    const auplug_effect_desc_t* desc = descriptorForPath(path);
    if (!desc) {
        errMsg = Verbatim(wxT("Unknown ABI plugin path"));
        return 0;
    }

    PluginEffectAdapter effect(*m_module.effectFactory(), *desc);
    if (!effect.isValid()) {
        errMsg = Verbatim(wxT("The plugin failed to create the effect"));
        return 0;
    }
    if (callback) {
        callback(this, &effect);
    }
    return 1;
}

bool PluginEffectsProvider::CheckPluginExist(const PluginPath& path) const
{
    return descriptorForPath(path) != nullptr;
}

std::unique_ptr<ComponentInterface> PluginEffectsProvider::LoadPlugin(const PluginPath& path)
{
    const auplug_effect_desc_t* desc = descriptorForPath(path);
    if (!desc) {
        return nullptr;
    }
    auto effect = std::make_unique<PluginEffectAdapter>(*m_module.effectFactory(), *desc);
    return effect->isValid() ? std::move(effect) : nullptr;
}
