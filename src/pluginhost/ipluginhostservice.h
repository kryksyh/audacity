/*
* Audacity: A Digital Audio Editor
*/
#pragma once

#include <vector>

#include "framework/global/modularity/imoduleinterface.h"
#include "framework/global/types/string.h"

namespace au::pluginhost {
class Plugin;

//! A QML view shipped by a stable-ABI plugin as source text
struct PluginView {
    int32_t role = 0;    // auplug_view_role
    muse::String ref;    // e.g. the effect id for effect views
    muse::String qml;
    muse::String pluginId;    // the owning plugin's auplug_desc_t::id
    muse::String pluginName;  // the owning plugin's auplug_desc_t::name
};

//! Discovers and loads dynamically loaded plugins (stable C ABI). Plugins
//! are generic capability providers; capability-specific hosts (e.g.
//! effects) bind the capabilities they understand.
class IPluginHostService : MODULE_GLOBAL_INTERFACE
{
    INTERFACE_ID(IPluginHostService)

public:
    virtual ~IPluginHostService() = default;

    virtual const std::vector<Plugin*>& plugins() const = 0;
    virtual const std::vector<PluginView>& views() const = 0;
};
}
