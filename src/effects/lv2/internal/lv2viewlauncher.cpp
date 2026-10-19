/*
 * Audacity: A Digital Audio Editor
 */
#include "lv2viewlauncher.h"

#include "lv2effectloader.h"

#include "au3-components/EffectInterface.h"
#include "au3-effects/Effect.h"
#include "au3-realtime-effects/RealtimeEffectState.h"

using namespace au::effects;

Lv2ViewLauncher::Lv2ViewLauncher(const muse::modularity::ContextPtr& ctx)
    : AbstractViewLauncher(ctx)
{
    effectsProvider()->effectMetaListChanged().onNotify(this, [this]() {
        m_vendorUiFailed.clear();
    });
}

muse::Ret Lv2ViewLauncher::showEffect(const EffectInstanceId& instanceId) const
{
    return doShowEffect(instanceId, EffectFamily::LV2);
}

void Lv2ViewLauncher::showRealtimeEffect(const RealtimeEffectStatePtr& state) const
{
    doShowRealtimeEffect(state);
}

bool Lv2ViewLauncher::vendorUiSupported(const EffectId& effectId) const
{
    if (m_vendorUiFailed.count(effectId)) {
        return false;
    }

    const auto* effect = effectsProvider()->effect(effectId);
    if (!effect) {
        return false;
    }

    return tracktionLv2VendorUiSupported(*effect);
}

void Lv2ViewLauncher::markVendorUiFailed(const EffectId& effectId)
{
    m_vendorUiFailed.insert(effectId);
}
