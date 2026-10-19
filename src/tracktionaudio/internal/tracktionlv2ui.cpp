/*
 * Audacity: A Digital Audio Editor
 */
#include <tracktion_engine/tracktion_engine.h>

#include "tracktionlv2ui.h"

#include <QLibrary>
#include <cstdarg>
#include <cstdio>
#include <cstring>

#include <lv2/ui/ui.h>
#include <lv2/urid/urid.h>
#include <lv2/uri-map/uri-map.h>
#include <lv2/data-access/data-access.h>
#include <lv2/instance-access/instance-access.h>
#include <lv2/options/options.h>
#include <lv2/parameters/parameters.h>
#include <lv2/atom/atom.h>
#include <lv2/log/log.h>
#include "au3-lv2/lv2_external_ui.h"

namespace au::tracktionaudio {
namespace {
using Client = juce::AudioPluginExtensions::LV2Client;
struct Selection {
    Client::UiDescription description;
    juce::String type;
    bool external = false;
};

std::optional<Selection> selectUi(const juce::AudioPluginInstance& processor)
{
    const auto* client = processor.getLV2Client();
    if (!client) {
        return {};
    }
    const auto uis = client->getUis();
    for (const auto& ui : uis) {
        for (const auto* type : { LV2_UI_PREFIX "Qt6UI", LV2_UI__X11UI }) {
            if (ui.types.contains(type)) {
                return Selection { ui, type, false };
            }
        }
    }
    for (const auto& ui : uis) {
        if (ui.types.contains(LV2_EXTERNAL_UI__Widget) || ui.types.contains(LV2_EXTERNAL_UI_DEPRECATED_URI)) {
            return Selection { ui, LV2_EXTERNAL_UI__Widget, true };
        }
    }
    return {};
}
}

struct TracktionLv2Ui::Data {
    Data(juce::AudioPluginInstance& value, Callbacks handlers)
        : processor(value), callbacks(std::move(handlers)) {}

    bool open()
    {
        if (connection) {
            return handle != nullptr;
        }
        auto* client = processor.getLV2Client();
        selection = selectUi(processor);
        if (!client || !selection) {
            return false;
        }
        connection = client->createUiConnection({ [this]() { closeView(); }, [this]() {
                if (!openView() && callbacks.failed) {
                    const auto callback = callbacks.failed;
                    callback();
                }
            } });
        if (!connection || !openView()) {
            close();
            return false;
        }
        return true;
    }

    void closeView()
    {
        closing = true;
        if (handle) {
            if (externalWidget && externalWidget->hide) {
                externalWidget->hide(externalWidget);
            } else if (show && show->hide) {
                show->hide(handle);
            }
            descriptor->cleanup(handle);
        }
        handle = nullptr;
        widget = nullptr;
        externalWidget = nullptr;
        idleInterface = nullptr;
        show = nullptr;
        descriptor = nullptr;
        features.clear();
        featurePointers.clear();
        nativeFeatures.clear();
        closing = false;
    }

    void close()
    {
        closeView();
        if (connection) {
            connection->disconnect();
            connection.reset();
        }
    }

    void addFeature(const char* uri, void* data)
    {
        const auto found = std::find_if(features.begin(), features.end(), [uri](const auto& feature) {
            return std::strcmp(feature.URI, uri) == 0;
        });
        if (found != features.end()) {
            found->data = data;
        } else {
            features.push_back({ uri, data });
        }
    }

