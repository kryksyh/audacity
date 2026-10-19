/*
* Audacity: A Digital Audio Editor
*/
#include <tracktion_engine/tracktion_engine.h>

#include "tracktionlv2settings.h"

#include <map>
#include <cmath>
#include <wx/base64.h>
#include <QJsonDocument>
#include <QJsonObject>

#include "au3-components/EffectInterface.h"
#include "global/translation.h"
#include "au3wrap/internal/wxtypes_convert.h"

using namespace au::tracktionaudio;
namespace effects = au::effects;
using Port = juce::AudioPluginExtensions::LV2Client::ControlPortInfo;

struct TracktionLv2Settings::Layout {
    std::vector<Port> ports;
    std::map<muse::String, size_t> symbols;
    std::map<wxString, size_t> names;
    wxString stateKey;
    wxString valuesKey;
};

namespace {
const TracktionLv2Settings* valuesFor(const EffectSettings& settings)
{
    const auto* values = settings.cast<TracktionLv2Settings>();
    return values && values->layout && values->values.size() == values->layout->ports.size() ? values : nullptr;
}

wxString privateKey(const std::vector<Port>& ports, wxString key)
{
    while (std::any_of(ports.begin(), ports.end(), [&](const auto& port) {
        return CommandParameters::NormalizeName(wxString::FromUTF8(port.name.toStdString())) == key;
    })) {
        key += "_";
    }
    return key;
}

muse::String text(const juce::String& value)
{
    return muse::String::fromStdString(value.toStdString());
}

const Port* portFor(const TracktionLv2Settings& settings, const muse::String& symbol)
{
    const auto found = settings.layout->symbols.find(symbol);
    return found == settings.layout->symbols.end() ? nullptr : &settings.layout->ports[found->second];
}

muse::String formatValue(const effects::ParameterInfo& info, double value)
{
    if (info.type == effects::ParameterType::Dropdown) {
        for (size_t index = 0; index < info.enumIndices.size(); ++index) {
            if (std::abs(info.enumIndices[index] - value) < 0.001) {
                return info.enumValues[index];
            }
        }
    }
    return muse::String::number(value, info.numDecimals());
}
}

EffectSettings au::tracktionaudio::makeTracktionLv2Settings(juce::AudioPluginInstance& processor, const juce::MemoryBlock* state)
{
    auto* client = processor.getLV2Client();
    if (!client) {
        return {};
    }
    auto result = EffectSettings::Make<TracktionLv2Settings>();
    auto& values = *result.cast<TracktionLv2Settings>();
    auto layout = std::make_shared<TracktionLv2Settings::Layout>();
    layout->ports = client->getControlPorts();
    layout->stateKey = privateKey(layout->ports, "__LV2State");
    layout->valuesKey = privateKey(layout->ports, "__LV2Ports");
    for (size_t index = 0; index < layout->ports.size(); ++index) {
        const auto& port = layout->ports[index];
        layout->symbols.emplace(text(port.symbol), index);
        if (port.isInput) {
            layout->names[CommandParameters::NormalizeName(wxString::FromUTF8(port.name.toStdString()))] = index;
        }
        values.values.push_back(port.defaultValue);
    }
    values.layout = std::move(layout);
    captureTracktionLv2Settings(processor, result, state);
    return result;
}

bool au::tracktionaudio::saveTracktionLv2Settings(const EffectSettings& settings, CommandParameters& parameters)
{
    const auto* values = valuesFor(settings);
    if (!values) {
        return false;
    }
    QJsonObject bySymbol;
    for (size_t index = 0; index < values->values.size(); ++index) {
        if (values->layout->ports[index].isInput && !std::isfinite(values->values[index])) {
            return false;
        }
    }
    for (size_t index = 0; index < values->values.size(); ++index) {
        const auto& port = values->layout->ports[index];
        if (port.isInput) {
            parameters.Write(wxString::FromUTF8(port.name.toStdString()), values->values[index]);
            bySymbol.insert(QString::fromStdString(port.symbol.toStdString()), values->values[index]);
        }
    }
    // Symbols preserve separate values when display names are the same.
    const auto encoded = QJsonDocument(bySymbol).toJson(QJsonDocument::Compact).toBase64();
    parameters.Write(values->layout->valuesKey, wxString::FromUTF8(encoded.toStdString()));
    if (values->state) {
        parameters.Write(values->layout->stateKey, wxBase64Encode(values->state->data(), values->state->size()));
    }
    return true;
}

