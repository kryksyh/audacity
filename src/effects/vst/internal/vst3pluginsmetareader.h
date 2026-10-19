/*
* Audacity: A Digital Audio Editor
*/
#pragma once

#include "tracktionaudio/internal/tracktionpluginscanner.h"

namespace au::effects {
class Vst3PluginsMetaReader final : public tracktionaudio::TracktionPluginMetaReader
{
public:
    Vst3PluginsMetaReader()
        : TracktionPluginMetaReader(tracktionaudio::PluginFormat::VST3) {}
};
}
