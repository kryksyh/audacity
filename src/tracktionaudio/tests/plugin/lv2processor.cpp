/*
* Audacity: A Digital Audio Editor
*/
#include <juce_audio_processors_headless/format_types/LV2_SDK/lv2/lv2/core/lv2.h>

#include <array>
#include <algorithm>
#include <cstring>
#include <atomic>
#include <chrono>
#include <thread>
#include "lv2workerprobe.h"

#include <lv2/state/state.h>
#include <lv2/urid/urid.h>
#include <lv2/atom/atom.h>
#include <lv2/atom/util.h>
#include <lv2/worker/worker.h>

namespace {
struct Processor {
    std::array<float*, 12> ports {};
    double sampleRate = 0.0;
    bool sampled = false;
    bool echo = false;
    bool stateUi = false;
    bool hasEnabledPort = false;
    bool hasLatency = false;
    bool lateLatency = false;
    bool workerLatency = false;
    bool workerEnd = false;
    bool emptyWork = false;
    bool workerRequested = false;
    std::atomic<bool> workerSynchronous { false };
    std::atomic<int> workerResult { 0 };
    std::thread::id requestThread;
    LV2_Worker_Schedule worker {};
    Lv2WorkerProbe* workerProbe = nullptr;
    std::array<float, 17> delay {};
    size_t delayPosition = 0;
    uint64_t processedFrames = 0;
    bool active = false;
    float privateGain = 1.0f;
    float pendingGain = -1.0f;
    LV2_URID_Map map {};
};

LV2_Handle instantiate(const LV2_Descriptor* descriptor, double sampleRate, const char*, const LV2_Feature* const* features)
{
    auto* processor = new Processor;
    processor->sampleRate = sampleRate;
    for (auto it = features; it && *it; ++it) {
        if (std::strcmp((*it)->URI, LV2_URID__map) == 0) {
            processor->map = *static_cast<const LV2_URID_Map*>((*it)->data);
        } else if (std::strcmp((*it)->URI, LV2_WORKER__schedule) == 0) {
            processor->worker = *static_cast<const LV2_Worker_Schedule*>((*it)->data);
        }
    }
    processor->sampled = std::strstr(descriptor->URI, "sample-rate") != nullptr;
    processor->hasEnabledPort = std::strstr(descriptor->URI, ":enabled") != nullptr;
    processor->hasLatency = std::strstr(descriptor->URI, ":latency") != nullptr;
    processor->lateLatency = std::strstr(descriptor->URI, ":latency-late") != nullptr;
    processor->workerLatency = std::strstr(descriptor->URI, ":latency-worker") != nullptr;
    processor->workerEnd = std::strstr(descriptor->URI, ":worker-end") != nullptr;
    processor->stateUi = std::strstr(descriptor->URI, "ui-state") != nullptr;
    processor->echo = processor->stateUi || std::strstr(descriptor->URI, "ui-echo") != nullptr;
    return processor;
}

void connectPort(LV2_Handle handle, uint32_t port, void* data)
{
    static_cast<Processor*>(handle)->ports.at(port) = static_cast<float*>(data);
}

void activate(LV2_Handle handle)
{
    auto& processor = *static_cast<Processor*>(handle);
    processor.active = true;
    processor.delay.fill(0.0f);
    processor.delayPosition = 0;
    processor.processedFrames = 0;
}

void deactivate(LV2_Handle handle)
{
    auto& processor = *static_cast<Processor*>(handle);
    if (auto* probe = processor.workerProbe) {
        probe->deactivatedWhileRunning.store(probe->entered.load() && !probe->finished.load());
    }
    processor.active = false;
}

void run(LV2_Handle handle, uint32_t count)
{
    auto& processor = *static_cast<Processor*>(handle);
    const auto& ports = processor.ports;
    if (processor.workerProbe) {
        ++processor.workerProbe->runs;
    }
    if ((processor.workerLatency || processor.workerEnd) && count > 0 && !processor.workerRequested) {
        processor.workerRequested = true;
        processor.requestThread = std::this_thread::get_id();
        const int latency = processor.delay.size();
        processor.worker.schedule_work(processor.worker.handle, processor.emptyWork ? 0 : sizeof(latency),
                                       processor.emptyWork ? nullptr : &latency);
    }
    bool stateChanged = false;
    if (processor.stateUi && ports[8]) {
        const auto* input = reinterpret_cast<const LV2_Atom_Sequence*>(ports[8]);
        const auto floatType = processor.map.map(processor.map.handle, LV2_ATOM__Float);
        LV2_ATOM_SEQUENCE_FOREACH(input, event) {
            if (event->body.type == floatType && event->body.size == sizeof(float)) {
                float value = 0.0f;
                std::memcpy(&value, &event->body + 1, sizeof(value));
                stateChanged = stateChanged || value != processor.privateGain;
                processor.privateGain = value;
            }
        }
    }
    if (ports[0] && ports[1]) {
        const bool bypassed = processor.hasEnabledPort && ports[3] && *ports[3] <= 0.0f;
        const auto gain = ports[2] ? *ports[2] / (processor.sampled ? processor.sampleRate : 1.0) : 1.0;
        for (uint32_t frame = 0; frame < count; ++frame) {
            const float value = ports[0][frame] * (bypassed ? 1.0 : gain * processor.privateGain);
            if (processor.hasLatency) {
                ports[1][frame] = processor.delay[processor.delayPosition];
                processor.delay[processor.delayPosition] = value;
                processor.delayPosition = (processor.delayPosition + 1) % processor.delay.size();
            } else {
                ports[1][frame] = value;
            }
        }
    }
    processor.processedFrames += count;
    if (processor.hasLatency && ports[3] && (!processor.lateLatency || count > 0)) {
        *ports[3]
            = processor.workerLatency ? static_cast<float>(processor.workerResult.load()) : static_cast<float>(processor.delay.size());
    }
    if (ports[7]) {
        *ports[7] = ports[2] ? *ports[2] : 0.0f;
    }
    if (processor.echo && ports[8] && ports[9]) {
        const auto* input = reinterpret_cast<const LV2_Atom_Sequence*>(ports[8]);
        auto* output = reinterpret_cast<LV2_Atom_Sequence*>(ports[9]);
        const auto size = sizeof(LV2_Atom) + input->atom.size;
        if (processor.stateUi) {
            output->atom.type = processor.map.map(processor.map.handle, LV2_ATOM__Sequence);
            output->atom.size = sizeof(LV2_Atom_Sequence_Body);
            output->body = {};
            if (stateChanged) {
                const int64_t time = 0;
                const LV2_Atom_Object object { { sizeof(LV2_Atom_Object_Body), processor.map.map(processor.map.handle, LV2_ATOM__Object) },
                    { 0, processor.map.map(processor.map.handle, LV2_STATE__StateChanged) } };
                auto* data = reinterpret_cast<unsigned char*>(output + 1);
                std::memcpy(data, &time, sizeof(time));
                std::memcpy(data + sizeof(time), &object, sizeof(object));
                output->atom.size += sizeof(time) + sizeof(object);
            }
        } else if (input->atom.size <= output->atom.size) {
            std::memcpy(output, input, size);
        } else {
            output->atom.size = sizeof(LV2_Atom_Sequence_Body);
        }
    }
}

void cleanup(LV2_Handle handle)
{
    auto* processor = static_cast<Processor*>(handle);
    if (auto* probe = processor->workerProbe) {
        probe->cleanedWhileRunning.store(probe->entered.load() && !probe->finished.load());
        probe->cleaned.store(true);
    }
    delete processor;
}

LV2_State_Status saveState(LV2_Handle instance, LV2_State_Store_Function store, LV2_State_Handle handle, uint32_t,
                           const LV2_Feature* const*)
{
    const auto& processor = *static_cast<Processor*>(instance);
    const auto key = processor.map.map(processor.map.handle, "urn:audacity:privateGain");
    const auto type = processor.map.map(processor.map.handle, LV2_ATOM__Float);
    return store(handle, key, &processor.privateGain, sizeof(float), type, LV2_STATE_IS_POD | LV2_STATE_IS_PORTABLE);
}

LV2_State_Status restoreState(LV2_Handle instance, LV2_State_Retrieve_Function retrieve, LV2_State_Handle handle, uint32_t,
                              const LV2_Feature* const*)
{
    auto& processor = *static_cast<Processor*>(instance);
    const auto key = processor.map.map(processor.map.handle, "urn:audacity:privateGain");
    const auto expectedType = processor.map.map(processor.map.handle, LV2_ATOM__Float);
    size_t size = 0;
    uint32_t type = 0;
    uint32_t flags = 0;
    const auto* value = retrieve(handle, key, &size, &type, &flags);
    processor.privateGain = 1.0f;
    if (!value) {
        return LV2_STATE_SUCCESS;
    }
    if (type != expectedType || size != sizeof(float)) {
        return LV2_STATE_ERR_BAD_TYPE;
    }
    std::memcpy(&processor.privateGain, value, size);
    return LV2_STATE_SUCCESS;
}

const void* extensionData(const char* uri)
{
    static const LV2_State_Interface state { saveState, restoreState };
    return std::strcmp(uri, LV2_STATE__interface) == 0 ? &state : nullptr;
}

LV2_Worker_Status work(LV2_Handle handle, LV2_Worker_Respond_Function respond, LV2_Worker_Respond_Handle response,
                       uint32_t size, const void* data)
{
    auto& processor = *static_cast<Processor*>(handle);
    processor.workerSynchronous.store(processor.requestThread == std::this_thread::get_id());
    auto* probe = processor.workerProbe;
    if (probe) {
        ++probe->workCalls;
        if (size == 0) {
            ++probe->emptyRequests;
        }
        probe->entered.store(true);
        while (!probe->release.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
    const auto result = respond(response, size, data);
    if (probe) {
        probe->finished.store(true);
    }
    return result;
}

LV2_Worker_Status workResponse(LV2_Handle handle, uint32_t size, const void* data)
{
    auto& processor = *static_cast<Processor*>(handle);
    if (processor.workerProbe) {
        ++processor.workerProbe->replies;
        if (size == 0) {
            ++processor.workerProbe->emptyReplies;
        }
    }
    if (processor.workerEnd) {
        processor.pendingGain = 0.25f;
    }
    if (size == sizeof(int)) {
        int value = 0;
        std::memcpy(&value, data, size);
        processor.workerResult.store(value);
    }
    return LV2_WORKER_SUCCESS;
}

LV2_Worker_Status endRun(LV2_Handle handle)
{
    auto& processor = *static_cast<Processor*>(handle);
    if (processor.pendingGain >= 0.0f) {
        processor.privateGain = processor.pendingGain;
        processor.pendingGain = -1.0f;
    }
    if (processor.workerProbe) {
        ++processor.workerProbe->endRuns;
        processor.workerProbe->repliesAtEnd.store(processor.workerProbe->replies.load());
    }
    return LV2_WORKER_SUCCESS;
}

const void* workerExtensionData(const char* uri)
{
    static const LV2_Worker_Interface worker { work, workResponse, endRun };
    return std::strcmp(uri, LV2_WORKER__interface) == 0 ? &worker : nullptr;
}

const std::array<LV2_Descriptor, 26> descriptors { {
    { "urn:audacity:tracktion-test:gain", instantiate, connectPort, activate, run, deactivate, cleanup, nullptr },
    { "urn:audacity:tracktion-test:generator", instantiate, connectPort, activate, run, deactivate, cleanup, nullptr },
    { "urn:audacity:tracktion-test:analyzer", instantiate, connectPort, activate, run, deactivate, cleanup, nullptr },
    { "urn:audacity:tracktion-test:tool", instantiate, connectPort, activate, run, deactivate, cleanup, nullptr },
    { "urn:audacity:tracktion-test:instrument", instantiate, connectPort, activate, run, deactivate, cleanup, nullptr },
    { "urn:audacity:tracktion-test:midi", instantiate, connectPort, activate, run, deactivate, cleanup, nullptr },
    { "urn:audacity:tracktion-test:math", instantiate, connectPort, activate, run, deactivate, cleanup, nullptr },
    { "urn:audacity:tracktion-test:empty", instantiate, connectPort, activate, run, deactivate, cleanup, nullptr },
    { "urn:audacity:tracktion-test:unsupported", instantiate, connectPort, activate, run, deactivate, cleanup, nullptr },
    { "urn:audacity:tracktion-test:no-author", instantiate, connectPort, activate, run, deactivate, cleanup, nullptr },
    { "urn:audacity:tracktion-test:unicode", instantiate, connectPort, activate, run, deactivate, cleanup, nullptr },
    { "urn:audacity:tracktion-test:sample-rate", instantiate, connectPort, activate, run, deactivate, cleanup, nullptr },
    { "urn:audacity:tracktion-test:bounded-default", instantiate, connectPort, activate, run, deactivate, cleanup, nullptr },
    { "urn:audacity:tracktion-test:private", instantiate, connectPort, activate, run, deactivate, cleanup, extensionData },
    { "urn:audacity:tracktion-test:rich", instantiate, connectPort, activate, run, deactivate, cleanup, nullptr },
    { "urn:audacity:tracktion-test:duplicates", instantiate, connectPort, activate, run, deactivate, cleanup, nullptr },
    { "urn:audacity:tracktion-test:ui-echo", instantiate, connectPort, activate, run, deactivate, cleanup, extensionData },
    { "urn:audacity:tracktion-test:ui-state", instantiate, connectPort, activate, run, deactivate, cleanup, extensionData },
    { "urn:audacity:tracktion-test:x11-ui", instantiate, connectPort, activate, run, deactivate, cleanup, extensionData },
    { "urn:audacity:tracktion-test:external-ui", instantiate, connectPort, activate, run, deactivate, cleanup, extensionData },
    { "urn:audacity:tracktion-test:missing-ui-feature", instantiate, connectPort, activate, run, deactivate, cleanup, extensionData },
    { "urn:audacity:tracktion-test:enabled", instantiate, connectPort, activate, run, deactivate, cleanup, extensionData },
    { "urn:audacity:tracktion-test:latency", instantiate, connectPort, activate, run, deactivate, cleanup, nullptr },
    { "urn:audacity:tracktion-test:latency-late", instantiate, connectPort, activate, run, deactivate, cleanup, nullptr },
    { "urn:audacity:tracktion-test:latency-worker", instantiate, connectPort, activate, run, deactivate, cleanup, workerExtensionData },
    { "urn:audacity:tracktion-test:worker-end", instantiate, connectPort, activate, run, deactivate, cleanup, workerExtensionData },
} };
}

extern "C" LV2_SYMBOL_EXPORT const LV2_Descriptor* lv2_descriptor(uint32_t index)
{
    return index < descriptors.size() ? &descriptors[index] : nullptr;
}

#include "lv2uiprobe.h"
#include <lv2/ui/ui.h>
#include <lv2/instance-access/instance-access.h>
#include <lv2/data-access/data-access.h>
#include <lv2/options/options.h>
#include <lv2/parameters/parameters.h>
#include "au3-lv2/lv2_external_ui.h"

namespace {
Lv2UiProbe uiProbe;
struct Ui {
    LV2UI_Write_Function write;
    LV2UI_Controller controller;
    LV2_External_UI_Widget widget;
    const LV2_External_UI_Host* host = nullptr;
    const LV2_URID_Map* map = nullptr;
    const LV2_Extension_Data_Feature* data = nullptr;
    void* instance = nullptr;
    bool closed = false;
};
Ui* currentUi = nullptr;

void runUi(LV2_External_UI_Widget* widget)
{
    auto& ui = *currentUi;
    if (&ui.widget == widget && ui.closed && ui.host) {
        ui.host->ui_closed(ui.controller);
    }
}

void showUi(LV2_External_UI_Widget*) { ++uiProbe.shown; }
void hideUi(LV2_External_UI_Widget*) { ++uiProbe.hidden; }

LV2UI_Handle instantiateUi(const LV2UI_Descriptor*, const char*, const char*, LV2UI_Write_Function write,
                           LV2UI_Controller controller, LV2UI_Widget* widget, const LV2_Feature* const* features)
{
    auto ui = new Ui { write, controller, { runUi, showUi, hideUi } };
    const LV2UI_Port_Map* ports = nullptr;
    const LV2UI_Resize* resize = nullptr;
    const LV2_Options_Option* options = nullptr;
    for (auto it = features; it && *it; ++it) {
        const auto& feature = **it;
        if (std::strcmp(feature.URI, LV2_URID__map) == 0) {
            ui->map = static_cast<const LV2_URID_Map*>(feature.data);
        } else if (std::strcmp(feature.URI, LV2_INSTANCE_ACCESS_URI) == 0) {
            ui->instance = feature.data;
        } else if (std::strcmp(feature.URI, LV2_DATA_ACCESS_URI) == 0) {
            ui->data = static_cast<const LV2_Extension_Data_Feature*>(feature.data);
        } else if (std::strcmp(feature.URI, LV2_UI__portMap) == 0) {
            ports = static_cast<const LV2UI_Port_Map*>(feature.data);
        } else if (std::strcmp(feature.URI, LV2_UI__resize) == 0) {
            resize = static_cast<const LV2UI_Resize*>(feature.data);
        } else if (std::strcmp(feature.URI, LV2_OPTIONS__options) == 0) {
            options = static_cast<const LV2_Options_Option*>(feature.data);
        } else if (std::strcmp(feature.URI, LV2_EXTERNAL_UI__Host) == 0) {
            ui->host = static_cast<const LV2_External_UI_Host*>(feature.data);
        }
    }
    uiProbe.validFeatures = ui->map && ui->instance && ui->data && ui->data->data_access && ports && resize && options;
    if (uiProbe.validFeatures) {
        uiProbe.validFeatures = ports->port_index(ports->handle, "gain") == 2
                                && ports->port_index(ports->handle, "missing") == LV2UI_INVALID_PORT_INDEX;
        const auto rateKey = ui->map->map(ui->map->handle, LV2_PARAMETERS__sampleRate);
        for (auto* option = options; option->key; ++option) {
            if (option->key == rateKey && option->size == sizeof(float)) {
                std::memcpy(&uiProbe.sampleRate, option->value, sizeof(float));
            }
        }
        resize->ui_resize(resize->handle, 320, 200);
    }
    uiProbe.instance = ui->instance;
    ++uiProbe.opened;
    currentUi = ui;
    *widget = &ui->widget;
    return ui;
}

void cleanupUi(LV2UI_Handle handle)
{
    if (handle == currentUi) {
        currentUi = nullptr;
    }
    delete static_cast<Ui*>(handle);
    ++uiProbe.cleaned;
}

void portEvent(LV2UI_Handle handle, uint32_t port, uint32_t size, uint32_t protocol, const void* buffer)
{
    const auto& ui = *static_cast<Ui*>(handle);
    if (protocol == 0 && port == 2 && size == sizeof(float)) {
        std::memcpy(&uiProbe.gain, buffer, size);
        ++uiProbe.portEvents;
    } else if (ui.map && protocol == ui.map->map(ui.map->handle, LV2_ATOM__eventTransfer) && size == sizeof(LV2_Atom_Float)) {
        const auto& atom = *static_cast<const LV2_Atom_Float*>(buffer);
        uiProbe.atomValue = atom.body;
        ++uiProbe.atomEvents;
    }
}

int idleUi(LV2UI_Handle handle) { return static_cast<Ui*>(handle)->closed ? 1 : 0; }
int showUi(LV2UI_Handle) { ++uiProbe.shown; return 0; }
int hideUi(LV2UI_Handle) { ++uiProbe.hidden; return 0; }
const void* uiExtension(const char* uri)
{
    static const LV2UI_Idle_Interface idle { idleUi };
    static const LV2UI_Show_Interface show { showUi, hideUi };
    return std::strcmp(uri, LV2_UI__idleInterface) == 0 ? &idle
           : std::strcmp(uri, LV2_UI__showInterface) == 0 ? static_cast<const void*>(&show) : nullptr;
}

const std::array<LV2UI_Descriptor, 7> uiDescriptors { {
    { "urn:audacity:tracktion-test:ui-echo#qt", instantiateUi, cleanupUi, portEvent, uiExtension },
    { "urn:audacity:tracktion-test:ui-state#qt", instantiateUi, cleanupUi, portEvent, uiExtension },
    { "urn:audacity:tracktion-test:ui-echo#x11", instantiateUi, cleanupUi, portEvent, uiExtension },
    { "urn:audacity:tracktion-test:x11-ui#x11", instantiateUi, cleanupUi, portEvent, uiExtension },
    { "urn:audacity:tracktion-test:ui-echo#external", instantiateUi, cleanupUi, portEvent, uiExtension },
    { "urn:audacity:tracktion-test:external-ui#external", instantiateUi, cleanupUi, portEvent, uiExtension },
    { "urn:audacity:tracktion-test:missing-ui-feature#qt", instantiateUi, cleanupUi, portEvent, uiExtension },
} };
}

extern "C" LV2_SYMBOL_EXPORT const LV2UI_Descriptor* lv2ui_descriptor(uint32_t index)
{
    return index < uiDescriptors.size() ? &uiDescriptors[index] : nullptr;
}

extern "C" LV2_SYMBOL_EXPORT const Lv2UiProbe* tracktion_ui_probe() { return &uiProbe; }
extern "C" LV2_SYMBOL_EXPORT void tracktion_ui_reset() { uiProbe = {}; }
extern "C" LV2_SYMBOL_EXPORT void tracktion_ui_gain(float value)
{
    currentUi->write(currentUi->controller, 2, sizeof(value), 0, &value);
}

extern "C" LV2_SYMBOL_EXPORT void tracktion_ui_enabled(float value)
{
    currentUi->write(currentUi->controller, 3, sizeof(value), 0, &value);
}

extern "C" LV2_SYMBOL_EXPORT void tracktion_ui_atom(float value)
{
    LV2_Atom_Float atom { { sizeof(float), currentUi->map->map(currentUi->map->handle, LV2_ATOM__Float) }, value };
    const auto protocol = currentUi->map->map(currentUi->map->handle, LV2_ATOM__eventTransfer);
    currentUi->write(currentUi->controller, 8, sizeof(atom), protocol, &atom);
}

extern "C" LV2_SYMBOL_EXPORT void tracktion_ui_close() { currentUi->closed = true; }

extern "C" LV2_SYMBOL_EXPORT bool tracktion_dsp_active(const void* handle)
{
    return static_cast<const Processor*>(handle)->active;
}

extern "C" LV2_SYMBOL_EXPORT uint64_t tracktion_dsp_frames(const void* handle)
{
    return static_cast<const Processor*>(handle)->processedFrames;
}

extern "C" LV2_SYMBOL_EXPORT bool tracktion_dsp_worker_synchronous(const void* handle)
{
    return static_cast<const Processor*>(handle)->workerSynchronous.load();
}

extern "C" LV2_SYMBOL_EXPORT void tracktion_dsp_request_work(void* handle)
{
    auto& processor = *static_cast<Processor*>(handle);
    processor.workerRequested = false;
    processor.workerSynchronous.store(true);
}

extern "C" LV2_SYMBOL_EXPORT void tracktion_dsp_worker_probe(void* handle, Lv2WorkerProbe* probe)
{
    static_cast<Processor*>(handle)->workerProbe = probe;
}

extern "C" LV2_SYMBOL_EXPORT void tracktion_dsp_request_empty_work(void* handle)
{
    auto& processor = *static_cast<Processor*>(handle);
    processor.workerRequested = false;
    processor.emptyWork = true;
}
