/*
* Audacity: A Digital Audio Editor
*/
#include <tracktion_engine/tracktion_engine.h>

#include "tracktionpluginscanner.h"
#include "tracktionplugindescriptions.h"
#include "tracktionruntime.h"
#include "tracktionpluginloader.h"

#include <stdexcept>

#include <QCoreApplication>
#include <QDirIterator>
#include <QEventLoop>
#include <QRegularExpression>
#include <QTemporaryDir>
#include <QUrl>

#include "effects/effects_base/internal/effectsutils.h"
#include "effects/effects_base/effecterrors.h"
#include "effects/vst/internal/vsttypes.h"
#include "effects/audio_unit/internal/audiounittypes.h"
#include "effects/lv2/internal/lv2types.h"
#include "global/progress.h"
#include "global/defer.h"
#include "global/log.h"

#if JUCE_MAC
#include <AudioToolbox/AudioToolbox.h>
#include <CoreFoundation/CoreFoundation.h>
#endif

using namespace au::tracktionaudio;
namespace effects = au::effects;

namespace {
struct ScanRuntime {
    ScanRuntime()
    {
        if (!directory.isValid()) {
            throw std::runtime_error("Cannot create the plug-in scan directory.");
        }
        runtime.init(directory.filePath("cache").toStdString(), directory.filePath("settings").toStdString());
    }

