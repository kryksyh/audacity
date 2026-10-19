/*
* Audacity: A Digital Audio Editor
*/
#include <tracktion_engine/tracktion_engine.h>

#include <gtest/gtest.h>
#include <QTemporaryDir>
#include <QUrl>

#include "../internal/tracktionpluginscanner.h"
#include "../internal/tracktionplugindescriptions.h"
#include "../internal/tracktionpluginloader.h"
#include "../internal/tracktionruntime.h"
#include "effects/effects_base/internal/effectsutils.h"
#include "effects/effects_base/effectstypes.h"
#include "global/progress.h"
#include "effects/lv2/internal/lv2pluginmetareader.h"
#include "effects/lv2/internal/lv2pluginsscanner.h"

namespace au::tracktionaudio {
namespace {
std::string identifier(const std::string& name, const std::string& bundle = AU_TRACKTION_TEST_LV2_BUNDLE)
{
    const auto uri = QUrl::fromLocalFile(QString::fromStdString(bundle) + "/").toEncoded().replace("%25", "%%").toStdString();
    return "urn:audacity:tracktion-test:" + name + "@" + uri;
}

class Scanner final : public TracktionPluginsScanner
{
public:
    explicit Scanner(muse::io::paths_t paths)
        : TracktionPluginsScanner(PluginFormat::LV2), m_paths(std::move(paths)) {}

    muse::io::paths_t customPaths() const override { return m_paths; }

private:
    muse::io::paths_t m_paths;
};
}

TEST(Lv2ScannerTests, SearchKeepsAudacityIdentifiersAndFiltersUnsupportedClasses)
{
    const auto parent = juce::File(AU_TRACKTION_TEST_LV2_BUNDLE).getParentDirectory().getFullPathName().toStdString();
    const Scanner scanner({ parent, parent, AU_TRACKTION_TEST_LV2_BUNDLE });
    const auto paths = scanner.scanPlugins();
    for (const auto* name : { "gain", "generator", "analyzer", "tool", "unsupported", "no-author", "unicode", "instrument" }) {
        SCOPED_TRACE(name);
        EXPECT_EQ(std::count(paths.begin(), paths.end(), muse::io::path_t(identifier(name))), 1);
    }
    for (const auto* name : { "midi", "math", "empty" }) {
        SCOPED_TRACE(name);
        EXPECT_EQ(std::count(paths.begin(), paths.end(), muse::io::path_t(identifier(name))), 0);
    }
    muse::Progress progress;
    progress.cancel();
    EXPECT_TRUE(scanner.scanPlugins(&progress).empty());
}

TEST(Lv2ScannerTests, NativeValidationKeepsTheAudacityMetadataSchema)
{
    const effects::Lv2PluginMetaReader reader;
    struct Case {
        const char* name;
        const char* title;
        effects::EffectType type;
    };
    for (const auto& test : { Case { "gain", "Tracktion LV2 Gain", effects::EffectType::Processor },
                              Case { "generator", "Tracktion LV2 Generator", effects::EffectType::Generator },
                              Case { "analyzer", "Tracktion LV2 Analyzer", effects::EffectType::Analyzer },
                              Case { "tool", "Tracktion LV2 Tool", effects::EffectType::Tool },
                              Case { "instrument", "Tracktion LV2 Instrument", effects::EffectType::Instrument } }) {
        SCOPED_TRACE(test.name);
        const auto path = identifier(test.name);
        ASSERT_TRUE(reader.canReadMeta(path));
        const auto result = reader.readMeta(path);
        ASSERT_TRUE(result.ret) << result.ret.toString();
        ASSERT_EQ(result.val.size(), 1u);
        const auto& meta = result.val.front();
        EXPECT_EQ(meta.id, effects::utils::effectId(u"LV2", u"Audacity", muse::String::fromUtf8(test.title), path).toStdString());
        EXPECT_EQ(meta.type, "Lv2Plugin");
        EXPECT_EQ(meta.vendor, "Audacity");
        EXPECT_EQ(meta.attributeVal(effects::EFFECT_TITLE_ATTRIBUTE), muse::String::fromUtf8(test.title));
        EXPECT_EQ(meta.attributeVal(effects::EFFECT_VERSION_ATTRIBUTE), u"1.0");
        EXPECT_EQ(meta.attributeVal(effects::EFFECT_DESCRIPTION_ATTRIBUTE), u"n/a");
        EXPECT_EQ(meta.attributeVal(effects::EFFECT_MODULE_ATTRIBUTE), u"Module__The Audacity Team_LV2 Effects_");
        EXPECT_EQ(muse::audioplugins::boolAttribute(meta, effects::EFFECT_IS_REALTIME_CAPABLE_ATTRIBUTE),
                  test.type == effects::EffectType::Processor);
        EXPECT_EQ(muse::audioplugins::boolAttribute(meta, effects::EFFECT_PARAMS_ARE_INPUT_AGNOSTIC_ATTRIBUTE),
                  test.type != effects::EffectType::Generator);
        EXPECT_EQ(effects::utils::museToAuEffectMeta(path, meta).type, test.type);
        const auto xml = juce::parseXML(meta.attributeVal(TRACKTION_PLUGIN_DESCRIPTION_ATTRIBUTE).toStdString());
        ASSERT_TRUE(xml);
        juce::PluginDescription description;
        ASSERT_TRUE(description.loadFromXml(*xml));
        EXPECT_EQ(description.pluginFormatName, "LV2");
        EXPECT_EQ(description.fileOrIdentifier.toStdString(), "urn:audacity:tracktion-test:" + std::string(test.name));
    }
    const auto noAuthor = reader.readMeta(identifier("no-author"));
    ASSERT_TRUE(noAuthor.ret);
    ASSERT_EQ(noAuthor.val.size(), 1u);
    EXPECT_EQ(noAuthor.val.front().vendor, "n/a");
    EXPECT_FALSE(reader.readMeta(identifier("unsupported")).ret);
    const auto unicode = reader.readMeta(identifier("unicode"));
    ASSERT_TRUE(unicode.ret);
    ASSERT_EQ(unicode.val.size(), 1u);
    EXPECT_EQ(unicode.val.front().vendor, "Audacity Équipe");
    EXPECT_EQ(unicode.val.front().attributeVal(effects::EFFECT_TITLE_ATTRIBUTE), u"Tracktion LV2 Étage");
}

TEST(Lv2ScannerTests, BundleLocationsWithEscapedCharactersRemainValid)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const auto bundle = directory.filePath(QString::fromUtf8("LV2 é + # 100%.lv2")).toStdString();
    ASSERT_TRUE(juce::File(AU_TRACKTION_TEST_LV2_BUNDLE).copyDirectoryTo(juce::File(bundle)));
    const Scanner scanner({ bundle });
    const auto paths = scanner.scanPlugins();
    const auto path = identifier("gain", bundle);
    EXPECT_EQ(std::count(paths.begin(), paths.end(), muse::io::path_t(path)), 1);
    const TracktionPluginMetaReader reader(PluginFormat::LV2);
    const auto result = reader.readMeta(path);
    ASSERT_TRUE(result.ret) << result.ret.toString();
    ASSERT_EQ(result.val.size(), 1u);
    EXPECT_EQ(effects::utils::parseEffectPath(muse::String::fromStdString(result.val.front().id)), path);
}

