/*
* Audacity: A Digital Audio Editor
*/
#include "plugineffectsmodule.h"

#include "audioplugins/iaudiopluginsscannerregister.h"
#include "audioplugins/iaudiopluginmetareaderregister.h"

#include "effects/effects_base/iparameterextractorregistry.h"
#include "effects/effects_base/ieffectviewlaunchregister.h"
#include "effects/effects_base/ieffectloadersregister.h"
#include "effects/effects_base/internal/au3/au3audiopluginscanner.h"
#include "effects/builtin/ibuiltineffectsviewregister.h"

#include "pluginhost/plugin.h"
#include "pluginhost/api/audacity_plugin.h"
#include "pluginhost/ipluginhostservice.h"

#include "internal/plugineffectviewsservice.h"
#include "internal/plugineffectsprovider.h"
#include "internal/plugineffectsmetareader.h"
#include "internal/plugineffectsloader.h"
#include "internal/pluginviewlauncher.h"
#include "internal/pluginparameterextractorservice.h"

#include "framework/global/log.h"

namespace {
const std::string mname("effects_plugins");
}

using namespace au::effects;

PluginEffectsModule::PluginEffectsModule() = default;

PluginEffectsModule::~PluginEffectsModule() = default;

std::string PluginEffectsModule::moduleName() const
{
    return mname;
}

void PluginEffectsModule::registerExports()
{
    m_viewsService = std::make_shared<PluginEffectViewsService>();
    globalIoc()->registerExport<IPluginEffectViews>(mname, m_viewsService);
}

void PluginEffectsModule::resolveImports()
{
    auto pluginHost = globalIoc()->resolve<au::pluginhost::IPluginHostService>(mname);
    IF_ASSERT_FAILED(pluginHost) {
        return;
    }

    for (au::pluginhost::Plugin* plugin : pluginHost->plugins()) {
        if (!plugin->effectFactory()) {
            continue;
        }
        auto provider = std::make_unique<PluginEffectsProvider>(*plugin);
        if (!provider->Initialize()) {
            LOGE() << "ABI plugin has no usable effect factory: " << plugin->path();
            continue;
        }
        m_providers.push_back(std::move(provider));
    }

    m_effectsLoader = std::make_shared<PluginEffectsLoader>();

    auto scannerRegister = globalIoc()->resolve<muse::audioplugins::IAudioPluginsScannerRegister>(mname);
    auto metaReaderRegister = globalIoc()->resolve<muse::audioplugins::IAudioPluginMetaReaderRegister>(mname);

    for (const auto& provider : m_providers) {
        auto scanner = std::make_shared<Au3AudioPluginScanner>(*provider);
        m_scanners.push_back(scanner);
        if (scannerRegister) {
            scannerRegister->registerScanner(scanner);
        }

        auto metaReader = std::make_shared<PluginEffectsMetaReader>(*provider);
        m_metaReaders.push_back(metaReader);
        if (metaReaderRegister) {
            metaReaderRegister->registerReader(metaReader);
        }

        m_effectsLoader->addProvider(*provider);
    }

    auto loadersRegister = globalIoc()->resolve<IEffectLoadersRegister>(mname);
    if (loadersRegister) {
        loadersRegister->registerLoader(m_effectsLoader);
    }

    // Custom QML views shipped by plugins are served through the same
    // register and viewer path as the builtin effects: a synthetic url
    // routes the viewer to the string-loading path.
    PluginEffectViewsService::EffectViews effectViews;
    for (const au::pluginhost::PluginView& view : pluginHost->views()) {
        if (view.role == AUPLUG_VIEW_EFFECT && !view.ref.empty()) {
            effectViews.push_back({ view.ref, view.qml });
        }
    }

    auto viewsRegister = globalIoc()->resolve<IBuiltinEffectsViewRegister>(mname);
    if (viewsRegister) {
        for (const auto& [effectSymbol, qml] : effectViews) {
            viewsRegister->regUrl(effectSymbol, u"auplug-view://" + effectSymbol);
        }
    }
    m_viewsService->setViews(std::move(effectViews));

    // family-wide, not per project window
    auto paramExtractorRegistry = globalIoc()->resolve<IParameterExtractorRegistry>(mname);
    if (paramExtractorRegistry) {
        paramExtractorRegistry->registerExtractor(std::make_shared<PluginParameterExtractorService>());
    }
}

void PluginEffectsModule::onInit(const muse::IApplication::RunMode& mode)
{
    for (const auto& metaReader : m_metaReaders) {
        metaReader->init();
    }
    if (m_effectsLoader) {
        m_effectsLoader->init();
    }
    for (const auto& scanner : m_scanners) {
        scanner->init(mode);
    }
}

void PluginEffectsModule::onDeinit()
{
    for (const auto& metaReader : m_metaReaders) {
        metaReader->deinit();
    }
    if (m_effectsLoader) {
        m_effectsLoader->deinit();
    }
    for (const auto& scanner : m_scanners) {
        scanner->deinit();
    }
}

muse::modularity::IContextSetup* PluginEffectsModule::newContext(const muse::modularity::ContextPtr& ctx) const
{
    return new PluginEffectsContext(ctx);
}

PluginEffectsContext::PluginEffectsContext(const muse::modularity::ContextPtr& ctx)
    : muse::modularity::IContextSetup(ctx)
{
}

void PluginEffectsContext::resolveImports()
{
    auto launchRegister = ioc()->resolve<IEffectViewLaunchRegister>(mname);
    if (launchRegister) {
        launchRegister->regLauncher(EffectFamily::Plugin, std::make_shared<PluginViewLauncher>(iocContext()));
    }
}
