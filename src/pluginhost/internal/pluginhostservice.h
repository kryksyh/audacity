/*
* Audacity: A Digital Audio Editor
*/
#pragma once

#include <memory>

#include "framework/global/iglobalconfiguration.h"
#include "framework/global/modularity/ioc.h"

#include "pluginhost/ipluginhostservice.h"

namespace au::pluginhost {
//! Scans the plugin directories once, on first access; loaded plugins are
//! kept alive for the lifetime of the application.
class PluginHostService : public IPluginHostService
{
    muse::GlobalInject<muse::IGlobalConfiguration> globalConfiguration;

public:
    PluginHostService() = default;
    ~PluginHostService() override;

    const std::vector<Plugin*>& plugins() const override;
    const std::vector<PluginView>& views() const override;

private:
    void ensureLoaded() const;
    void loadModules();

    bool m_loaded = false;
    std::vector<std::unique_ptr<Plugin> > m_plugins;
    std::vector<Plugin*> m_pluginPtrs;
    std::vector<PluginView> m_views;
};
}
