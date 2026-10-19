/*
* Audacity: A Digital Audio Editor
*/
#pragma once

#include "tracktionaudio/internal/tracktionpluginscanner.h"

namespace au::effects {
class AudioUnitPluginsMetaReader final : public tracktionaudio::TracktionPluginMetaReader
{
public:
    AudioUnitPluginsMetaReader()
        : TracktionPluginMetaReader(tracktionaudio::PluginFormat::AudioUnit) {}
};
}
