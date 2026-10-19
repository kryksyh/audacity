/*
* Audacity: A Digital Audio Editor
*/
#pragma once

#include "modularity/ioc.h"

#include "tracktionaudio/internal/tracktionpluginscanner.h"
#include "effects/effects_base/ieffectsconfiguration.h"

namespace au::effects {
class Lv2PluginsScanner final : public tracktionaudio::TracktionPluginsScanner
{
    muse::GlobalInject<IEffectsConfiguration> effectsConfiguration;

public:
    Lv2PluginsScanner()
        : TracktionPluginsScanner(tracktionaudio::PluginFormat::LV2) {}

protected:
    muse::io::paths_t customPaths() const override
    {
        return effectsConfiguration()->lv2CustomPaths();
    }
};
}
