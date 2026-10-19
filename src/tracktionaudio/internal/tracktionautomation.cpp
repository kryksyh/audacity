/*
* Audacity: A Digital Audio Editor
*/
#include "tracktionautomation.h"
#include "tracktionpluginloader.h"

namespace au::tracktionaudio {
void applyTracktionAutomation(tracktion::Plugin& plugin, const AutomationData& data, const std::string& prefix)
{
    auto* external = dynamic_cast<tracktion::ExternalPlugin*>(&plugin);
    if (external) {
        waitForTracktionPluginLoad(*external);
    }
    for (auto* parameter : plugin.getAutomatableParameters()) {
        auto& curve = parameter->getCurve();
        curve.clear(nullptr);
        const auto* lane = data.Find(prefix + parameter->paramID.toStdString());
        if (!lane || !lane->enabled) {
            continue;
        }
        for (const auto& point : lane->points) {
            curve.addPoint(tracktion::TimePosition::fromSeconds(point.time),
                           parameter->valueRange.convertFrom0to1(static_cast<float>(point.value)), static_cast<float>(point.curve),
                           nullptr);
        }
    }
}

bool hasTrackAutomation(const AutomationData& data)
{
    const auto active = [&](const char* parameter) {
        const auto* lane = data.Find(parameter);
        return lane && lane->enabled && !lane->points.empty();
    };
    return active("volume") || active("pan");
}
}
