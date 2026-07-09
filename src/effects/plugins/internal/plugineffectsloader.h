/*
* Audacity: A Digital Audio Editor
*/
#pragma once

#include <vector>

#include "effects/effects_base/ieffectloader.h"
#include "effects/effects_base/internal/au3effectloader.h"

#include "framework/audioplugins/iknownaudiopluginsregister.h"
#include "framework/global/modularity/ioc.h"

class PluginProvider;

namespace au::effects {
//! There is one effect loader per family, but effects of the Plugin family
//! can come from several loaded plugins: dispatch by provider.
class PluginEffectsLoader final : public IEffectLoader
{
    muse::GlobalInject<muse::audioplugins::IKnownAudioPluginsRegister> knownPlugins;

public:
    void addProvider(PluginProvider& provider);

    void init();
    void deinit();

    EffectFamily family() const override;
    bool ensurePluginIsLoaded(const EffectId& effectId) override;
    Effect* effect(const EffectId& effectId) const override;

private:
    Au3EffectLoader* loaderFor(const EffectId& effectId) const;

    struct Entry {
        PluginProvider* provider = nullptr;
        std::shared_ptr<Au3EffectLoader> loader;
    };

    std::vector<Entry> m_entries;
};
}
