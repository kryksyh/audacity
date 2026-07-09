/*
* Audacity: A Digital Audio Editor
*/
#pragma once

#include "modularity/imoduleinterface.h"

#include "global/types/string.h"

namespace au::effects {
//! Access to QML views shipped by stable-ABI plugins as source text
class IPluginEffectViews : MODULE_GLOBAL_INTERFACE
{
    INTERFACE_ID(IPluginEffectViews)

public:
    virtual ~IPluginEffectViews() = default;

    //! QML source for the effect's settings view; empty if the plugin
    //! ships none for this effect symbol
    virtual muse::String effectViewQml(const muse::String& effectSymbol) const = 0;
};
}
