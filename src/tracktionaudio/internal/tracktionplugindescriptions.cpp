/*
* Audacity: A Digital Audio Editor
*/
#include <tracktion_engine/tracktion_engine.h>

#include "tracktionplugindescriptions.h"
#include "tracktionpluginloader.h"

#include <QUrl>
#include <QDir>

#include "effects/effects_base/internal/effectsutils.h"
#include "effects/vst/internal/vsttypes.h"
#include "effects/audio_unit/internal/audiounittypes.h"
#include "effects/lv2/internal/lv2types.h"

using namespace au::tracktionaudio;

namespace {
QUrl lv2BundleUrl(const std::string& uri)
{
    // The saved Lilv file URI uses two percent signs for a literal percent sign.
    const auto encoded = QString::fromStdString(uri).replace("%%", "%25");
    return QUrl(encoded, QUrl::StrictMode);
}

juce::String audioUnitComponent(const juce::String& identifier)
{
    return identifier.fromLastOccurrenceOf("/", false, false).fromLastOccurrenceOf(":", false, false);
}

bool matches(const juce::PluginDescription& description, const juce::String& format, const juce::String& identifier,
             const juce::String& classID)
{
    if (description.pluginFormatName != format) {
        return false;
    }
    if (format == "AudioUnit") {
        return audioUnitComponent(description.fileOrIdentifier) == audioUnitComponent(identifier);
    }
    return description.fileOrIdentifier == identifier && description.uniqueIdString == classID;
}
}

std::string Lv2PluginIdentifier::serialise() const
{
    return pluginUri + "@" + bundleUri;
}

std::optional<Lv2PluginIdentifier> Lv2PluginIdentifier::parse(const std::string& identifier)
{
    const auto separator = identifier.find('@');
    if (separator == std::string::npos) {
        return std::nullopt;
    }
    Lv2PluginIdentifier result { identifier.substr(0, separator), identifier.substr(separator + 1) };
    const QUrl plugin(QString::fromStdString(result.pluginUri), QUrl::StrictMode);
    const auto bundle = lv2BundleUrl(result.bundleUri);
    if (!plugin.isValid() || plugin.scheme().isEmpty() || !bundle.isValid() || !bundle.isLocalFile()
        || !QDir::isAbsolutePath(bundle.toLocalFile())) {
        return std::nullopt;
    }
    return result;
}

void au::tracktionaudio::restoreTracktionPluginDescriptions(tracktion::Engine& engine,
                                                            const muse::audioplugins::AudioPluginInfoList& plugins)
{
    auto& known = engine.getPluginManager().knownPluginList;
    known.clear();
    for (const auto& info : plugins) {
        if (info.state != muse::audioplugins::AudioPluginState::Validated) {
            continue;
        }
        const bool vst3 = info.meta.type == au::effects::vst::AUDIO_RESOURCE_TYPE_NAME;
        const bool audioUnit = info.meta.type == au::effects::audio_unit::AUDIO_RESOURCE_TYPE_NAME;
        const bool lv2 = info.meta.type == au::effects::lv2::AUDIO_RESOURCE_TYPE_NAME;
        if (!vst3 && !audioUnit && !lv2) {
            continue;
        }
        const auto xml = juce::parseXML(info.meta.attributeVal(TRACKTION_PLUGIN_DESCRIPTION_ATTRIBUTE).toStdString());
        juce::PluginDescription description;
        if (!xml || !description.loadFromXml(*xml)) {
            continue;
        }
        if (vst3) {
            const auto effectPath = juce::String(au::effects::utils::parseEffectPath(muse::String::fromStdString(info.meta.id)));
            const auto classID = effectPath.fromLastOccurrenceOf(";", false, false);
            if (description.pluginFormatName != "VST3" || classID.length() != 32
                || !classID.containsOnly("0123456789abcdefABCDEF") || description.uniqueIdString != classID) {
                continue;
            }
            // The application can move after the description is saved.
            description.fileOrIdentifier = info.path.toStdString();
        } else if (lv2) {
            const auto identifier = Lv2PluginIdentifier::parse(info.path.toStdString());
            if (!identifier || description.pluginFormatName != "LV2"
                || description.fileOrIdentifier.toStdString() != identifier->pluginUri) {
                continue;
            }
        } else {
            const auto fields = juce::StringArray::fromTokens(juce::String(info.path.toStdString()), "/", "");
            if (fields.size() < 4 || fields[0].length() != 4 || fields[1].length() != 4 || fields[2].length() != 4
                || !matches(description, "AudioUnit", fields[1] + "," + fields[2] + "," + fields[0], {})) {
                continue;
            }
        }
        known.addType(description);
    }
}

std::optional<juce::PluginDescription> au::tracktionaudio::findTracktionPluginDescription(tracktion::PluginManager& manager,
                                                                                          juce::AudioPluginFormat& format,
                                                                                          const juce::String& identifier,
                                                                                          const juce::String& classID)
{
    if (format.getName() == "LV2") {
#if JUCE_PLUGINHOST_LV2
        const auto path = Lv2PluginIdentifier::parse(identifier.toStdString());
        if (!path) {
            return std::nullopt;
        }
        const auto bundle = lv2BundleUrl(path->bundleUri).toLocalFile().toStdString();
        juce::OwnedArray<juce::PluginDescription> descriptions;
        format.findAllTypesForFile(descriptions, bundle);
        const auto metadata = static_cast<juce::LV2PluginFormatHeadless&>(format).getPluginMetadata(path->pluginUri);
        if (!metadata || metadata->bundleUri.toStdString() != path->bundleUri) {
            return std::nullopt;
        }
        for (const auto* description : descriptions) {
            if (description->fileOrIdentifier.toStdString() == path->pluginUri) {
                manager.knownPluginList.addType(*description);
                return *description;
            }
        }
#endif
        return std::nullopt;
    }
    for (const auto& description : manager.knownPluginList.getTypes()) {
        if (matches(description, format.getName(), identifier, classID)) {
            if (format.doesPluginStillExist(description) && !format.pluginNeedsRescanning(description)) {
                return description;
            }
            manager.knownPluginList.removeType(description);
        }
    }
    if (format.getName() == "AudioUnit") {
        juce::PluginDescription request;
        request.pluginFormatName = format.getName();
        request.fileOrIdentifier = identifier;
        juce::String error;
        const auto instance = loadTracktionPlugin(manager.pluginFormatManager, request, error);
        if (instance) {
            auto description = instance->getPluginDescription();
            manager.knownPluginList.addType(description);
            return description;
        }
        return std::nullopt;
    }
    juce::OwnedArray<juce::PluginDescription> descriptions;
    format.findAllTypesForFile(descriptions, identifier);
    for (const auto* description : descriptions) {
        if (description->pluginFormatName == format.getName()
            && (format.getName() != "VST3" || description->uniqueIdString == classID)) {
            manager.knownPluginList.addType(*description);
            return *description;
        }
    }
    return std::nullopt;
}