bool au::tracktionaudio::loadTracktionLv2Settings(const CommandParameters& parameters, EffectSettings& settings)
{
    const auto* previous = valuesFor(settings);
    if (!previous) {
        return false;
    }
    auto updated = *previous;
    const bool symbolic = parameters.HasEntry(updated.layout->valuesKey);
    const bool hasState = parameters.HasEntry(updated.layout->stateKey);
    if (symbolic) {
        wxString encoded;
        if (!parameters.Read(updated.layout->valuesKey, &encoded)) {
            return false;
        }
        QJsonParseError error;
        const auto decoded = QByteArray::fromBase64Encoding(QByteArray::fromStdString(encoded.ToStdString()),
                                                            QByteArray::AbortOnBase64DecodingErrors);
        if (!decoded) {
            return false;
        }
        const auto document = QJsonDocument::fromJson(decoded.decoded, &error);
        if (error.error != QJsonParseError::NoError || !document.isObject()) {
            return false;
        }
        const auto object = document.object();
        for (size_t index = 0; index < updated.values.size(); ++index) {
            const auto& port = updated.layout->ports[index];
            if (!port.isInput) {
                continue;
            }
            const auto value = object.value(QString::fromStdString(port.symbol.toStdString()));
            if (!value.isDouble() || !std::isfinite(static_cast<float>(value.toDouble()))) {
                return false;
            }
            updated.values[index] = static_cast<float>(value.toDouble());
        }
    }
    for (const auto& [name, last] : updated.layout->names) {
        if (symbolic && !parameters.HasEntry(name)) {
            continue;
        }
        double value = 0.0;
        if (!parameters.Read(name, &value) || !std::isfinite(value)) {
            return false;
        }
        if (symbolic) {
            CommandParameters canonical;
            canonical.Write(name, updated.values[last]);
            double savedValue = 0.0;
            canonical.Read(name, &savedValue);
            if (value == savedValue) {
                continue;
            }
        }
        for (size_t index = 0; index < updated.values.size(); ++index) {
            const auto& port = updated.layout->ports[index];
            if (port.isInput && CommandParameters::NormalizeName(wxString::FromUTF8(port.name.toStdString())) == name) {
                if (value < port.minimum || value > port.maximum) {
                    return false;
                }
                updated.values[index] = static_cast<float>(value);
            }
        }
    }
    if (hasState) {
        const auto decoded = wxBase64Decode(parameters.Read(updated.layout->stateKey));
        if (decoded.GetDataLen() == 0) {
            return false;
        }
        const auto* bytes = static_cast<const unsigned char*>(decoded.GetData());
        updated.state = std::make_shared<const TracktionLv2Settings::State>(bytes, bytes + decoded.GetDataLen());
    }
    *settings.cast<TracktionLv2Settings>() = std::move(updated);
    return true;
}

bool au::tracktionaudio::applyTracktionLv2Settings(juce::AudioPluginInstance& processor, const EffectSettings& settings, bool restoreState)
{
    const auto* values = valuesFor(settings);
    auto* client = processor.getLV2Client();
    if (!client || !values) {
        return false;
    }
    const juce::ScopedLock lock(processor.getCallbackLock());
    if (restoreState && values->state) {
        const juce::MemoryBlock state(values->state->data(), values->state->size());
        if (!client->restoreState(state)) {
            return false;
        }
    }
    const auto rate = client->getInstanceSampleRate();
    for (size_t index = 0; index < values->values.size(); ++index) {
        const auto& port = values->layout->ports[index];
        if (port.isInput && !client->setControlPortValue(port.index, values->values[index] * (port.isSampleRate ? rate : 1.0))) {
            return false;
        }
    }
    return true;
}

bool au::tracktionaudio::captureTracktionLv2Settings(juce::AudioPluginInstance& processor, EffectSettings& settings,
                                                     const juce::MemoryBlock* suppliedState)
{
    auto* values = settings.cast<TracktionLv2Settings>();
    auto* client = processor.getLV2Client();
    if (!valuesFor(settings) || !client) {
        return false;
    }
    const juce::ScopedLock lock(processor.getCallbackLock());
    const auto rate = client->getInstanceSampleRate();
    for (size_t index = 0; index < values->values.size(); ++index) {
        const auto& port = values->layout->ports[index];
        const auto value = client->getControlPortValue(port.index);
        if (!value) {
            return false;
        }
        values->values[index] = *value / (port.isSampleRate ? rate : 1.0);
    }
    juce::MemoryBlock state;
    if (!suppliedState) {
        processor.getStateInformation(state);
        suppliedState = &state;
    }
    if (suppliedState->getSize() != 0) {
        const auto* bytes = static_cast<const unsigned char*>(suppliedState->getData());
        if (!values->state || values->state->size() != suppliedState->getSize()
            || !std::equal(values->state->begin(), values->state->end(), bytes)) {
            values->state = std::make_shared<const TracktionLv2Settings::State>(bytes, bytes + suppliedState->getSize());
        }
    }
    return true;
}

