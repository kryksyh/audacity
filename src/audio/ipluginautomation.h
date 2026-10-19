/*
* Audacity: A Digital Audio Editor
*/
#pragma once

#include "iinstrumenteditor.h"

class RealtimeEffectState;

namespace au::audio {
class IPluginAutomation : MODULE_GLOBAL_INTERFACE
{
    INTERFACE_ID(IPluginAutomation)
public:
    virtual std::vector<InstrumentParameter> parameters(RealtimeEffectState& effect) const = 0;
};
}
