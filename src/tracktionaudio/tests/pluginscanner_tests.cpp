/*
* Audacity: A Digital Audio Editor
*/
#include <tracktion_engine/tracktion_engine.h>

#include <gtest/gtest.h>
#include <QTemporaryDir>

#include "../internal/tracktionpluginscanner.h"
#include "../internal/tracktionplugindescriptions.h"
#include "../internal/tracktionruntime.h"
#include "../internal/audacityexternaleffect.h"
#include "effects/effects_base/internal/effectsutils.h"
#include "effects/effects_base/effectstypes.h"
#include "au3-module-manager/ModuleManager.h"
#include "au3-components/EffectInterface.h"
#include "au3wrap/internal/wxtypes_convert.h"
#include "global/progress.h"

#if AU_TRACKTION_TEST_VST3
#include "au3-vst3/VST3EffectsModule.h"
#endif
#if AU_TRACKTION_TEST_AUDIO_UNIT
#include "au3-audio-unit/AudioUnitEffectsModule.h"
#endif

namespace au::tracktionaudio {
namespace {
void compareMetadata(PluginProvider& provider, const muse::io::path_t& path, const TracktionPluginMetaReader& reader)
{
    ASSERT_TRUE(provider.Initialize());
    ASSERT_TRUE(reader.canReadMeta(path));
    const auto actual = reader.readMeta(path);
    ASSERT_TRUE(actual.ret) << actual.ret.toString();
    ASSERT_FALSE(actual.val.empty());
    TranslatableString error;
    PluginID id;
    size_t compared = 0;
    const auto count = provider.DiscoverPluginsAtPath(au3::wxFromPath(path), error,
                                                      [&](PluginProvider* module, ComponentInterface* component) -> const PluginID& {
        const auto* effect = dynamic_cast<const EffectDefinitionInterface*>(component);
        EXPECT_TRUE(effect);
        if (!effect) {
            return id;
        }
        id = au3::wxFromString(effects::utils::effectId(effect));
        const auto found = std::find_if(actual.val.begin(), actual.val.end(), [&](const auto& meta) {
            return meta.id == id.ToStdString();
        });
        EXPECT_NE(found, actual.val.end()) << id.ToStdString();
        if (found == actual.val.end()) {
            return id;
        }
        ++compared;
        EXPECT_EQ(found->vendor, effect->GetVendor().Internal().ToStdString());
        EXPECT_EQ(found->type, reader.metaType());
        EXPECT_EQ(found->attributeVal(effects::EFFECT_TITLE_ATTRIBUTE), au3::wxToString(effect->GetSymbol().Internal()));
        EXPECT_EQ(found->attributeVal(effects::EFFECT_DESCRIPTION_ATTRIBUTE), muse::String(effect->GetDescription().translated()));
        EXPECT_EQ(found->attributeVal(effects::EFFECT_VERSION_ATTRIBUTE), au3::wxToString(effect->GetVersion()));
        EXPECT_EQ(found->attributeVal(effects::EFFECT_MODULE_ATTRIBUTE), au3::wxToString(ModuleManager::GetID(module)));
        EXPECT_EQ(muse::audioplugins::boolAttribute(*found, effects::EFFECT_IS_REALTIME_CAPABLE_ATTRIBUTE), effect->SupportsRealtime());
        EXPECT_EQ(muse::audioplugins::boolAttribute(*found, effects::EFFECT_PARAMS_ARE_INPUT_AGNOSTIC_ATTRIBUTE),
                  effect->ParamsAreInputAgnostic());
        const auto xml = juce::parseXML(found->attributeVal(TRACKTION_PLUGIN_DESCRIPTION_ATTRIBUTE).toStdString());
        EXPECT_TRUE(xml);
        if (xml) {
            juce::PluginDescription description;
            EXPECT_TRUE(description.loadFromXml(*xml));
            if (effect->GetFamily().Internal() == wxT("VST3")) {
                EXPECT_EQ(description.uniqueIdString.toStdString(), effect->GetPath().AfterLast(';').ToStdString());
            }
        }
        return id;
    });
    EXPECT_TRUE(error.empty()) << error.debugStr().toStdString();
    EXPECT_EQ(count, actual.val.size());
    EXPECT_EQ(compared, actual.val.size());
    provider.Terminate();
}
}

TEST(PluginScannerTests, CompleteClassIdentifiersSurviveSerializationAndHashCollisions)
{
    juce::PluginDescription first;
    first.fileOrIdentifier = "/plugins/shared.vst3";
    first.name = "Effect";
    first.uniqueId = 29791;
    first.deprecatedUid = 1;
    first.uniqueIdString = "00000001000000000000000000000000";
    juce::PluginDescription second = first;
    second.uniqueIdString = "000000000000001F0000000000000000";
    EXPECT_FALSE(first.isDuplicateOf(second));
    juce::KnownPluginList known;
    EXPECT_TRUE(known.addType(first));
    EXPECT_TRUE(known.addType(second));
    EXPECT_EQ(known.getNumTypes(), 2);
    const auto xml = first.createXml();
    juce::PluginDescription restored;
    ASSERT_TRUE(restored.loadFromXml(*xml));
    EXPECT_EQ(restored.uniqueIdString, first.uniqueIdString);
    EXPECT_TRUE(first.isDuplicateOf(restored));
    xml->removeAttribute("uniqueIdString");
    ASSERT_TRUE(restored.loadFromXml(*xml));
    EXPECT_TRUE(restored.uniqueIdString.isEmpty());
    EXPECT_TRUE(first.isDuplicateOf(restored));
}

#if AU_TRACKTION_TEST_VST3
TEST(PluginScannerTests, ValidatedDescriptionsAvoidScanningAndRefreshChangedPlugins)
{
    class Format final : public juce::VST3PluginFormat
    {
    public:
        void findAllTypesForFile(juce::OwnedArray<juce::PluginDescription>& result, const juce::String& path) override
        {
            ++scans;
            VST3PluginFormat::findAllTypesForFile(result, path);
        }

        int scans = 0;
    } format;
    const TracktionPluginMetaReader reader(PluginFormat::VST3);
    const auto metadata = reader.readMeta(AU_TRACKTION_TEST_VST3_PATH);
    ASSERT_TRUE(metadata.ret);
    ASSERT_EQ(metadata.val.size(), 1u);
    muse::audioplugins::AudioPluginInfo info { metadata.val.front(), AU_TRACKTION_TEST_VST3_PATH,
                                               muse::audioplugins::AudioPluginState::Validated };
    const auto xml = juce::parseXML(info.meta.attributeVal(TRACKTION_PLUGIN_DESCRIPTION_ATTRIBUTE).toStdString());
    ASSERT_TRUE(xml);
    juce::PluginDescription expected;
    ASSERT_TRUE(expected.loadFromXml(*xml));
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    TracktionRuntime runtime;
    runtime.init(directory.filePath("cache").toStdString(), directory.filePath("settings").toStdString());
    auto& manager = runtime.engine().getPluginManager();
    restoreTracktionPluginDescriptions(runtime.engine(), { info });
    auto actual = findTracktionPluginDescription(manager, format, AU_TRACKTION_TEST_VST3_PATH, expected.uniqueIdString);
    ASSERT_TRUE(actual);
    EXPECT_EQ(actual->uniqueIdString, expected.uniqueIdString);
    EXPECT_EQ(actual->numInputChannels, expected.numInputChannels);
    EXPECT_EQ(format.scans, 0);
    EXPECT_FALSE(findTracktionPluginDescription(manager, format, AU_TRACKTION_TEST_VST3_PATH,
                                                "00000000000000000000000000000001"));
    EXPECT_EQ(format.scans, 1);

    auto stale = expected;
    stale.name = "An old description";
    stale.lastFileModTime = {};
    info.meta.attributes[TRACKTION_PLUGIN_DESCRIPTION_ATTRIBUTE] = muse::String::fromStdString(stale.createXml()->toString().toStdString());
    restoreTracktionPluginDescriptions(runtime.engine(), { info });
    actual = findTracktionPluginDescription(manager, format, AU_TRACKTION_TEST_VST3_PATH, expected.uniqueIdString);
    ASSERT_TRUE(actual);
    EXPECT_EQ(actual->name, expected.name);
    EXPECT_EQ(format.scans, 2);
    actual = findTracktionPluginDescription(manager, format, AU_TRACKTION_TEST_VST3_PATH, expected.uniqueIdString);
    ASSERT_TRUE(actual);
    EXPECT_EQ(format.scans, 2);

    info.path = directory.filePath("missing.vst3").toStdString();
    restoreTracktionPluginDescriptions(runtime.engine(), { info });
    EXPECT_FALSE(findTracktionPluginDescription(manager, format, info.path.toStdString(), expected.uniqueIdString));
    EXPECT_EQ(format.scans, 3);
}

TEST(PluginScannerTests, DescriptionsFollowValidationStateAndTheCurrentBundlePath)
{
    const TracktionPluginMetaReader reader(PluginFormat::VST3);
    const auto metadata = reader.readMeta(AU_TRACKTION_TEST_VST3_PATH);
    ASSERT_TRUE(metadata.ret);
    ASSERT_EQ(metadata.val.size(), 1u);
    muse::audioplugins::AudioPluginInfo info { metadata.val.front(), "/new/application/plugin.vst3",
                                               muse::audioplugins::AudioPluginState::Validated };
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    TracktionRuntime runtime;
    runtime.init(directory.filePath("cache").toStdString(), directory.filePath("settings").toStdString());
    auto& known = runtime.engine().getPluginManager().knownPluginList;
    restoreTracktionPluginDescriptions(runtime.engine(), { info });
    ASSERT_EQ(known.getNumTypes(), 1);
    EXPECT_EQ(known.getTypes()[0].fileOrIdentifier.toStdString(), info.path.toStdString());
    for (const auto state : { muse::audioplugins::AudioPluginState::Discovered, muse::audioplugins::AudioPluginState::Missing,
                              muse::audioplugins::AudioPluginState::Error }) {
        info.state = state;
        restoreTracktionPluginDescriptions(runtime.engine(), { info });
        EXPECT_EQ(known.getNumTypes(), 0);
    }
    info.state = muse::audioplugins::AudioPluginState::Validated;
    auto invalid = info;
    invalid.meta.attributes[TRACKTION_PLUGIN_DESCRIPTION_ATTRIBUTE] = u"<PLUGIN";
    restoreTracktionPluginDescriptions(runtime.engine(), { invalid });
    EXPECT_EQ(known.getNumTypes(), 0);
    invalid = info;
    invalid.meta.attributes.erase(TRACKTION_PLUGIN_DESCRIPTION_ATTRIBUTE);
    restoreTracktionPluginDescriptions(runtime.engine(), { invalid });
    EXPECT_EQ(known.getNumTypes(), 0);
    invalid = info;
    invalid.meta.id += ";00000000000000000000000000000001";
    restoreTracktionPluginDescriptions(runtime.engine(), { invalid });
    EXPECT_EQ(known.getNumTypes(), 0);
    restoreTracktionPluginDescriptions(runtime.engine(), { info });
    ASSERT_EQ(known.getNumTypes(), 1);
    restoreTracktionPluginDescriptions(runtime.engine(), {});
    EXPECT_EQ(known.getNumTypes(), 0);
}

TEST(PluginScannerTests, ExternalProcessorUsesTheCompleteIdentifierBeforeTheHash)
{
    VST3EffectsModule provider;
    ASSERT_TRUE(provider.Initialize());
    PluginPath legacyPath;
    PluginID id;
    TranslatableString error;
    ASSERT_EQ(provider.DiscoverPluginsAtPath(wxString::FromUTF8(AU_TRACKTION_TEST_VST3_PATH), error,
                                             [&](PluginProvider*, ComponentInterface* component) -> const PluginID& {
        legacyPath = component->GetPath();
        return id;
    }), 1u);
    const auto component = provider.LoadPlugin(legacyPath);
    const auto* effect = dynamic_cast<const EffectInstanceFactory*>(component.get());
    ASSERT_TRUE(effect);
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    TracktionRuntime runtime;
    runtime.init(directory.filePath("cache").toStdString(), directory.filePath("settings").toStdString());
    auto& manager = runtime.engine().getPluginManager();
    juce::OwnedArray<juce::PluginDescription> descriptions;
    for (auto* format : manager.pluginFormatManager.getFormats()) {
        if (format->getName() == "VST3") {
            format->findAllTypesForFile(descriptions, AU_TRACKTION_TEST_VST3_PATH);
        }
    }
    ASSERT_EQ(descriptions.size(), 1);
    const auto expected = *descriptions[0];
    auto collision = expected;
    collision.name = "A different class";
    collision.uniqueIdString = "00000000000000000000000000000001";
    ASSERT_TRUE(manager.knownPluginList.addType(collision));
    ASSERT_TRUE(manager.knownPluginList.addType(expected));
    tracktion::Edit edit(runtime.engine(), tracktion::Edit::forRendering);
    tracktion::ExternalPlugin::Ptr plugin = createTracktionExternalProcessor(edit, *effect);
    ASSERT_TRUE(plugin->getAudioPluginInstance()) << plugin->getLoadError().toStdString();
    EXPECT_EQ(plugin->getAudioPluginInstance()->getPluginDescription().uniqueIdString, expected.uniqueIdString);
    EXPECT_EQ(plugin->desc.uniqueIdString, expected.uniqueIdString);
    EXPECT_EQ(plugin->state.getProperty("uniqueIdString").toString(), expected.uniqueIdString);
    provider.Terminate();
}

TEST(PluginScannerTests, Vst3ValidationKeepsAudacityMetadataAndCompleteClassIdentifiers)
{
    VST3EffectsModule provider;
    const TracktionPluginMetaReader reader(PluginFormat::VST3);
    for (const auto* path : { AU_TRACKTION_TEST_VST3_PATH, AU_TRACKTION_TEST_VST3_MONO_PATH,
                              AU_TRACKTION_TEST_VST3_STEREO_PATH, AU_TRACKTION_TEST_VST3_MONO_STEREO_PATH }) {
        SCOPED_TRACE(path);
        ASSERT_NO_FATAL_FAILURE(compareMetadata(provider, path, reader));
    }
}

TEST(PluginScannerTests, Vst3SearchIncludesCustomPathsWithoutDuplicatesAndCanCancel)
{
    class Scanner final : public TracktionPluginsScanner
    {
    public:
        Scanner()
            : TracktionPluginsScanner(PluginFormat::VST3) {}
        muse::io::paths_t customPaths() const override
        {
            const auto parent = juce::File(AU_TRACKTION_TEST_VST3_PATH).getParentDirectory().getFullPathName().toStdString();
            return { parent, parent };
        }
    } scanner;
    const auto paths = scanner.scanPlugins();
    EXPECT_EQ(std::count(paths.begin(), paths.end(), muse::io::path_t(AU_TRACKTION_TEST_VST3_PATH)), 1);
    muse::Progress progress;
    progress.cancel();
    EXPECT_TRUE(scanner.scanPlugins(&progress).empty());
    const TracktionPluginMetaReader reader(PluginFormat::VST3);
    EXPECT_FALSE(reader.canReadMeta("/plugins/invalid.vst"));
    EXPECT_FALSE(reader.readMeta("/plugins/missing.vst3").ret);
}
#endif

#if AU_TRACKTION_TEST_AUDIO_UNIT
TEST(PluginScannerTests, AudioUnitDescriptionsUseComponentIdentifiersAndRefreshChangedVersions)
{
    class Format final : public juce::AudioUnitPluginFormat
    {
    public:
        void findAllTypesForFile(juce::OwnedArray<juce::PluginDescription>& result, const juce::String& path) override
        {
            ++scans;
            AudioUnitPluginFormat::findAllTypesForFile(result, path);
        }

        int scans = 0;
    } format;
    const TracktionPluginMetaReader reader(PluginFormat::AudioUnit);
    const auto metadata = reader.readMeta("appl/aufx/lpas/Apple: AULowpass");
    ASSERT_TRUE(metadata.ret);
    ASSERT_EQ(metadata.val.size(), 1u);
    muse::audioplugins::AudioPluginInfo info { metadata.val.front(), "appl/aufx/lpas/Apple: AULowpass",
                                               muse::audioplugins::AudioPluginState::Validated };
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    TracktionRuntime runtime;
    runtime.init(directory.filePath("cache").toStdString(), directory.filePath("settings").toStdString());
    auto& manager = runtime.engine().getPluginManager();
    restoreTracktionPluginDescriptions(runtime.engine(), { info });
    ASSERT_EQ(manager.knownPluginList.getNumTypes(), 1);
    auto actual = findTracktionPluginDescription(manager, format, "AudioUnit:aufx,lpas,appl", {});
    ASSERT_TRUE(actual);
    EXPECT_EQ(format.scans, 0);
    EXPECT_TRUE(static_cast<const juce::AudioPluginFormat&>(format).requiresUnblockedMessageThreadDuringCreation(*actual));
    const auto version = actual->version;
    actual->version = "0.0.0";
    info.meta.attributes[TRACKTION_PLUGIN_DESCRIPTION_ATTRIBUTE]
        = muse::String::fromStdString(actual->createXml()->toString().toStdString());
    restoreTracktionPluginDescriptions(runtime.engine(), { info });
    actual = findTracktionPluginDescription(manager, format, "AudioUnit:aufx,lpas,appl", {});
    ASSERT_TRUE(actual);
    EXPECT_EQ(actual->version, version);
    EXPECT_EQ(format.scans, 0);
    info.path = "appl/aufx/lmtr/Apple: AUPeakLimiter";
    restoreTracktionPluginDescriptions(runtime.engine(), { info });
    EXPECT_EQ(manager.knownPluginList.getNumTypes(), 0);
}

TEST(PluginScannerTests, AudioUnitSearchAndValidationKeepProviderIdentifiersAndMetadata)
{
    ::AudioUnitEffectsModule provider;
    const TracktionPluginsScanner scanner(PluginFormat::AudioUnit);
    const auto paths = scanner.scanPlugins();
    const muse::io::path_t lowpass("appl/aufx/lpas/Apple: AULowpass");
    EXPECT_NE(std::find(paths.begin(), paths.end(), lowpass), paths.end());
    const TracktionPluginMetaReader reader(PluginFormat::AudioUnit);
    ASSERT_NO_FATAL_FAILURE(compareMetadata(provider, lowpass, reader));
    ASSERT_NO_FATAL_FAILURE(compareMetadata(provider, "appl/aufx/lmtr/Apple: AUPeakLimiter", reader));
    EXPECT_FALSE(reader.canReadMeta("aufx/lpas/appl"));
    EXPECT_FALSE(reader.readMeta("abcd/aufx/miss/Missing").ret);
}
#endif
}