    QTemporaryDir directory;
    TracktionRuntime runtime;
};

juce::String formatName(PluginFormat format)
{
    switch (format) {
    case PluginFormat::VST3: return "VST3";
    case PluginFormat::AudioUnit: return "AudioUnit";
    case PluginFormat::LV2: return "LV2";
    }
    return {};
}

juce::AudioPluginFormat* findFormat(TracktionRuntime& runtime, PluginFormat format)
{
    for (auto* candidate : runtime.engine().getPluginManager().pluginFormatManager.getFormats()) {
        if (candidate->getName() == formatName(format)) {
            return candidate;
        }
    }
    return nullptr;
}

bool pollScan(muse::Progress* progress, int64_t current, int64_t total, const std::string& path)
{
    if (progress) {
        progress->progress(current, std::max(current, total), path);
        QCoreApplication::processEvents(QEventLoop::AllEvents);
        return !progress->isCanceled();
    }
    return true;
}

#if JUCE_MAC
juce::String audioUnitPath(const juce::String& identifier)
{
    const auto fields = juce::StringArray::fromTokens(identifier.fromLastOccurrenceOf("/", false, false)
                                                      .fromLastOccurrenceOf(":", false, false), ",", "");
    if (fields.size() != 3 || fields[0].length() != 4 || fields[1].length() != 4 || fields[2].length() != 4) {
        return {};
    }
    const auto code = [](const juce::String& value) {
        const auto* bytes = reinterpret_cast<const unsigned char*>(value.toRawUTF8());
        return static_cast<OSType>((uint32_t(bytes[0]) << 24) | (uint32_t(bytes[1]) << 16)
                                   | (uint32_t(bytes[2]) << 8) | bytes[3]);
    };
    AudioComponentDescription description {};
    description.componentType = code(fields[0]);
    description.componentSubType = code(fields[1]);
    description.componentManufacturer = code(fields[2]);
    const auto component = AudioComponentFindNext(nullptr, &description);
    CFStringRef name = nullptr;
    if (!component || AudioComponentCopyName(component, &name) != noErr || !name) {
        return {};
    }
    const auto fullName = juce::String::fromCFString(name);
    CFRelease(name);
    return fields[2] + "/" + fields[0] + "/" + fields[1] + "/" + fullName;
}

bool isSupportedAudioUnit(const juce::String& path)
{
    const auto type = path.substring(5, 9);
    const auto allowed = type == "aumu" || type == "aufx" || type == "augn" || type == "aumx" || type == "aumf" || type == "aupn";
    const juce::StringArray excluded {
        "appl/augn/afpl", "appl/augn/sspl", "appl/augn/ttsp", "appl/augn/nrcv", "appl/aumx/3dmx",
        "appl/aumx/mspl", "appl/aumx/mcmx", "appl/aumx/mxmx", "appl/aumx/smxr", "Ignt/aufx/PTQX"
    };
    return allowed && !excluded.contains(path.substring(0, 14));
}

#endif

juce::String nativeIdentifier(PluginFormat format, const muse::io::path_t& path)
{
    if (format != PluginFormat::AudioUnit) {
        return path.toStdString();
    }
    const auto fields = juce::StringArray::fromTokens(juce::String(path.toStdString()), "/", "");
    return "AudioUnit:" + fields[1] + "," + fields[2] + "," + fields[0];
}

effects::EffectMeta effectMeta(PluginFormat format, const muse::io::path_t& path, const juce::PluginDescription& description)
{
    effects::EffectMeta meta;
    meta.path = path;
    meta.title = muse::String::fromStdString(description.descriptiveName.toStdString());
    meta.vendor = muse::String::fromStdString(description.manufacturerName.toStdString());
    meta.version = muse::String::fromStdString(description.version.toStdString());
    if (format == PluginFormat::VST3) {
        meta.family = effects::EffectFamily::VST3;
        meta.path = path.toStdString() + ";" + description.uniqueIdString.toStdString();
        const auto categories = juce::StringArray::fromTokens(description.category, "|", "");
        meta.type = description.category == "Fx|Generator" ? effects::EffectType::Generator
                    : categories.contains("Fx") ? effects::EffectType::Processor : effects::EffectType::Unknown;
        meta.module = u"Module__The Audacity Team_VST3 Effects_";
        meta.description = muse::qtrc("vst3", "SubCategories: %1").arg(QString::fromStdString(description.category.toStdString()));
    } else if (format == PluginFormat::LV2) {
        meta.family = effects::EffectFamily::LV2;
        if (meta.vendor.empty()) {
            meta.vendor = u"n/a";
        }
        meta.version = u"1.0";
        meta.type = description.numInputChannels == 0 && description.numOutputChannels == 0 ? effects::EffectType::Tool
                    : description.numInputChannels == 0 ? effects::EffectType::Generator
                    : description.numOutputChannels == 0 ? effects::EffectType::Analyzer : effects::EffectType::Processor;
        meta.module = u"Module__The Audacity Team_LV2 Effects_";
        meta.description = muse::qtrc("lv2", "n/a");
#if JUCE_MAC
    } else {
        meta.family = effects::EffectFamily::AudioUnit;
        const auto fullName = juce::String(path.toStdString()).substring(15).upToFirstOccurrenceOf("/", false, false);
        const auto separator = fullName.indexOfChar(':');
        meta.title = muse::String::fromStdString((separator < 0 ? fullName : fullName.substring(separator + 1)).trim().toStdString());
        meta.vendor = muse::String::fromStdString((separator < 0 ? fullName : fullName.substring(0, separator)).trim().toStdString());
        meta.type = description.numInputChannels == 0 && description.numOutputChannels == 0 ? effects::EffectType::Unknown
                    : description.numInputChannels == 0 ? effects::EffectType::Generator
                    : description.numOutputChannels == 0 ? effects::EffectType::Analyzer : effects::EffectType::Processor;
        meta.module = u"Module__The Audacity Team_Audio Unit Effects_";
        meta.description = muse::qtrc("audio-unit", "n/a");
#endif
    }
    if (description.isInstrument) {
        meta.type = effects::EffectType::Instrument;
    }
    meta.id = effects::utils::effectId(effects::utils::effectFamilyToString(meta.family), meta.vendor, meta.title, meta.path);
    meta.category = effects::utils::effectCategoryToString(effects::EffectCategory::Unspecified);
    meta.isRealtimeCapable = meta.type == effects::EffectType::Processor;
    meta.paramsAreInputAgnostic = meta.type != effects::EffectType::Generator;
    return meta;
}
}

TracktionPluginsScanner::TracktionPluginsScanner(PluginFormat format)
    : m_format(format)
{
}