TEST(Lv2ScannerTests, ValidatedDescriptionsLoadTheSelectedPluginThroughTracktion)
{
    const TracktionPluginMetaReader reader(PluginFormat::LV2);
    const auto path = identifier("gain");
    const auto result = reader.readMeta(path);
    ASSERT_TRUE(result.ret);
    ASSERT_EQ(result.val.size(), 1u);
    muse::audioplugins::AudioPluginInfo info { result.val.front(), path, muse::audioplugins::AudioPluginState::Validated };
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    TracktionRuntime runtime;
    runtime.init(directory.filePath("cache").toStdString(), directory.filePath("settings").toStdString());
    auto& manager = runtime.engine().getPluginManager();
    restoreTracktionPluginDescriptions(runtime.engine(), { info });
    ASSERT_EQ(manager.knownPluginList.getNumTypes(), 1);
    juce::AudioPluginFormat* lv2 = nullptr;
    for (auto* format : manager.pluginFormatManager.getFormats()) {
        if (format->getName() == "LV2") {
            lv2 = format;
        }
    }
    ASSERT_TRUE(lv2);
    const auto description = findTracktionPluginDescription(manager, *lv2, path, {});
    ASSERT_TRUE(description);
    EXPECT_EQ(description->fileOrIdentifier, "urn:audacity:tracktion-test:gain");
    juce::String error;
    const auto processor = loadTracktionPlugin(manager.pluginFormatManager, *description, error);
    ASSERT_TRUE(processor) << error.toStdString();
    ASSERT_EQ(processor->getParameters().size(), 1);
    processor->prepareToPlay(48000.0, 64);
    juce::AudioBuffer<float> audio(1, 64);
    std::fill_n(audio.getWritePointer(0), 64, 0.8f);
    juce::MidiBuffer midi;
    processor->processBlock(audio, midi);
    EXPECT_NEAR(audio.getSample(0, 63), 0.4f, 0.00001f);
    processor->getParameters()[0]->setValueNotifyingHost(1.0f);
    std::fill_n(audio.getWritePointer(0), 64, 0.8f);
    processor->processBlock(audio, midi);
    EXPECT_NEAR(audio.getSample(0, 63), 1.6f, 0.00001f);
    processor->releaseResources();
    EXPECT_FALSE(runtime.engine().getDeviceManager().deviceManager.getCurrentAudioDevice());
    info.state = muse::audioplugins::AudioPluginState::Error;
    restoreTracktionPluginDescriptions(runtime.engine(), { info });
    EXPECT_EQ(manager.knownPluginList.getNumTypes(), 0);
    info.state = muse::audioplugins::AudioPluginState::Validated;
    info.path = identifier("analyzer");
    restoreTracktionPluginDescriptions(runtime.engine(), { info });
    EXPECT_EQ(manager.knownPluginList.getNumTypes(), 0);
    EXPECT_FALSE(findTracktionPluginDescription(manager, *lv2, identifier("missing"), {}));
    EXPECT_FALSE(findTracktionPluginDescription(manager, *lv2, identifier("gain", directory.filePath("missing.lv2").toStdString()), {}));
}

TEST(Lv2ScannerTests, InvalidIdentifiersDoNotLoadBundles)
{
    const TracktionPluginMetaReader reader(PluginFormat::LV2);
    for (const auto* path : { "", "/plugin.lv2", "urn:plugin", "urn:plugin@", "@file:///plugins/test.lv2/",
                              "relative@file:///plugins/test.lv2/", "urn:plugin@https://example.com/test.lv2/" }) {
        SCOPED_TRACE(path);
        EXPECT_FALSE(reader.canReadMeta(path));
        EXPECT_FALSE(reader.readMeta(path).ret);
    }
    EXPECT_FALSE(reader.readMeta(identifier("missing")).ret);
    EXPECT_FALSE(reader.readMeta(identifier("gain", "/missing/bundle.lv2")).ret);
}
}
