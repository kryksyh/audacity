/*
 * Audacity: A Digital Audio Editor
 */
#include <gmock/gmock.h>

#include <QFile>
#include <QTemporaryDir>

#include "effects/extensions/internal/extensioneffectsrepository.h"
#include "effects/extensions/internal/extensioneffectsscanner.h"
#include "effects/extensions/internal/extensiontypes.h"
#include "framework/audioplugins/iregisteraudiopluginsscenario.h"
#include "framework/audioplugins/tests/mocks/knownaudiopluginsregistermock.h"

using namespace testing;

namespace au::effects::extensions {
namespace {
class ExtensionsRegisterMock : public muse::extensions::IExtensionsRegister
{
public:
    MOCK_METHOD(void, reload, (), (override));
    MOCK_METHOD(muse::extensions::ManifestList, manifestList, (muse::extensions::Filter), (const, override));
    MOCK_METHOD(muse::async::Notification, manifestListChanged, (), (const, override));
    MOCK_METHOD(muse::extensions::KnownCategories, knownCategories, (), (const, override));
    MOCK_METHOD(bool, exists, (const muse::extensions::ExtensionUri&), (const, override));
    MOCK_METHOD(const muse::extensions::Manifest&, manifest, (const muse::extensions::ExtensionUri&), (const, override));
    MOCK_METHOD(void, setEnabled, (const muse::extensions::ExtensionUri&, bool), (override));
    MOCK_METHOD(bool, isEnabled, (const muse::extensions::ExtensionUri&), (const, override));
    MOCK_METHOD(muse::async::Channel<muse::extensions::ExtensionUri>, enabledChanged, (), (const, override));
};

class RegistrationScenarioMock : public muse::audioplugins::IRegisterAudioPluginsScenario
{
public:
    MOCK_METHOD(muse::audioplugins::PluginScanResult, scanPlugins, (muse::Progress*), (const, override));
    MOCK_METHOD(muse::Ret, updatePluginsRegistry, (), (override));
    MOCK_METHOD(muse::Ret, rescanAllPlugins, (), (override));
    MOCK_METHOD(muse::Ret, registerNewPlugins, (const muse::io::paths_t&, bool), (override));
    MOCK_METHOD(muse::Ret, unregisterRemovedPlugins, (const muse::audioplugins::PluginResourceIdList&), (override));
    MOCK_METHOD(muse::Ret, registerPlugin, (const muse::io::path_t&), (override));
    MOCK_METHOD(muse::Ret, validatePlugin, (const muse::io::path_t&, const muse::io::path_t&), (override));
};

class ExtensionEffectsScannerTests : public Test
{
protected:
    using Known = muse::audioplugins::IKnownAudioPluginsRegister;
    using Extensions = muse::extensions::IExtensionsRegister;

    void SetUp() override
    {
        auto* ioc = muse::modularity::globalIoc();
        m_previousKnown = ioc->resolve<Known>("utests");
        m_previousExtensions = ioc->resolve<Extensions>("utests");
        if (m_previousKnown) {
            ioc->unregister<Known>("utests");
        }
        if (m_previousExtensions) {
            ioc->unregister<Extensions>("utests");
        }
        ioc->registerExport<Known>("utests", m_known);
        ioc->registerExport<Extensions>("utests", m_extensions);
        ON_CALL(*m_extensions, manifestList(_)).WillByDefault(Return(muse::extensions::ManifestList {}));
    }

    void TearDown() override
    {
        auto* ioc = muse::modularity::globalIoc();
        ioc->unregister<Known>("utests");
        ioc->unregister<Extensions>("utests");
        if (m_previousKnown) {
            ioc->registerExport<Known>("utests", m_previousKnown);
        }
        if (m_previousExtensions) {
            ioc->registerExport<Extensions>("utests", m_previousExtensions);
        }
    }

    std::shared_ptr<StrictMock<muse::audioplugins::KnownAudioPluginsRegisterMock> > m_known
        = std::make_shared<StrictMock<muse::audioplugins::KnownAudioPluginsRegisterMock> >();
    std::shared_ptr<NiceMock<ExtensionsRegisterMock> > m_extensions = std::make_shared<NiceMock<ExtensionsRegisterMock> >();
    std::shared_ptr<Known> m_previousKnown;
    std::shared_ptr<Extensions> m_previousExtensions;
    StrictMock<RegistrationScenarioMock> m_scenario;
};
}

TEST_F(ExtensionEffectsScannerTests, UnchangedExtensionsDoNotReloadTheAudioPluginRegister)
{
    muse::audioplugins::AudioPluginInfo external;
    external.meta.type = "VST3";
    external.path = "/plugins/existing.vst3";
    EXPECT_CALL(*m_known, pluginInfoList(_)).Times(2).WillRepeatedly(Return(muse::audioplugins::AudioPluginInfoList { external }));
    const auto repository = std::make_shared<ExtensionEffectsRepository>();
    ExtensionEffectsScanner scanner(repository);
    scanner.refreshPlugins(m_scenario);
    scanner.refreshPlugins(m_scenario);
}

TEST_F(ExtensionEffectsScannerTests, RemovingAnExtensionReloadsTheRegister)
{
    muse::audioplugins::AudioPluginInfo obsolete;
    obsolete.meta.type = AUDIO_RESOURCE_TYPE_NAME;
    obsolete.path = "/extensions/removed/manifest.json";
    EXPECT_CALL(*m_known, pluginInfoList(_)).WillOnce(Return(muse::audioplugins::AudioPluginInfoList { obsolete }));
    EXPECT_CALL(*m_known, removePluginsAtPath(obsolete.path)).WillOnce(Return(muse::make_ok()));
    EXPECT_CALL(*m_known, load()).WillOnce(Return(muse::make_ok()));
    ExtensionEffectsScanner scanner(std::make_shared<ExtensionEffectsRepository>());
    scanner.refreshPlugins(m_scenario);
}

TEST_F(ExtensionEffectsScannerTests, AddingAnExtensionReloadsTheRegister)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    QFile script(directory.filePath("effect.js"));
    ASSERT_TRUE(script.open(QIODevice::WriteOnly));
    script.close();
    muse::extensions::Manifest manifest;
    manifest.uri = muse::extensions::ExtensionUri("audacity://extensions/test");
    manifest.path = directory.filePath("manifest.json");
    manifest.contributes["audacity.effects"] = {
        { { "id", muse::Val("test") }, { "title", muse::Val("Test") },
            { "group", muse::Val("effect") }, { "script", muse::Val("effect.js") } }
    };
    ON_CALL(*m_extensions, manifestList(_)).WillByDefault(Return(muse::extensions::ManifestList { manifest }));
    EXPECT_CALL(*m_known, exists(manifest.path)).WillOnce(Return(false));
    EXPECT_CALL(m_scenario, registerPlugin(manifest.path)).WillOnce(Return(muse::make_ok()));
    EXPECT_CALL(*m_known, pluginInfoList(_)).WillOnce(Return(muse::audioplugins::AudioPluginInfoList {}));
    EXPECT_CALL(*m_known, load()).WillOnce(Return(muse::make_ok()));
    ExtensionEffectsScanner scanner(std::make_shared<ExtensionEffectsRepository>());
    scanner.refreshPlugins(m_scenario);
}
} // namespace au::effects::extensions