    bool openView()
    {
        closing = false;
        closed = false;
        if (!library.isLoaded()) {
            library.setLoadHints(QLibrary::ResolveAllSymbolsHint | QLibrary::ExportExternalSymbolsHint | QLibrary::PreventUnloadHint);
            library.setFileName(QString::fromStdString(selection->description.binaryPath.toStdString()));
        }
        const auto descriptors = reinterpret_cast<LV2UI_DescriptorFunction>(library.resolve("lv2ui_descriptor"));
        if (!descriptors) {
            return false;
        }
        for (uint32_t index = 0; const auto* candidate = descriptors(index); ++index) {
            if (selection->description.uri == candidate->URI) {
                descriptor = candidate;
                break;
            }
        }
        if (!descriptor || !descriptor->instantiate || !descriptor->cleanup) {
            return false;
        }
        nativeFeatures = connection->getFeatures();
        features.clear();
        for (auto& feature : nativeFeatures) {
            features.push_back({ feature.uri.toRawUTF8(), feature.data });
        }
        const auto mapped = std::find_if(nativeFeatures.begin(), nativeFeatures.end(), [](const auto& feature) {
            return feature.uri == LV2_URID__map;
        });
        map = mapped != nativeFeatures.end() ? static_cast<LV2_URID_Map*>(mapped->data) : nullptr;
        dataAccess.data_access = connection->getExtensionDataFunction();
        title = processor.getName().toStdString();
        externalHost.plugin_human_id = title.c_str();
        addFeature(LV2_UI__resize, &resize);
        addFeature(LV2_UI__portMap, &portMap);
        addFeature(LV2_DATA_ACCESS_URI, &dataAccess);
        addFeature(LV2_INSTANCE_ACCESS_URI, connection->getInstanceHandle());
        addFeature(LV2_UI__parent, nullptr);
        addFeature(LV2_UI__idleInterface, nullptr);
        addFeature(LV2_UI__noUserResize, nullptr);
        addFeature(LV2_UI__fixedSize, nullptr);
        addFeature(LV2_UI_PREFIX "makeResident", nullptr);
        addFeature(LV2_EXTERNAL_UI__Host, &externalHost);
        addFeature(LV2_EXTERNAL_UI_DEPRECATED_URI, &externalHost);
        addFeature(LV2_EXTERNAL_UI__Widget, nullptr);
        addFeature(LV2_URI_MAP_URI, &legacyMap);
        addFeature(LV2_LOG__log, &log);
        options.clear();
        const auto configured = std::find_if(nativeFeatures.begin(), nativeFeatures.end(), [](const auto& feature) {
            return feature.uri == LV2_OPTIONS__options;
        });
        if (configured != nativeFeatures.end()) {
            for (auto* option = static_cast<const LV2_Options_Option*>(configured->data); option && option->key; ++option) {
                options.push_back(*option);
            }
        }
        if (map) {
            sampleRate = processor.getLV2Client()->getInstanceSampleRate();
            const auto floatType = map->map(map->handle, LV2_ATOM__Float);
            options.push_back({ LV2_OPTIONS_INSTANCE, 0, map->map(map->handle, LV2_PARAMETERS__sampleRate),
                                sizeof(float), floatType, &sampleRate });
            options.push_back({ LV2_OPTIONS_INSTANCE, 0, map->map(map->handle, LV2_UI__scaleFactor),
                                sizeof(float), floatType, &scaleFactor });
        }
        options.push_back({});
        addFeature(LV2_OPTIONS__options, options.data());
        for (const auto& required : selection->description.requiredFeatures) {
            if (std::none_of(features.begin(), features.end(), [&](const auto& feature) { return required == feature.URI; })) {
                return false;
            }
        }
        featurePointers.clear();
        for (const auto& feature : features) {
            featurePointers.push_back(&feature);
        }
        featurePointers.push_back(nullptr);
        const auto bundle = juce::File::addTrailingSeparator(selection->description.bundlePath).toStdString();
        const auto uri = processor.getPluginDescription().fileOrIdentifier.toStdString();
        handle = descriptor->instantiate(descriptor, uri.c_str(), bundle.c_str(), write, this, &widget, featurePointers.data());
        if (!handle || !widget) {
            closeView();
            return false;
        }
        if (selection->external) {
            externalWidget = static_cast<LV2_External_UI_Widget*>(widget);
            if (!externalWidget->run || !externalWidget->show || !externalWidget->hide) {
                closeView();
                return false;
            }
            externalWidget->show(externalWidget);
        } else {
            idleInterface = static_cast<const LV2UI_Idle_Interface*>(extension(LV2_UI__idleInterface));
            show = static_cast<const LV2UI_Show_Interface*>(extension(LV2_UI__showInterface));
            if (!idleInterface || !idleInterface->idle) {
                closeView();
                return false;
            }
            if (show && show->show) {
                show->show(handle);
            }
        }
        if (!closed && selection->type == LV2_UI__X11UI && callbacks.x11WindowReady) {
            callbacks.x11WindowReady(widget);
        }
        return true;
    }