muse::io::paths_t TracktionPluginsScanner::scanPlugins(muse::Progress* progress) const
{
    muse::io::paths_t result;
    try {
        ScanRuntime scan;
        auto* format = findFormat(scan.runtime, m_format);
        if (!format || !pollScan(progress, 0, 1, {})) {
            return result;
        }
        if (m_format == PluginFormat::VST3) {
            auto roots = format->getDefaultLocationsToSearch();
#if JUCE_MAC
            roots.add(juce::File("/Network/Library/Audio/Plug-ins/VST3"));
#endif
#if JUCE_LINUX
            const auto applicationFolder = QCoreApplication::applicationDirPath() + "/vst3";
#else
            const auto applicationFolder = QCoreApplication::applicationDirPath() + "/VST3";
#endif
            roots.add(juce::File(applicationFolder.toStdString()));
            for (const auto& path : customPaths()) {
                if (!path.empty()) {
                    roots.add(juce::File(path.toStdString()));
                }
            }
            roots.removeRedundantPaths();
            juce::StringArray found;
            int64_t visited = 0;
            const auto scanDirectory = [&](const QString& path) {
                if (!pollScan(progress, visited++, roots.getNumPaths(), path.toStdString())) {
                    return false;
                }
                found.addArray(format->searchPathsForPlugins(juce::FileSearchPath(path.toStdString()), false, true));
                return true;
            };
            for (int root = 0; root < roots.getNumPaths(); ++root) {
                const auto directory = QString::fromStdString(roots[root].getFullPathName().toStdString());
                if (!scanDirectory(directory)) {
                    break;
                }
                QDirIterator directories(directory, QDir::Dirs | QDir::NoDotAndDotDot, QDirIterator::Subdirectories);
                while (directories.hasNext()) {
                    const auto path = directories.next();
                    if (!path.contains(".vst3/", Qt::CaseInsensitive) && !path.endsWith(".vst3", Qt::CaseInsensitive)) {
                        if (!scanDirectory(path)) {
                            break;
                        }
                    }
                }
                if (progress && progress->isCanceled()) {
                    break;
                }
            }
            found.removeDuplicates(false);
            for (const auto& path : found) {
                result.emplace_back(path.toStdString());
            }
        } else if (m_format == PluginFormat::LV2) {
#if JUCE_PLUGINHOST_LV2
            auto roots = format->getDefaultLocationsToSearch();
            juce::StringArray bundles;
            for (const auto& path : customPaths()) {
                if (!path.empty()) {
                    if (juce::String(path.toStdString()).endsWith(".lv2")) {
                        bundles.add(path.toStdString());
                    } else {
                        roots.add(juce::File(path.toStdString()));
                    }
                }
            }
            roots.removeRedundantPaths();
            bundles.addArray(format->searchPathsForPlugins(roots, false, true));
            bundles.removeDuplicates(false);
            juce::StringArray identifiers;
            auto& lv2 = static_cast<juce::LV2PluginFormatHeadless&>(*format);
            const juce::StringArray excluded {
                "http://lv2plug.in/ns/lv2core#MIDIPlugin",
                "http://ll-plugins.nongnu.org/lv2/namespace#MathConstantPlugin",
                "http://ll-plugins.nongnu.org/lv2/namespace#MathFunctionPlugin"
            };
            for (int index = 0; index < bundles.size(); ++index) {
                if (!pollScan(progress, index, bundles.size(), bundles[index].toStdString())) {
                    break;
                }
                juce::OwnedArray<juce::PluginDescription> descriptions;
                format->findAllTypesForFile(descriptions, bundles[index]);
                for (const auto* description : descriptions) {
                    const auto metadata = lv2.getPluginMetadata(description->fileOrIdentifier);
                    if (!metadata || metadata->numPorts == 0 || description->name.isEmpty() || excluded.contains(metadata->classUri)) {
                        continue;
                    }
                    identifiers.add(Lv2PluginIdentifier { description->fileOrIdentifier.toStdString(), metadata->bundleUri.toStdString() }
                                    .serialise());
                }
            }
            identifiers.removeDuplicates(false);
            for (const auto& identifier : identifiers) {
                result.emplace_back(identifier.toStdString());
            }
#endif
        } else {
#if JUCE_MAC
            const auto identifiers = format->searchPathsForPlugins({}, false, true);
            for (int index = 0; index < identifiers.size(); ++index) {
                if (!pollScan(progress, index, identifiers.size(), identifiers[index].toStdString())) {
                    break;
                }
                const auto path = audioUnitPath(identifiers[index]);
                if (isSupportedAudioUnit(path)) {
                    result.emplace_back(path.toStdString());
                }
            }
#endif
        }
    } catch (const std::exception& error) {
        LOGE() << error.what();
    }
    return result;
}