bool au::tracktionaudio::sameTracktionLv2State(const EffectSettings& first, const EffectSettings& second)
{
    const auto* left = valuesFor(first);
    const auto* right = valuesFor(second);
    return left && right && (left->state == right->state || (left->state && right->state && *left->state == *right->state));
}

effects::ParameterInfo au::tracktionaudio::tracktionLv2Parameter(const juce::AudioPluginInstance& processor, const EffectSettings& layout,
                                                                 const muse::String& symbol)
{
    const auto* values = valuesFor(layout);
    const auto* client = processor.getLV2Client();
    const auto* port = values ? portFor(*values, symbol) : nullptr;
    if (!client || !port || !port->isInput) {
        return {};
    }
    const auto current = client->getControlPortValue(port->index);
    if (!current) {
        return {};
    }
    const auto factor = port->isSampleRate ? client->getInstanceSampleRate() : 1.0;
    effects::ParameterInfo info;
    info.id = symbol;
    info.name = text(port->name);
    info.group = port->group.isEmpty() ? muse::mtrc("effects/lv2", "Effect Settings") : text(port->group);
    info.units = text(port->units);
    info.minValue = port->minimum * factor;
    info.maxValue = port->maximum * factor;
    info.defaultValue = port->defaultValue * factor;
    info.currentValue = *current;
    info.isHidden = port->isTrigger;
    info.isInteger = port->isInteger;
    info.isLogarithmic = port->isLogarithmic;
    info.type = effects::ParameterType::Slider;
    if (port->isToggle) {
        info.type = effects::ParameterType::Toggle;
        info.minValue = 0.0;
        info.maxValue = 1.0;
        info.stepCount = 1;
        info.stepSize = 1.0;
        info.isInteger = true;
        info.currentValue = *current > 0.0f ? 1.0 : 0.0;
    } else if (port->isEnumeration && !port->scalePoints.empty()) {
        info.type = effects::ParameterType::Dropdown;
        size_t nearest = 0;
        for (size_t index = 0; index < port->scalePoints.size(); ++index) {
            const auto& point = port->scalePoints[index];
            info.enumValues.push_back(text(point.second));
            info.enumIndices.push_back(point.first * factor);
            if (std::abs(info.enumIndices[index] - *current) < std::abs(info.enumIndices[nearest] - *current)) {
                nearest = index;
            }
        }
        info.currentValue = info.enumIndices[nearest];
    } else if (port->isInteger) {
        info.stepSize = 1.0;
    }
    info.currentValueString = formatValue(info, info.currentValue);
    return info;
}

effects::ParameterInfoList au::tracktionaudio::tracktionLv2Parameters(const juce::AudioPluginInstance& processor,
                                                                      const EffectSettings& layout)
{
    const auto* values = valuesFor(layout);
    effects::ParameterInfoList result;
    if (values) {
        for (const auto& port : values->layout->ports) {
            if (port.isInput && !port.isTrigger) {
                result.push_back(tracktionLv2Parameter(processor, layout, text(port.symbol)));
            }
        }
    }
    return result;
}

bool au::tracktionaudio::setTracktionLv2Parameter(juce::AudioPluginInstance& processor, const EffectSettings& layout,
                                                  const muse::String& symbol, double value)
{
    const auto* values = valuesFor(layout);
    auto* client = processor.getLV2Client();
    const auto* port = values ? portFor(*values, symbol) : nullptr;
    if (!client || !port || !port->isInput || !std::isfinite(value)) {
        return false;
    }
    const auto factor = port->isSampleRate ? client->getInstanceSampleRate() : 1.0;
    value /= factor;
    if (port->isToggle) {
        value = value > 0.0 ? 1.0 : 0.0;
    } else {
        if (port->hasMinimum) {
            value = std::max(value, static_cast<double>(port->minimum));
        }
        if (port->hasMaximum) {
            value = std::min(value, static_cast<double>(port->maximum));
        }
        if (port->isInteger && !port->isEnumeration) {
            value = std::round(value);
        }
    }
    return client->setControlPortValue(port->index, static_cast<float>(value * factor));
}

muse::String au::tracktionaudio::tracktionLv2ParameterValueString(const juce::AudioPluginInstance& processor, const EffectSettings& layout,
                                                                  const muse::String& symbol, double value)
{
    const auto info = tracktionLv2Parameter(processor, layout, symbol);
    return info.isValid() ? formatValue(info, value) : muse::String{};
}
