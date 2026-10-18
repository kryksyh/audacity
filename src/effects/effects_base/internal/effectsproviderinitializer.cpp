/*
 * Audacity: A Digital Audio Editor
 */
#include "effectsproviderinitializer.h"

namespace au::effects {
void EffectsProviderInitializer::setStartupPluginValidationPolicy(StartupPluginValidationPolicy policy)
{
    m_validationPolicy = policy;
}

void EffectsProviderInitializer::callAfterSplashScreen()
{
    static bool effectsProviderInitialized = false;
    if (!effectsProviderInitialized) {
        effectsProvider()->initOnce(iocContext(), *interactive(), *registerAudioPluginsScenario(), m_validationPolicy);
        effectsProviderInitialized = true;
    }
    m_initializationCompleted = true;
}
} // namespace au::effects
