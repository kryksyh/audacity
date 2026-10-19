/*
* Audacity: A Digital Audio Editor
*/
#pragma once

#include "modularity/ioc.h"

#include "tracktionaudio/internal/tracktionpluginscanner.h"
#include "effects/effects_base/ieffectsconfiguration.h"

namespace au::effects {
class Vst3PluginsScanner : public tracktionaudio::TracktionPluginsScanner
{
    muse::GlobalInject<effects::IEffectsConfiguration> effectsConfiguration;

public:
    Vst3PluginsScanner()
        : TracktionPluginsScanner(tracktionaudio::PluginFormat::VST3) {}

protected:
    muse::io::paths_t customPaths() const override
    {
        return effectsConfiguration()->vst3CustomPaths();
    }
};
}
