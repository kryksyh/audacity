/* A small native LV2 instrument used to verify MIDI, block, and state contracts. */
#include <array>
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include <lv2/core/lv2.h>
#include <lv2/options/options.h>
#include <lv2/buf-size/buf-size.h>
#include <lv2/atom/util.h>
#include <lv2/midi/midi.h>
#include <lv2/patch/patch.h>
#include <lv2/state/state.h>
#include <lv2/time/time.h>
#include <lv2/urid/urid.h>

namespace {
struct Synth {
    const LV2_Atom_Sequence* midi = nullptr;
    const float* gain = nullptr;
    float* left = nullptr;
    float* right = nullptr;
    float* tempo = nullptr;
    const float* lookahead = nullptr;
    float* latency = nullptr;
    std::array<float, 32> delayed {};
    size_t delayHead = 0;
    LV2_URID midiType {}, objectType {}, positionType {}, tempoKey {}, privateKey {}, assetKey {}, floatType {}, pathType {};
    LV2_URID patchType {}, propertyKey {}, valueKey {}, uridType {}, assetMultiplierKey {};
    std::array<float, 128> velocities {};
    std::array<bool, 128> held {};
    bool pedal = false, fixed = false;
    uint32_t firstBlock = 0;
    std::atomic<float> multiplier { 1.0f };
    std::atomic<float> assetValue { 1.0f };
    std::atomic<bool> assetDirty { false };
    std::string assetPath;
};

LV2_Handle instantiate(const LV2_Descriptor* descriptor, double, const char*, const LV2_Feature* const* features)
{
    const LV2_URID_Map* map = nullptr;
    const LV2_State_Make_Path* makePath = nullptr;
    const LV2_State_Free_Path* freePath = nullptr;
    for (auto feature = features; *feature; ++feature) {
        if (!std::strcmp((*feature)->URI, LV2_URID__map)) {
            map = static_cast<LV2_URID_Map*>((*feature)->data);
        }
        if (!std::strcmp((*feature)->URI, LV2_STATE__makePath)) {
            makePath = static_cast<LV2_State_Make_Path*>((*feature)->data);
        }
        if (!std::strcmp((*feature)->URI, LV2_STATE__freePath)) {
            freePath = static_cast<LV2_State_Free_Path*>((*feature)->data);
        }
    }
    if (!map || !makePath) {
        return nullptr;
    }
    auto* synth = new Synth;
    synth->fixed = !std::strcmp(descriptor->URI, "urn:audacity:test:fixed-instrument");
    synth->midiType = map->map(map->handle, LV2_MIDI__MidiEvent);
    synth->objectType = map->map(map->handle, LV2_ATOM__Object);
    synth->positionType = map->map(map->handle, LV2_TIME__Position);
    synth->tempoKey = map->map(map->handle, LV2_TIME__beatsPerMinute);
    synth->privateKey = map->map(map->handle, "urn:audacity:test:multiplier");
    synth->assetKey = map->map(map->handle, "urn:audacity:test:asset");
    synth->assetMultiplierKey = map->map(map->handle, "urn:audacity:test:asset-multiplier");
    synth->floatType = map->map(map->handle, LV2_ATOM__Float);
    synth->pathType = map->map(map->handle, LV2_ATOM__Path);
    synth->patchType = map->map(map->handle, LV2_PATCH__Set);
    synth->propertyKey = map->map(map->handle, LV2_PATCH__property);
    synth->valueKey = map->map(map->handle, LV2_PATCH__value);
    synth->uridType = map->map(map->handle, LV2_ATOM__URID);
    char* asset = makePath->path(makePath->handle, "samples/asset.bin");
    if (asset) {
        synth->assetPath = asset;
        if (auto* file = std::fopen(asset, "wb")) {
            const auto value = synth->assetValue.load();
            std::fwrite(&value, sizeof(value), 1, file);
            std::fclose(file);
        }
        if (freePath) {
            freePath->free_path(freePath->handle, asset);
        } else {
            std::free(asset);
        }
    }
    return synth;
}

void connectPort(LV2_Handle handle, uint32_t index, void* data)
{
    auto& synth = *static_cast<Synth*>(handle);
    switch (index) {
    case 0: synth.midi = static_cast<LV2_Atom_Sequence*>(data);
        break;
    case 1: synth.gain = static_cast<float*>(data);
        break;
    case 2: synth.left = static_cast<float*>(data);
        break;
    case 3: synth.right = static_cast<float*>(data);
        break;
    case 4: synth.tempo = static_cast<float*>(data);
        break;
    case 5: synth.lookahead = static_cast<float*>(data);
        break;
    case 6: synth.latency = static_cast<float*>(data);
        break;
    }
}

void run(LV2_Handle handle, uint32_t frames)
{
    auto& synth = *static_cast<Synth*>(handle);
    if (!synth.firstBlock) {
        synth.firstBlock = frames;
    }
    const bool wrongBlock = synth.fixed && (frames != synth.firstBlock || (frames & (frames - 1)) != 0);
    const auto delay = synth.lookahead ? static_cast<size_t>(std::clamp(*synth.lookahead, 0.0f, 31.0f)) : 0;
    if (synth.latency) {
        *synth.latency = delay;
    }
    const auto* event = lv2_atom_sequence_begin(&synth.midi->body);
    for (uint32_t frame = 0; frame < frames; ++frame) {
        while (!lv2_atom_sequence_is_end(&synth.midi->body, synth.midi->atom.size, event) && event->time.frames <= frame) {
            if (event->body.type == synth.midiType) {
                const auto* bytes = static_cast<const uint8_t*>(LV2_ATOM_BODY_CONST(&event->body));
                const auto type = bytes[0] & 0xf0;
                if (type == 0x90 && bytes[2]) {
                    synth.held[bytes[1]] = true;
                    synth.velocities[bytes[1]] = bytes[2] / 127.0f;
                } else if (type == 0x80 || type == 0x90) {
                    synth.held[bytes[1]] = false;
                    if (!synth.pedal) {
                        synth.velocities[bytes[1]] = 0.0f;
                    }
                } else if (type == 0xb0 && bytes[1] == 64) {
                    synth.pedal = bytes[2] >= 64;
                    if (!synth.pedal) {
                        for (size_t i = 0; i < 128; ++i) {
                            if (!synth.held[i]) {
                                synth.velocities[i] = 0.0f;
                            }
                        }
                    }
                } else if (type == 0xb0 && (bytes[1] == 120 || bytes[1] == 123)) {
                    synth.velocities.fill(0.0f);
                    synth.held.fill(false);
                    if (bytes[1] == 120) {
                        synth.delayed.fill(0.0f);
                    }
                } else if (type == 0xc0) {
                    synth.multiplier.store(bytes[1] + 1.0f);
                }
            } else if (event->body.type == synth.objectType) {
                const auto* object = reinterpret_cast<const LV2_Atom_Object*>(&event->body);
                if (object->body.otype == synth.positionType) {
                    const LV2_Atom* tempo = nullptr;
                    lv2_atom_object_get(object, synth.tempoKey, &tempo, 0);
                    if (tempo && tempo->type == synth.floatType) {
                        *synth.tempo = reinterpret_cast<const LV2_Atom_Float*>(tempo)->body;
                    }
                } else if (object->body.otype == synth.patchType) {
                    const LV2_Atom* property = nullptr;
                    const LV2_Atom* value = nullptr;
                    lv2_atom_object_get(object, synth.propertyKey, &property, synth.valueKey, &value, 0);
                    if (property && value && property->type == synth.uridType && value->type == synth.floatType) {
                        const auto key = reinterpret_cast<const LV2_Atom_URID*>(property)->body;
                        const auto number = reinterpret_cast<const LV2_Atom_Float*>(value)->body;
                        if (key == synth.privateKey) {
                            synth.multiplier.store(number);
                        }
                        if (key == synth.assetMultiplierKey) {
                            synth.assetValue.store(number);
                            synth.assetDirty.store(true);
                        }
                    }
                }
            }
            event = lv2_atom_sequence_next(event);
        }
        float value = 0.0f;
        for (auto velocity : synth.velocities) {
            value += velocity;
        }
        const auto gain = *synth.gain;
        const auto tempo = *synth.tempo;
        synth.delayed[synth.delayHead] = value * gain * synth.multiplier.load() * synth.assetValue.load() * tempo / 120.0f;
        synth.left[frame] = synth.right[frame] = wrongBlock ? -100.0f
                                                 : synth.delayed[(synth.delayHead + synth.delayed.size() - delay) % synth.delayed.size()];
        synth.delayHead = (synth.delayHead + 1) % synth.delayed.size();
    }
}

LV2_State_Status save(LV2_Handle handle, LV2_State_Store_Function store, LV2_State_Handle state, uint32_t,
                      const LV2_Feature* const* features)
{
    auto& synth = *static_cast<Synth*>(handle);
    // A sample edit is committed at state capture, never by the audio callback.
    // A fresh instance creates a default file, so lost archived bytes change DSP.
    if (synth.assetDirty.exchange(false)) {
        if (auto* file = std::fopen(synth.assetPath.c_str(), "wb")) {
            const auto asset = synth.assetValue.load();
            std::fwrite(&asset, sizeof(asset), 1, file);
            std::fclose(file);
        }
    }
    const auto value = synth.multiplier.load();
    store(state, synth.privateKey, &value, sizeof(value), synth.floatType, LV2_STATE_IS_POD | LV2_STATE_IS_PORTABLE);
    for (auto feature = features; *feature; ++feature) {
        if (!std::strcmp((*feature)->URI, LV2_STATE__mapPath)) {
            auto* map = static_cast<LV2_State_Map_Path*>((*feature)->data);
            char* abstract = map->abstract_path(map->handle, synth.assetPath.c_str());
            store(state, synth.assetKey, abstract, std::strlen(abstract) + 1, synth.pathType, LV2_STATE_IS_POD | LV2_STATE_IS_PORTABLE);
            std::free(abstract);
        }
    }
    return LV2_STATE_SUCCESS;
}

LV2_State_Status restore(LV2_Handle handle, LV2_State_Retrieve_Function retrieve, LV2_State_Handle state, uint32_t,
                         const LV2_Feature* const* features)
{
    auto& synth = *static_cast<Synth*>(handle);
    size_t size;
    uint32_t type, flags;
    if (const auto* value = retrieve(state, synth.privateKey, &size, &type, &flags); value && size == sizeof(float)) {
        synth.multiplier.store(*static_cast<const float*>(value));
    }
    if (const auto* value = retrieve(state, synth.assetKey, &size, &type, &flags)) {
        for (auto feature = features; *feature; ++feature) {
            if (!std::strcmp((*feature)->URI, LV2_STATE__mapPath)) {
                auto* map = static_cast<LV2_State_Map_Path*>((*feature)->data);
                char* path = map->absolute_path(map->handle, static_cast<const char*>(value));
                synth.assetValue = -100.0f;
                if (auto* file = std::fopen(path, "rb")) {
                    float value = -100.0f;
                    std::fread(&value, sizeof(value), 1, file);
                    synth.assetValue.store(value);
                    std::fclose(file);
                }
                synth.assetPath = path;
                std::free(path);
            }
        }
    }
    return LV2_STATE_SUCCESS;
}

const void* extensionData(const char* uri)
{
    static const LV2_State_Interface state { save, restore };
    return !std::strcmp(uri, LV2_STATE__interface) ? &state : nullptr;
}
}

