/*
* Audacity: A Digital Audio Editor
*/
#pragma once

#include "au3-components/PluginProvider.h"

#include "pluginhost/api/audacity_plugin.h"

namespace au::pluginhost {
class Plugin;
}

namespace au::effects {

//! Host-side plugin provider over a stable-ABI plugin's effect factory.
//! Lets ABI effects flow through the same registration, scanning and
//! loading machinery as every other plugin effect.
class PluginEffectsProvider final : public PluginProvider
{
public:
    explicit PluginEffectsProvider(pluginhost::Plugin& plugin);
    ~PluginEffectsProvider() override = default;

    // ComponentInterface implementation
    PluginPath GetPath() const override;
    ComponentInterfaceSymbol GetSymbol() const override;
    VendorSymbol GetVendor() const override;
    wxString GetVersion() const override;
    TranslatableString GetDescription() const override;

    // PluginProvider implementation
    bool Initialize() override;
    void Terminate() override;
    EffectFamilySymbol GetOptionalFamilySymbol() override;
    const FileExtensions& GetFileExtensions() override;
    FilePath InstallPath() override;
    void AutoRegisterPlugins(PluginManagerInterface&) override;
    PluginPaths FindModulePaths(PluginManagerInterface&, BasicUI::ProgressDialog*) const override;
    unsigned DiscoverPluginsAtPath(const PluginPath& path, TranslatableString& errMsg,
                                   const RegistrationCallback& callback) override;
    bool CheckPluginExist(const PluginPath& path) const override;
    std::unique_ptr<ComponentInterface> LoadPlugin(const PluginPath& path) override;

private:
    const auplug_effect_desc_t* descriptorForPath(const PluginPath& path) const;

    pluginhost::Plugin& m_module;
};
}
