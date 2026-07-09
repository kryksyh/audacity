/*
* Audacity: A Digital Audio Editor
*/
#include "plugineffectsmetareader.h"

#include "pluginstypes.h"

#include "au3-components/PluginProvider.h"

using namespace au::effects;

muse::audioplugins::PluginType PluginEffectsMetaReader::metaType() const
{
    return std::string(plugins::AUDIO_RESOURCE_TYPE_NAME);
}

bool PluginEffectsMetaReader::canReadMeta(const muse::io::path_t& pluginPath) const
{
    return m_provider.CheckPluginExist(wxString::FromUTF8(pluginPath.toStdString()));
}