TracktionPluginMetaReader::TracktionPluginMetaReader(PluginFormat format)
    : m_format(format)
{
}

muse::audioplugins::PluginType TracktionPluginMetaReader::metaType() const
{
    switch (m_format) {
    case PluginFormat::VST3: return std::string(effects::vst::AUDIO_RESOURCE_TYPE_NAME);
    case PluginFormat::AudioUnit: return std::string(effects::audio_unit::AUDIO_RESOURCE_TYPE_NAME);
    case PluginFormat::LV2: return std::string(effects::lv2::AUDIO_RESOURCE_TYPE_NAME);
    }
    return {};
}

bool TracktionPluginMetaReader::canReadMeta(const muse::io::path_t& path) const
{
    if (m_format == PluginFormat::VST3) {
        return juce::String(path.toStdString()).endsWithIgnoreCase(".vst3");
    }
    if (m_format == PluginFormat::LV2) {
        return JUCE_PLUGINHOST_LV2 && Lv2PluginIdentifier::parse(path.toStdString()).has_value();
    }
#if JUCE_MAC
    static const QRegularExpression identifier("^[^/]{4}/[^/]{4}/[^/]{4}/.+$");
    return identifier.match(QString::fromStdString(path.toStdString())).hasMatch();
#else
    return false;
#endif
}

muse::RetVal<muse::audioplugins::PluginMetaList> TracktionPluginMetaReader::readMeta(const muse::io::path_t& path) const
{
    using Result = muse::RetVal<muse::audioplugins::PluginMetaList>;
    if (!canReadMeta(path)) {
        return effects::make_ret(effects::Err::EffectLoadFailed);
    }
    try {
        ScanRuntime scan;
        auto* format = findFormat(scan.runtime, m_format);
        if (!format) {
            return effects::make_ret(effects::Err::EffectLoadFailed);
        }
        juce::OwnedArray<juce::PluginDescription> descriptions;
        const auto identifier = nativeIdentifier(m_format, path);
        if (m_format == PluginFormat::LV2) {
            const auto description = findTracktionPluginDescription(scan.runtime.engine().getPluginManager(), *format, identifier, {});
            if (description) {
                descriptions.add(new juce::PluginDescription(*description));
            }
        } else if (m_format == PluginFormat::AudioUnit) {
            auto request = std::make_unique<juce::PluginDescription>();
            request->pluginFormatName = format->getName();
            request->fileOrIdentifier = identifier;
            descriptions.add(request.release());
        } else {
            format->findAllTypesForFile(descriptions, identifier);
        }
        muse::audioplugins::PluginMetaList result;
        for (const auto* description : descriptions) {
            juce::String error;
            auto processor = loadTracktionPlugin(scan.runtime.engine().getPluginManager().pluginFormatManager, *description, error);
            if (!processor) {
                LOGW() << "Tracktion cannot load the plug-in: " << error.toStdString();
                continue;
            }
            processor->prepareToPlay(44100.0, 512);
            DEFER { processor->releaseResources();
            };
            auto validated = processor->getPluginDescription();
#if JUCE_PLUGINHOST_LV2
            if (m_format == PluginFormat::LV2) {
                const auto metadata = static_cast<juce::LV2PluginFormatHeadless&>(*format).getPluginMetadata(validated.fileOrIdentifier);
                validated.isInstrument = (metadata && metadata->classUri == "http://lv2plug.in/ns/lv2core#InstrumentPlugin")
                                         || (validated.isInstrument && processor->acceptsMidi());
            }
#endif
            if (m_format == PluginFormat::VST3 && validated.uniqueIdString.isEmpty()) {
                return effects::make_ret(effects::Err::EffectLoadFailed, "The VST3 class identifier is missing.");
            }
            auto meta = effects::utils::auToMuseEffectMeta(effectMeta(m_format, path, validated));
            meta.attributes.emplace(TRACKTION_PLUGIN_DESCRIPTION_ATTRIBUTE,
                                    muse::String::fromStdString(validated.createXml()->toString().toStdString()));
            result.push_back(std::move(meta));
        }
        return result.empty() ? Result(effects::make_ret(effects::Err::EffectLoadFailed)) : Result::make_ok(result);
    } catch (const std::exception& error) {
        return effects::make_ret(effects::Err::EffectLoadFailed, error.what());
    }
}
