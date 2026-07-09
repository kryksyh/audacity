/*
* Audacity: A Digital Audio Editor
*/
#pragma once

#include "modularity/imodulesetup.h"

namespace au::importexport {
//! Registers import/export formats provided by stable-ABI plugins in the
//! au3 importer/exporter registries. Runs in resolveImports so the formats
//! are in place before the registries take their one-time snapshot.
class PluginFormatsModule : public muse::modularity::IModuleSetup
{
public:
    std::string moduleName() const override;
    void resolveImports() override;
};
}