LV2_SYMBOL_EXPORT const LV2_Descriptor* lv2_descriptor(uint32_t index)
{
    struct Gain {
        const float* input = nullptr;
        const float* gain = nullptr;
        float* output = nullptr;
        uint32_t block = 0;
        const LV2_Atom_Sequence* events = nullptr;
        float multiplier = 1.0f;
        LV2_URID floatType = 0;
        const float* lookahead = nullptr;
        float* latency = nullptr;
        std::array<float, 32> delayed {};
        size_t head = 0;
    };
    static const LV2_Descriptor descriptors[] {
        { "urn:audacity:test:native-instrument", instantiate, connectPort, nullptr, run, nullptr,
          [](LV2_Handle handle) { delete static_cast<Synth*>(handle); }, extensionData },
        { "urn:audacity:test:fixed-instrument", instantiate, connectPort, nullptr, run, nullptr,
          [](LV2_Handle handle) { delete static_cast<Synth*>(handle); }, extensionData },
        { "urn:audacity:test:audio-effect",
          [](const LV2_Descriptor*, double, const char*, const LV2_Feature* const*) -> LV2_Handle { return new Gain; },
          [](LV2_Handle handle, uint32_t port, void* buffer) {
                auto& gain = *static_cast<Gain*>(handle);
                if (port == 0) {
                    gain.input = static_cast<float*>(buffer);
                }
                if (port == 1) {
                    gain.gain = static_cast<float*>(buffer);
                }
                if (port == 2) {
                    gain.output = static_cast<float*>(buffer);
                }
            }, nullptr,
          [](LV2_Handle handle, uint32_t frames) {
                const auto& gain = *static_cast<Gain*>(handle);
                for (uint32_t i = 0; i < frames; ++i) {
                    gain.output[i] = gain.input[i] * *gain.gain;
                }
            }, nullptr, [](LV2_Handle handle) { delete static_cast<Gain*>(handle); }, nullptr }
    };
    static const LV2_Descriptor fixed {
        "urn:audacity:test:fixed-audio-effect",
        [](const LV2_Descriptor*, double, const char*, const LV2_Feature* const* features) -> LV2_Handle {
            const LV2_URID_Map* map = nullptr;
            const LV2_Options_Option* options = nullptr;
            bool hasFixed = false, hasPower = false;
            for (auto feature = features; *feature; ++feature) {
                if (!std::strcmp((*feature)->URI, LV2_URID__map)) {
                    map = static_cast<const LV2_URID_Map*>((*feature)->data);
                }
                if (!std::strcmp((*feature)->URI, LV2_OPTIONS__options)) {
                    options = static_cast<const LV2_Options_Option*>((*feature)->data);
                }
                hasFixed |= !std::strcmp((*feature)->URI, LV2_BUF_SIZE__fixedBlockLength);
                hasPower |= !std::strcmp((*feature)->URI, LV2_BUF_SIZE__powerOf2BlockLength);
            }
            if (!map || !options || !hasFixed || !hasPower) {
                return nullptr;
            }
            const auto minimum = map->map(map->handle, LV2_BUF_SIZE__minBlockLength);
            const auto maximum = map->map(map->handle, LV2_BUF_SIZE__maxBlockLength);
            const auto nominal = map->map(map->handle, LV2_BUF_SIZE__nominalBlockLength);
            const auto integer = map->map(map->handle, LV2_ATOM__Int);
            int32_t min = 0, max = 0, size = 0;
            for (auto option = options; option->key; ++option) {
                if (option->key != minimum && option->key != maximum && option->key != nominal) {
                    continue;
                }
                if (option->size != sizeof(int32_t) || option->type != integer) {
                    return nullptr;
                }
                const auto value = *static_cast<const int32_t*>(option->value);
                if (option->key == minimum) {
                    min = value;
                }
                if (option->key == maximum) {
                    max = value;
                }
                if (option->key == nominal) {
                    size = value;
                }
            }
            if (size <= 0 || min != size || max != size || (size & (size - 1))) {
                return nullptr;
            }
            auto* gain = new Gain;
            gain->block = size;
            gain->floatType = map->map(map->handle, LV2_ATOM__Float);
            return gain;
        },
        [](LV2_Handle handle, uint32_t port, void* buffer) {
            auto& gain = *static_cast<Gain*>(handle);
            if (port == 0) {
                gain.input = static_cast<float*>(buffer);
            }
            if (port == 1) {
                gain.gain = static_cast<float*>(buffer);
            }
            if (port == 2) {
                gain.output = static_cast<float*>(buffer);
            }
            if (port == 3) {
                gain.events = static_cast<const LV2_Atom_Sequence*>(buffer);
            }
            if (port == 4) {
                gain.lookahead = static_cast<const float*>(buffer);
            }
            if (port == 5) {
                gain.latency = static_cast<float*>(buffer);
            }
        }, [](LV2_Handle handle) {
            auto& gain = *static_cast<Gain*>(handle);
            gain.delayed.fill(0.0f);
            gain.head = 0;
        },
        [](LV2_Handle handle, uint32_t frames) {
            auto& gain = *static_cast<Gain*>(handle);
            const auto* event = lv2_atom_sequence_begin(&gain.events->body);
            const auto latency = gain.lookahead ? static_cast<size_t>(std::clamp(*gain.lookahead, 0.0f, 31.0f)) : 0;
            if (gain.latency) {
                *gain.latency = latency;
            }
            for (uint32_t i = 0; i < frames; ++i) {
                while (!lv2_atom_sequence_is_end(&gain.events->body, gain.events->atom.size, event) && event->time.frames <= i) {
                    if (event->body.type == gain.floatType && event->body.size == sizeof(float)) {
                        gain.multiplier = reinterpret_cast<const LV2_Atom_Float*>(&event->body)->body;
                    }
                    event = lv2_atom_sequence_next(event);
                }
                gain.delayed[gain.head] = gain.input[i] * *gain.gain * gain.multiplier;
                gain.output[i] = frames
                                 == gain.block ? gain.delayed[(gain.head + gain.delayed.size() - latency) % gain.delayed.size()] : -100.0f;
                gain.head = (gain.head + 1) % gain.delayed.size();
            }
        }, nullptr, descriptors[2].cleanup, nullptr
    };
    static const LV2_Descriptor delayed {
        "urn:audacity:test:delayed-instrument", instantiate, connectPort, nullptr, run, nullptr,
        [](LV2_Handle handle) { delete static_cast<Synth*>(handle); }, extensionData
    };
    return index < 3 ? &descriptors[index] : index == 3 ? &fixed : index == 4 ? &delayed : nullptr;
}
