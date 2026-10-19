/*
 * Audacity: A Digital Audio Editor
 */
#include "LV2PluginId.h"

LV2PluginId::LV2PluginId(const LilvPlugin* plugin)
    : pluginUri{lilv_node_as_string(lilv_plugin_get_uri(plugin))},
    bundleUri{lilv_node_as_string(lilv_plugin_get_bundle_uri(plugin))}
{}

LV2PluginId::LV2PluginId(std::string pluginUri, std::string bundleUri)
    : pluginUri{std::move(pluginUri)}, bundleUri{std::move(bundleUri)} {}

std::string LV2PluginId::Serialize() const
{
    return pluginUri + "@" + bundleUri;
}

std::optional<LV2PluginId> LV2PluginId::Deserialize(const std::string& str)
{
    // The serialized ID contains a plugin URI and a local bundle file URI.
    // An '@' in a VST filename or URI is not sufficient to identify LV2.
    const size_t pos = str.find("@file://");
    const size_t scheme = str.find(':');
    if (pos == std::string::npos || pos == 0 || pos + 8 >= str.size()
        || scheme == std::string::npos || scheme >= pos) {
        return std::nullopt;
    }
    return LV2PluginId{ str.substr(0, pos), str.substr(pos + 1) };
}

bool LV2PluginId::operator==(const LV2PluginId& other) const
{
    return pluginUri == other.pluginUri && bundleUri == other.bundleUri;
}

bool LV2PluginId::operator!=(const LV2PluginId& other) const
{
    return !(*this == other);
}
