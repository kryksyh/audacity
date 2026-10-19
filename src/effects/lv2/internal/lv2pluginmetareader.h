/*
* Audacity: A Digital Audio Editor
*/
#pragma once

#include "tracktionaudio/internal/tracktionpluginscanner.h"

namespace au::effects {
class Lv2PluginMetaReader final : public tracktionaudio::TracktionPluginMetaReader
{
public:
    Lv2PluginMetaReader()
        : TracktionPluginMetaReader(tracktionaudio::PluginFormat::LV2) {}
};
}
