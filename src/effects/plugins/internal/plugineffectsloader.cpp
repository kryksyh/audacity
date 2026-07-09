/*
* Audacity: A Digital Audio Editor
*/
#include "plugineffectsloader.h"

#include "au3-components/PluginProvider.h"

#include "framework/global/log.h"

using namespace au::effects;

void PluginEffectsLoader::addProvider(PluginProvider& provider)
{
    m_entries.push_back({ &provider,
                          std::make_shared<Au3EffectLoader>(provider, EffectFamily::Plugin) });
}

void PluginEffectsLoader::init()
{
    for (const Entry& entry : m_entries) {
        entry.loader->init();
    }
}

void PluginEffectsLoader::deinit()
{
    for (const Entry& entry : m_entries) {
        entry.loader->deinit();
    }
}

EffectFamily PluginEffectsLoader::family() const
{
    return EffectFamily::Plugin;
}

bool PluginEffectsLoader::ensurePluginIsLoaded(const EffectId& effectId)
{
    Au3EffectLoader* loader = loaderFor(effectId);
    return loader ? loader->ensurePluginIsLoaded(effectId) : false;
}

Effect* PluginEffectsLoader::effect(const EffectId& effectId) const
{
    Au3EffectLoader* loader = loaderFor(effectId);
    return loader ? loader->effect(effectId) : nullptr;
}

Au3EffectLoader* PluginEffectsLoader::loaderFor(const EffectId& effectId) const
{
    const auto plugins = knownPlugins()->pluginInfoList();
    const auto it = std::find_if(plugins.begin(), plugins.end(), [&](const muse::audioplugins::AudioPluginInfo& info) {
        return info.meta.id == effectId.toStdString();
    });
    if (it == plugins.end()) {
        LOGE() << "plugin not in registry: " << effectId;
        return nullptr;
    }

    const wxString path = wxString::FromUTF8(it->path.toStdString());
    for (const Entry& entry : m_entries) {
        if (entry.provider->CheckPluginExist(path)) {
            return entry.loader.get();
        }
    }

    LOGE() << "no plugin provider owns plugin: " << effectId;
    return nullptr;
}
