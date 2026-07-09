/*
* Audacity: A Digital Audio Editor
*/
#pragma once

#include "effects/effects_base/internal/au3/au3audiopluginmetareader.h"

class PluginProvider;

namespace au::effects {
class PluginEffectsMetaReader : public Au3AudioPluginMetaReader
{
public:
    PluginEffectsMetaReader(PluginProvider& provider)
        : Au3AudioPluginMetaReader(provider), m_provider(provider) {}

    muse::audioplugins::PluginType metaType() const override;
    bool canReadMeta(const muse::io::path_t& pluginPath) const override;

private:
    PluginProvider& m_provider;
};
}
