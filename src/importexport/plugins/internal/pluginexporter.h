/*
* Audacity: A Digital Audio Editor
*/
#pragma once

#include <string>

#include "au3-import-export/ExportPlugin.h"

#include "pluginhost/api/audacity_plugin.h"

namespace au::importexport {
//! Presents a stable-ABI exporter as an AU3 export plugin
class PluginExporter final : public ExportPlugin
{
public:
    PluginExporter(const auplug_exporter_factory_t& factory, uint32_t index);

    //! False if the plugin failed to describe this exporter
    bool isValid() const { return m_valid; }

    int GetFormatCount() const override;
    FormatInfo GetFormatInfo(int index) const override;
    std::unique_ptr<ExportOptionsEditor> CreateOptionsEditor(int formatIndex,
                                                             ExportOptionsEditor::Listener* listener) const override;
    std::unique_ptr<ExportProcessor> CreateProcessor(int format) const override;

private:
    const auplug_exporter_factory_t& m_factory;
    const uint32_t m_index;
    bool m_valid = false;
    std::string m_id;
    std::string m_formatName;
    std::string m_extension;
    unsigned m_maxChannels = 0;
};
}
