/*
* Audacity: A Digital Audio Editor
*/
#pragma once

#include "tracktionaudio/internal/tracktionpluginscanner.h"

namespace au::effects {
class AudioUnitPluginsScanner final : public tracktionaudio::TracktionPluginsScanner
{
public:
    AudioUnitPluginsScanner()
        : TracktionPluginsScanner(tracktionaudio::PluginFormat::AudioUnit) {}
};
}
