/*
* Audacity: A Digital Audio Editor
*/
#pragma once

#include <memory>
#include <vector>

#include "framework/global/modularity/imodulesetup.h"

namespace au::effects {
class Au3AudioPluginScanner;
class PluginEffectViewsService;
class PluginEffectsMetaReader;
class PluginEffectsLoader;
class PluginEffectsProvider;

//! Binds dynamically loaded plugins (served by the generic plugin host)
//! into the effects infrastructure as the Plugin family.
class PluginEffectsModule : public muse::modularity::IModuleSetup
{
public:
    PluginEffectsModule();
    ~PluginEffectsModule() override;

    std::string moduleName() const override;
    void registerExports() override;
    void resolveImports() override;
    void onInit(const muse::IApplication::RunMode& mode) override;
    void onDeinit() override;

    muse::modularity::IContextSetup* newContext(const muse::modularity::ContextPtr& ctx) const override;

private:
    std::vector<std::unique_ptr<PluginEffectsProvider> > m_providers;
    std::vector<std::shared_ptr<Au3AudioPluginScanner> > m_scanners;
    std::vector<std::shared_ptr<PluginEffectsMetaReader> > m_metaReaders;
    std::shared_ptr<PluginEffectsLoader> m_effectsLoader;
    std::shared_ptr<PluginEffectViewsService> m_viewsService;
};

class PluginEffectsContext : public muse::modularity::IContextSetup
{
public:
    PluginEffectsContext(const muse::modularity::ContextPtr& ctx);

    void resolveImports() override;
};
}