    const void* extension(const char* uri) const
    {
        return descriptor && descriptor->extension_data ? descriptor->extension_data(uri) : nullptr;
    }

    bool idle()
    {
        if (!handle || closed) {
            return false;
        }
        const auto active = connection;
        active->dispatchEvents([this](uint32_t port, uint32_t size, uint32_t protocol, const void* data) {
            if (handle && !closed && descriptor->port_event) {
                descriptor->port_event(handle, port, size, protocol, data);
            }
        });
        if (closed) {
            return false;
        }
        if (externalWidget) {
            externalWidget->run(externalWidget);
        } else if (idleInterface && idleInterface->idle(handle)) {
            closed = true;
        }
        return !closed;
    }

    static void write(LV2UI_Controller controller, uint32_t port, uint32_t size, uint32_t protocol, const void* data)
    {
        auto& self = *static_cast<Data*>(controller);
        if (self.connection) {
            self.connection->writePort(port, size, protocol, data);
        }
    }

    static int resizeUi(LV2UI_Feature_Handle controller, int width, int height)
    {
        auto& self = *static_cast<Data*>(controller);
        return self.callbacks.resize ? self.callbacks.resize(width, height) : 0;
    }

    static uint32_t portIndex(LV2UI_Feature_Handle controller, const char* symbol)
    {
        const auto& self = *static_cast<Data*>(controller);
        return self.connection ? self.connection->getPortIndex(juce::String::fromUTF8(symbol)) : LV2UI_INVALID_PORT_INDEX;
    }

    static void uiClosed(LV2UI_Controller controller)
    {
        auto& self = *static_cast<Data*>(controller);
        if (!self.closing) {
            self.closed = true;
        }
    }

    static uint32_t mapUri(LV2_URI_Map_Callback_Data controller, const char*, const char* uri)
    {
        const auto& self = *static_cast<Data*>(controller);
        return self.map ? self.map->map(self.map->handle, uri) : 0;
    }

    static int printLog(LV2_Log_Handle, LV2_URID, const char* format, va_list arguments)
    {
        return std::vfprintf(stderr, format, arguments);
    }

    static int printLog(LV2_Log_Handle handle, LV2_URID type, const char* format, ...)
    {
        va_list arguments;
        va_start(arguments, format);
        const auto result = printLog(handle, type, format, arguments);
        va_end(arguments);
        return result;
    }

    juce::AudioPluginInstance& processor;
    Callbacks callbacks;
    std::optional<Selection> selection;
    std::shared_ptr<Client::UiConnection> connection;
    QLibrary library;
    const LV2UI_Descriptor* descriptor = nullptr;
    LV2UI_Handle handle = nullptr;
    LV2UI_Widget widget = nullptr;
    LV2_External_UI_Widget* externalWidget = nullptr;
    const LV2UI_Idle_Interface* idleInterface = nullptr;
    const LV2UI_Show_Interface* show = nullptr;
    std::vector<Client::UiFeature> nativeFeatures;
    std::vector<LV2_Feature> features;
    std::vector<const LV2_Feature*> featurePointers;
    LV2_URID_Map* map = nullptr;
    LV2UI_Resize resize { this, resizeUi };
    LV2UI_Port_Map portMap { this, portIndex };
    LV2_Extension_Data_Feature dataAccess {};
    LV2_External_UI_Host externalHost { uiClosed, nullptr };
    LV2_URI_Map_Feature legacyMap { this, mapUri };
    LV2_Log_Log log { this, printLog, printLog };
    std::vector<LV2_Options_Option> options;
    float sampleRate = 44100.0f;
    float scaleFactor = 1.0f;
    std::string title;
    bool closing = false;
    bool closed = false;
};

bool TracktionLv2Ui::isSupported(const juce::AudioPluginInstance& processor)
{
    return selectUi(processor).has_value();
}

TracktionLv2Ui::TracktionLv2Ui(juce::AudioPluginInstance& processor, Callbacks callbacks)
    : m_data(std::make_unique<Data>(processor, std::move(callbacks))) {}

TracktionLv2Ui::~TracktionLv2Ui() { close(); }
bool TracktionLv2Ui::open() { return m_data->open(); }
void TracktionLv2Ui::close() { m_data->close(); }
bool TracktionLv2Ui::idle() { return m_data->idle(); }
}
