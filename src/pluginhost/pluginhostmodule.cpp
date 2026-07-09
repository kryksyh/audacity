/*
* Audacity: A Digital Audio Editor
*/
#include "pluginhostmodule.h"

#include "plugin.h"
#include "internal/pluginhostservice.h"

using namespace au::pluginhost;

std::string PluginHostModule::moduleName() const
{
    return "pluginhost";
}

void PluginHostModule::registerExports()
{
    muse::modularity::globalIoc()->registerExport<IPluginHostService>(moduleName(),
                                                                      std::make_shared<PluginHostService>());
}
