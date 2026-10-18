/*
* Audacity: A Digital Audio Editor
*/
#include "testing/environment.h"

#include <QDir>
#include <QTemporaryDir>
#include <QLibrary>
#include <stdexcept>
#include <portaudio.h>

#include "au3wrap/au3wrapmodule.h"
#include "au3-module-manager/PluginManager.h"
#include "effects/effects_base/internal/effectconfigsettings.h"
#include "effects/effects_base/internal/effectidresolver.h"
#include "playback/tests/mocks/playbackconfigurationmock.h"
#include "project/tests/mocks/projectconfigurationmock.h"

using namespace testing;

namespace {
std::unique_ptr<QTemporaryDir> effectConfigDirectory;
bool physicalAudioInitialised = false;
std::vector<std::unique_ptr<QLibrary> > nativeFixtureLibraries;

void retainNativeFixture(const char* path)
{
    auto library = std::make_unique<QLibrary>(QString::fromUtf8(path));
    // Darwin's ASan can leave global redzones on virtual pages after dlclose.
    // AppKit subsequently maps its session plist on those pages. Retain these
    // instrumented fixtures for the test process, like a host's plugin cache.
    // Host allocations and processing remain fully instrumented.
    library->setLoadHints(QLibrary::ResolveAllSymbolsHint | QLibrary::PreventUnloadHint);
    if (!library->load()) {
        throw std::runtime_error(library->errorString().toStdString());
    }
    nativeFixtureLibraries.push_back(std::move(library));
}
}

static const muse::testing::SuiteEnvironment nativeAudioEnvironment = muse::testing::SuiteEnvironment()
                                                                      .setDependencyModules({ new au::au3::Au3WrapModule() })
                                                                      .setPreInit([] {
#ifdef AU_NATIVE_TEST_INSTRUMENT_BINARY
    retainNativeFixture(AU_NATIVE_TEST_INSTRUMENT_BINARY);
#endif
#ifdef AU_NATIVE_TEST_LV2_BINARY
    retainNativeFixture(AU_NATIVE_TEST_LV2_BINARY);
#endif
    if (!qEnvironmentVariableIsEmpty("AU_NATIVE_TEST_AUDIO_HOST")) {
        physicalAudioInitialised = Pa_Initialize() == paNoError;
    }
    auto projectConfiguration = std::make_shared<NiceMock<au::project::ProjectConfigurationMock> >();
    auto playbackConfiguration = std::make_shared<NiceMock<au::playback::PlaybackConfigurationMock> >();
    ON_CALL(*projectConfiguration, temporaryDir()).WillByDefault(Return(muse::io::path_t(QDir::tempPath().toStdString())));
    muse::modularity::globalIoc()->registerExport<au::project::IProjectConfiguration>("utests", projectConfiguration);
    muse::modularity::globalIoc()->registerExport<au::playback::IPlaybackConfiguration>("utests", playbackConfiguration);
    effectConfigDirectory = std::make_unique<QTemporaryDir>();
    const auto path = effectConfigDirectory->filePath("plugins.json").toStdString();
    PluginManager::Get().Initialize([path](const FilePath&) -> std::unique_ptr<audacity::BasicSettings> {
        return std::make_unique<au::au3::EffectConfigSettings>(path);
    }, std::make_unique<au::effects::EffectIdResolver>());
})
                                                                      .setDeInit([] {
    PluginManager::Get().Terminate();
    effectConfigDirectory.reset();
    nativeFixtureLibraries.clear();
    if (physicalAudioInitialised) {
        Pa_Terminate();
        physicalAudioInitialised = false;
    }
    muse::modularity::globalIoc()->unregister<au::project::IProjectConfiguration>("utests");
    muse::modularity::globalIoc()->unregister<au::playback::IPlaybackConfiguration>("utests");
});
