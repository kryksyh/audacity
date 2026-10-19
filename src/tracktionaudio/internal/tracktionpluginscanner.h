/*
* Audacity: A Digital Audio Editor
*/
#pragma once

#include "audioplugins/iaudiopluginmetareader.h"
#include "audioplugins/iaudiopluginsscanner.h"

namespace au::tracktionaudio {
enum class PluginFormat {
    VST3, AudioUnit, LV2
};

class TracktionPluginsScanner : public muse::audioplugins::IAudioPluginsScanner
{
public:
    explicit TracktionPluginsScanner(PluginFormat format);
    muse::io::paths_t scanPlugins(muse::Progress* progress = nullptr) const override;

protected:
    virtual muse::io::paths_t customPaths() const { return {}; }

private:
    const PluginFormat m_format;
};

class TracktionPluginMetaReader : public muse::audioplugins::IAudioPluginMetaReader
{
public:
    explicit TracktionPluginMetaReader(PluginFormat format);
    muse::audioplugins::PluginType metaType() const override;
    bool canReadMeta(const muse::io::path_t& path) const override;
    muse::RetVal<muse::audioplugins::PluginMetaList> readMeta(const muse::io::path_t& path) const override;

private:
    const PluginFormat m_format;
};
}
