/*
* Audacity: A Digital Audio Editor
*/
#include "testing/environment.h"

#include <QDir>
#include <QTemporaryDir>
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
}

static const muse::testing::SuiteEnvironment tracktionaudioEnvironment = muse::testing::SuiteEnvironment()
                                                                         .setDependencyModules({ new au::au3::Au3WrapModule() })
                                                                         .setPreInit([] {
    if (!qEnvironmentVariableIsEmpty("AU_TRACKTION_TEST_AUDIO_HOST")) {
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
    if (physicalAudioInitialised) {
        Pa_Terminate();
        physicalAudioInitialised = false;
    }
    muse::modularity::globalIoc()->unregister<au::project::IProjectConfiguration>("utests");
    muse::modularity::globalIoc()->unregister<au::playback::IPlaybackConfiguration>("utests");
});
