/*
* Audacity: A Digital Audio Editor
*/
#include "pluginviewlauncher.h"

using namespace au::effects;

muse::Ret PluginViewLauncher::showEffect(const EffectInstanceId& instanceId) const
{
    return doShowEffect(instanceId, EffectFamily::Plugin);
}

void PluginViewLauncher::showRealtimeEffect(const RealtimeEffectStatePtr& state) const
{
    doShowRealtimeEffect(state);
}
