/* Native toolkit editor fixture: no Tracktion, JUCE, or audio device. */
#include <QSlider>
#include <QPushButton>
#include <QSignalBlocker>
#include <QVBoxLayout>
#include <QWidget>
#include <cmath>
#include <cstring>
#include <lv2/ui/ui.h>
#include <lv2/atom/forge.h>
#include <lv2/patch/patch.h>
#include <lv2/urid/urid.h>

namespace {
struct Editor {
    QWidget* widget;
    QSlider* gain;
    LV2UI_Write_Function write;
    LV2UI_Controller controller;
};

LV2UI_Handle instantiate(const LV2UI_Descriptor*, const char*, const char*, LV2UI_Write_Function write,
                         LV2UI_Controller controller, LV2UI_Widget* widget, const LV2_Feature* const* features)
{
    QWidget* parent = nullptr;
    const LV2UI_Resize* resize = nullptr;
    LV2_URID_Map* map = nullptr;
    for (auto feature = features; *feature; ++feature) {
        if (!std::strcmp((*feature)->URI, LV2_UI__parent)) {
            parent = static_cast<QWidget*>((*feature)->data);
        }
        if (!std::strcmp((*feature)->URI, LV2_UI__resize)) {
            resize = static_cast<LV2UI_Resize*>((*feature)->data);
        }
        if (!std::strcmp((*feature)->URI, LV2_URID__map)) {
            map = static_cast<LV2_URID_Map*>((*feature)->data);
        }
    }
    auto* editor = new Editor { new QWidget(parent), nullptr, write, controller };
    editor->widget->setObjectName("nativeLv2Editor");
    editor->widget->resize(360, 180);
    auto* layout = new QVBoxLayout(editor->widget);
    editor->gain = new QSlider(Qt::Horizontal, editor->widget);
    editor->gain->setObjectName("nativeLv2Gain");
    editor->gain->setRange(0, 100);
    layout->addWidget(editor->gain);
    QObject::connect(editor->gain, &QSlider::valueChanged, editor->widget, [editor](int position) {
        const float value = position / 100.0f;
        editor->write(editor->controller, 1, sizeof(value), 0, &value);
    });
    auto* button = new QPushButton("Set private state", editor->widget);
    button->setObjectName("nativeLv2Preset");
    layout->addWidget(button);
    QObject::connect(button, &QPushButton::clicked, editor->widget, [editor, map] {
        uint8_t buffer[128] {};
        LV2_Atom_Forge forge;
        LV2_Atom_Forge_Frame object;
        lv2_atom_forge_init(&forge, map);
        lv2_atom_forge_set_buffer(&forge, buffer, sizeof(buffer));
        lv2_atom_forge_object(&forge, &object, 0, map->map(map->handle, LV2_PATCH__Set));
        lv2_atom_forge_key(&forge, map->map(map->handle, LV2_PATCH__property));
        lv2_atom_forge_urid(&forge, map->map(map->handle, "urn:audacity:test:multiplier"));
        lv2_atom_forge_key(&forge, map->map(map->handle, LV2_PATCH__value));
        lv2_atom_forge_float(&forge, 3.0f);
        lv2_atom_forge_pop(&forge, &object);
        editor->write(editor->controller, 0, sizeof(LV2_Atom) + reinterpret_cast<const LV2_Atom*>(buffer)->size,
                      map->map(map->handle, LV2_ATOM__eventTransfer), buffer);
    });
    auto* asset = new QPushButton("Change referenced sample", editor->widget);
    asset->setObjectName("nativeLv2Asset");
    layout->addWidget(asset);
    QObject::connect(asset, &QPushButton::clicked, editor->widget, [editor, map] {
        uint8_t buffer[128] {};
        LV2_Atom_Forge forge;
        LV2_Atom_Forge_Frame object;
        lv2_atom_forge_init(&forge, map);
        lv2_atom_forge_set_buffer(&forge, buffer, sizeof(buffer));
        lv2_atom_forge_object(&forge, &object, 0, map->map(map->handle, LV2_PATCH__Set));
        lv2_atom_forge_key(&forge, map->map(map->handle, LV2_PATCH__property));
        lv2_atom_forge_urid(&forge, map->map(map->handle, "urn:audacity:test:asset-multiplier"));
        lv2_atom_forge_key(&forge, map->map(map->handle, LV2_PATCH__value));
        lv2_atom_forge_float(&forge, 1.5f);
        lv2_atom_forge_pop(&forge, &object);
        editor->write(editor->controller, 0, sizeof(LV2_Atom) + reinterpret_cast<const LV2_Atom*>(buffer)->size,
                      map->map(map->handle, LV2_ATOM__eventTransfer), buffer);
    });
    *widget = editor->widget;
    if (resize) {
        resize->ui_resize(resize->handle, 360, 180);
    }
    return editor;
}

void portEvent(LV2UI_Handle handle, uint32_t port, uint32_t size, uint32_t format, const void* buffer)
{
    if (port == 1 && size == sizeof(float) && format == 0) {
        auto& editor = *static_cast<Editor*>(handle);
        QSignalBlocker block(editor.gain);
        editor.gain->setValue(std::lround(*static_cast<const float*>(buffer) * 100));
    }
}
}

LV2_SYMBOL_EXPORT const LV2UI_Descriptor* lv2ui_descriptor(uint32_t index)
{
    static const LV2UI_Descriptor descriptor {
        "urn:audacity:test:instrument-ui", instantiate,
        [](LV2UI_Handle handle) {
            auto* editor = static_cast<Editor*>(handle);
            delete editor->widget;
            delete editor;
        }, portEvent, nullptr
    };
    return index == 0 ? &descriptor : nullptr;
}
