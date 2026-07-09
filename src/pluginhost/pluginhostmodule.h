/*
* Audacity: A Digital Audio Editor
*/
#pragma once

#include "framework/global/modularity/imodulesetup.h"

namespace au::pluginhost {
//! Hosts dynamically loaded plugins: discovers and loads them, and exposes
//! them to the capability-specific hosts (e.g. effects) through
//! IPluginHostService.
class PluginHostModule : public muse::modularity::IModuleSetup
{
public:
    std::string moduleName() const override;
    void registerExports() override;
};
}
