/*
* Audacity: A Digital Audio Editor
*/
#include "plugineffectviewsservice.h"

using namespace au::effects;

muse::String PluginEffectViewsService::effectViewQml(const muse::String& effectSymbol) const
{
    for (const auto& [symbol, qml] : m_views) {
        if (symbol == effectSymbol) {
            return qml;
        }
    }
    return {};
}
