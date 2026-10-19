/*
 * Audacity: A Digital Audio Editor
 */
#pragma once

#include <map>

#include "effects/effects_base/ieffectloader.h"
#include "audioplugins/iknownaudiopluginsregister.h"
#include "audioplugins/iaudiopluginsloadguard.h"
#include "global/iglobalconfiguration.h"
#include "global/modularity/ioc.h"

class EffectInstanceFactory;

namespace au::tracktionaudio {
class TracktionRuntime;
}

namespace au::effects {
bool tracktionLv2VendorUiSupported(const EffectInstanceFactory& effect);

class Lv2EffectLoader final : public IEffectLoader
{
public:
    muse::GlobalInject<muse::audioplugins::IKnownAudioPluginsRegister> knownPlugins;
    muse::GlobalInject<muse::audioplugins::IAudioPluginsLoadGuard> loadGuard;
    muse::GlobalInject<muse::IGlobalConfiguration> globalConfiguration;

    explicit Lv2EffectLoader(std::shared_ptr<tracktionaudio::TracktionRuntime> runtime = {});
    ~Lv2EffectLoader() override;

    void deinit();
    EffectFamily family() const override;
    bool ensurePluginIsLoaded(const EffectId& effectId) override;
    Effect* effect(const EffectId& effectId) const override;

private:
    std::shared_ptr<tracktionaudio::TracktionRuntime> m_runtime;
    std::map<EffectId, std::unique_ptr<Effect> > m_effects;
};
} // namespace au::effects
