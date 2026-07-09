/*
* Audacity: A Digital Audio Editor
*/
#include "pluginformatsmodule.h"

#include "modularity/ioc.h"

#include "au3-import-export/ExportPluginRegistry.h"
#include "au3-import-export/Import.h"

#include "pluginhost/ipluginhostservice.h"
#include "pluginhost/plugin.h"

#include "internal/pluginexporter.h"
#include "internal/pluginimporter.h"

#include "framework/global/log.h"

using namespace au::importexport;

static const std::string mname("importexport_plugins");

std::string PluginFormatsModule::moduleName() const
{
    return mname;
}

void PluginFormatsModule::resolveImports()
{
    auto pluginHost = muse::modularity::globalIoc()->resolve<au::pluginhost::IPluginHostService>(mname);
    IF_ASSERT_FAILED(pluginHost) {
        return;
    }

    int importers = 0, exporters = 0;

    for (au::pluginhost::Plugin* plugin : pluginHost->plugins()) {
        if (const auplug_importer_factory_t* factory = plugin->importerFactory()) {
            const uint32_t n = factory->count ? factory->count() : 0;
            for (uint32_t i = 0; i < n; ++i) {
                auto importer = std::make_unique<PluginImporter>(*factory, i);
                if (!importer->isValid()) {
                    LOGW() << "skipping invalid plugin importer " << i << " from " << plugin->desc().id;
                    continue;
                }
                const wxString id = importer->GetPluginStringID();
                Importer::RegisteredImportPlugin { Identifier(id), std::move(importer) };
                ++importers;
            }
        }

        if (const auplug_exporter_factory_t* factory = plugin->exporterFactory()) {
            const uint32_t n = factory->count ? factory->count() : 0;
            for (uint32_t i = 0; i < n; ++i) {
                auto probe = std::make_unique<PluginExporter>(*factory, i);
                if (!probe->isValid()) {
                    LOGW() << "skipping invalid plugin exporter " << i << " from " << plugin->desc().id;
                    continue;
                }
                const wxString id = wxString::FromUTF8(plugin->desc().id) + wxString::Format(wxT("/%u"), i);
                ExportPluginRegistry::RegisteredPlugin {
                    Identifier(id),
                    [factory, i] { return std::make_unique<PluginExporter>(*factory, i); }
                };
                ++exporters;
            }
        }
    }

    if (importers || exporters) {
        LOGI() << "registered plugin formats: " << importers << " importer(s), " << exporters << " exporter(s)";
    }
}
