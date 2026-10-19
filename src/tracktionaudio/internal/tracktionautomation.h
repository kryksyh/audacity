/*
* Audacity: A Digital Audio Editor
*/
#pragma once

#include <tracktion_engine/tracktion_engine.h>
#include "au3-automation-data/AutomationData.h"

namespace au::tracktionaudio {
void applyTracktionAutomation(tracktion::Plugin& plugin, const AutomationData& data, const std::string& prefix = {});
bool hasTrackAutomation(const AutomationData& data);
}
