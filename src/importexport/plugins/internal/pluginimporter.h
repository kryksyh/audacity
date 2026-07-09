/*
* Audacity: A Digital Audio Editor
*/
#pragma once

#include <string>

#include "au3-import-export/ImportPlugin.h"

#include "pluginhost/api/audacity_plugin.h"

namespace au::importexport {
//! Presents a stable-ABI importer as an AU3 import plugin
class PluginImporter final : public ImportPlugin
{
public:
    PluginImporter(const auplug_importer_factory_t& factory, uint32_t index);

    //! False if the plugin failed to describe this importer
    bool isValid() const { return m_valid; }

    wxString GetPluginStringID() override;
    TranslatableString GetPluginFormatDescription() override;
    std::unique_ptr<ImportFileHandle> Open(const FilePath& filename, AudacityProject*) override;

private:
    const auplug_importer_factory_t& m_factory;
    const uint32_t m_index;
    bool m_valid = false;
    std::string m_id;
    std::string m_formatName;
};

class PluginImportHandle final : public ImportFileHandleEx
{
public:
    PluginImportHandle(const auplug_importer_factory_t& factory, uint32_t index, const FilePath& filename,
                       const wxString& formatName);

    TranslatableString GetFileDescription() override;
    ByteCount GetFileUncompressedBytes() override;
    double GetDuration() const override;
    int GetRequiredTrackCount() const override;
    wxInt32 GetStreamCount() override;
    const TranslatableStrings& GetStreamInfo() override;
    void SetStreamUsage(wxInt32 streamId, bool use) override;

    void Import(ImportProgressListener& progressListener, WaveTrackFactory* trackFactory, TrackHolders& outTracks,
                Tags* tags, std::optional<LibFileFormats::AcidizerTags>& acidTags) override;

private:
    const auplug_importer_factory_t& m_factory;
    const uint32_t m_index;
    wxString m_formatName;
};
}
