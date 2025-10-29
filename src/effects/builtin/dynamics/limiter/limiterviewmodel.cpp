/*
 * Audacity: A Digital Audio Editor
 */
#include "limiterviewmodel.h"

#include "log.h"

namespace au::effects {
LimiterViewModel::LimiterViewModel(const EffectInstanceId instanceId, QObject* parent)
    : BuiltinEffectModel{instanceId, parent}
{
}

void LimiterViewModel::doReload()
{
}
}
