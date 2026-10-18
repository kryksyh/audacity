/*
* Audacity: A Digital Audio Editor
*/
#include "nativeeffectparameters.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>
#include <QString>

#include "au3-components/EffectInterface.h"
#include "au3-components/SettingsVisitor.h"
#include "au3-realtime-effects/RealtimeEffectState.h"

#if AU_NATIVE_LV2
#include "au3-lv2/LV2EffectBase.h"
#endif
#if AU_NATIVE_AUDIO_UNIT
#include "au3-audio-unit/AudioUnitEffectBase.h"
#include "au3-audio-unit/AudioUnitInstance.h"
#endif
#if AU_NATIVE_VST
#include "au3-vst3/VST3EffectBase.h"
#include "au3-vst3/VST3Utils.h"
#include "au3-vst3/VST3Wrapper.h"
#include "au3-vst3/VST3Instance.h"
#include <public.sdk/source/vst/hosting/module.h>
#endif

namespace au::au3audio {
namespace {
std::optional<uint32_t> parameterId(const std::string& id)
{
    try {
        size_t used = 0;
        const auto value = std::stoul(id, &used);
        if (used == id.size() && value <= std::numeric_limits<uint32_t>::max()) {
            return static_cast<uint32_t>(value);
        }
    } catch (const std::exception&) {}
    return std::nullopt;
}

double normalize(double value, double minimum, double maximum, bool logarithmic = false)
{
    if (!std::isfinite(value) || maximum <= minimum) {
        return 0.0;
    }
    if (logarithmic && minimum > 0.0) {
        return std::clamp(std::log(std::max(value, minimum) / minimum) / std::log(maximum / minimum), 0.0, 1.0);
    }
    return std::clamp((value - minimum) / (maximum - minimum), 0.0, 1.0);
}

class ParameterVisitor final : public ConstSettingsVisitor
{
public:
    std::vector<audio::InstrumentParameter> result;

    void number(double value, const wxChar* key, double minimum, double maximum)
    {
        if (maximum <= minimum) {
            return;
        }
        const auto name = wxString(key).utf8_string();
        result.push_back({ name, name, QString::number(value, 'g', 6).toStdString(), normalize(value, minimum, maximum) });
    }

    void Define(bool value, const wxChar* key, bool, bool, bool, bool) override { number(value, key, 0.0, 1.0); }
    void Define(size_t value, const wxChar* key, int, int minimum, int maximum, int) override { number(value, key, minimum, maximum); }
    void Define(int value, const wxChar* key, int, int minimum, int maximum, int) override { number(value, key, minimum, maximum); }
    void Define(float value, const wxChar* key, float, float minimum, float maximum, float) override
    {
        number(value, key, minimum, maximum);
    }

    void Define(double value, const wxChar* key, float, float minimum, float maximum, float) override
    {
        number(value, key, minimum, maximum);
    }

    void Define(double value, const wxChar* key, double, double minimum, double maximum, double) override
    {
        number(value, key, minimum, maximum);
    }

    void DefineEnum(int value, const wxChar* key, int, const EnumValueSymbol[], size_t count) override
    {
        if (count) {
            number(value, key, 0.0, count - 1);
        }
    }
};

class BindingVisitor final : public SettingsVisitor
{
public:
    explicit BindingVisitor(const std::string& id)
        : m_id(wxString::FromUTF8(id)) {}
    NativeEffectParameterBinding result;
    template<typename T> void number(T& target, const wxChar* key, double minimum, double maximum)
    {
        if (m_id != key || maximum <= minimum) {
            return;
        }
        result.beforeProcessStart = true;
        result.set = [&target, minimum, maximum](double normalized) {
            const auto value = minimum + std::clamp(normalized, 0.0, 1.0) * (maximum - minimum);
            if constexpr (std::is_integral_v<T>) {
                target = static_cast<T>(std::round(value));
            } else {
                target = value;
            }
        };
    }

    void Define(bool& value, const wxChar* key, bool, bool, bool, bool) override { number(value, key, 0.0, 1.0); }
    void Define(size_t& value, const wxChar* key, int, int minimum, int maximum, int) override { number(value, key, minimum, maximum); }
    void Define(int& value, const wxChar* key, int, int minimum, int maximum, int) override { number(value, key, minimum, maximum); }
    void Define(float& value, const wxChar* key, float, float minimum, float maximum, float) override
    {
        number(value, key, minimum, maximum);
    }

    void Define(double& value, const wxChar* key, float, float minimum, float maximum, float) override
    {
        number(value, key, minimum, maximum);
    }

    void Define(double& value, const wxChar* key, double, double minimum, double maximum, double) override
    {
        number(value, key, minimum, maximum);
    }

    void DefineEnum(int& value, const wxChar* key, int, const EnumValueSymbol[], size_t count) override
    {
        if (count) {
            number(value, key, 0.0, count - 1);
        }
    }

private:
    wxString m_id;
};
}

NativeEffectParameterBinding bindNativeEffectParameter(const EffectInstanceFactory& effect, EffectSettings& settings,
                                                       EffectInstance& instance, const std::string& id, double sampleRate)
{
#if AU_NATIVE_LV2
    if (const auto* lv2 = dynamic_cast<const LV2EffectBase*>(&effect)) {
        for (size_t i = 0; i < lv2->mPorts.mControlPorts.size(); ++i) {
            auto port = lv2->mPorts.mControlPorts[i];
            if (port->mIsInput && port->mSymbol == wxString::FromUTF8(id)) {
                auto* target = &GetSettings(settings).values[i];
                return { [target, port, sampleRate](double normalized) {
                        normalized = std::clamp(normalized, 0.0, 1.0);
                        double value = port->mLogarithmic && port->mMin > 0.0f && port->mMax > 0.0f
                                       ? std::exp(std::log(port->mMin) + normalized * std::log(port->mMax / port->mMin))
                                       : port->mMin + normalized * (port->mMax - port->mMin);
                        if (port->mToggle) {
                            value = normalized >= 0.5 ? port->mMax : port->mMin;
                        } else if (port->mEnumeration && !port->mScaleValues.empty()) {
                            value = port->mScaleValues[port->Discretize(value)];
                        } else if (port->mInteger) {
                            value = std::round(value);
                        }
                        *target = value * (port->mSampleRate ? sampleRate : 1.0);
                    }, true };
            }
        }
        return {};
    }
#endif
#if AU_NATIVE_AUDIO_UNIT
    if (auto* au = dynamic_cast<AudioUnitInstance*>(&instance)) {
        const auto parsed = parameterId(id);
        if (!parsed) {
            return {};
        }
        const AudioUnitParameterID parameter = *parsed;
        const AudioUnitWrapper::ParameterInfo info(au->GetAudioUnit(), parameter);
        if (!(info.mInfo.flags & kAudioUnitParameterFlag_IsWritable) || info.mInfo.maxValue <= info.mInfo.minValue) {
            return {};
        }
        const auto minimum = info.mInfo.minValue, maximum = info.mInfo.maxValue;
        return { [au, parameter, minimum, maximum](double normalized) {
                au->SetRealtimeParameter(parameter, minimum + std::clamp(normalized, 0.0, 1.0) * (maximum - minimum));
            }, false };
    }
#endif
#if AU_NATIVE_VST
    if (auto* vst = dynamic_cast<VST3Instance*>(&instance)) {
        const auto parsed = parameterId(id);
        if (!parsed) {
            return {};
        }
        const Steinberg::Vst::ParamID parameter = *parsed;
        auto* wrapper = &vst->GetWrapper();
        bool found = false;
        for (Steinberg::int32 i = 0; i < wrapper->mEditController->getParameterCount(); ++i) {
            Steinberg::Vst::ParameterInfo info {};
            if (wrapper->mEditController->getParameterInfo(i, info) == Steinberg::kResultOk && info.id == parameter
                && !(info.flags & Steinberg::Vst::ParameterInfo::kIsReadOnly)) {
                found = true;
            }
        }
        if (!found) {
            return {};
        }
        return { [vst, parameter](double normalized) {
                vst->QueueRealtimeParameterChange(parameter, std::clamp(normalized, 0.0, 1.0));
            }, false };
    }
#endif
    BindingVisitor visitor(id);
    const_cast<EffectInstanceFactory&>(effect).VisitSettings(visitor, settings);
    return std::move(visitor.result);
}

std::vector<audio::InstrumentParameter> nativeEffectParameters(const EffectInstanceFactory& effect, const EffectSettings& settings)
{
    std::vector<audio::InstrumentParameter> result;
#if AU_NATIVE_LV2
    if (const auto* lv2 = dynamic_cast<const LV2EffectBase*>(&effect)) {
        const auto& values = GetSettings(settings).values;
        for (size_t i = 0; i < lv2->mPorts.mControlPorts.size() && i < values.size(); ++i) {
            const auto& port = *lv2->mPorts.mControlPorts[i];
            if (port.mIsInput && port.mMax > port.mMin) {
                result.push_back({ port.mSymbol.utf8_string(), port.mName.utf8_string(),
                                   QString::number(values[i], 'g', 6).toStdString(),
                                   normalize(values[i], port.mMin, port.mMax, port.mLogarithmic) });
            }
        }
        return result;
    }
#endif
#if AU_NATIVE_AUDIO_UNIT
    if (const auto* au = dynamic_cast<const AudioUnitEffectBase*>(&effect)) {
        const auto& values = AudioUnitWrapper::GetSettings(settings).values;
        au->ForEachParameter([&](const auto& parameter, AudioUnitParameterID id) {
            const auto& info = parameter.mInfo;
            const auto value = values.find(id);
            if ((info.flags & kAudioUnitParameterFlag_IsWritable) && info.maxValue > info.minValue
                && value != values.end() && value->second) {
                result.push_back({ std::to_string(id), wxString::FromUTF8(info.name).utf8_string(),
                                   QString::number(value->second->second, 'g', 6).toStdString(),
                                   normalize(value->second->second, info.minValue, info.maxValue) });
            }
            return true;
        });
        return result;
    }
#endif
#if AU_NATIVE_VST
    if (const auto* vst = dynamic_cast<const VST3EffectBase*>(&effect)) {
        wxString path;
        std::string uid;
        if (!VST3Utils::ParsePluginPath(vst->GetPath(), &path, &uid)) {
            return {};
        }
        auto module = vst->vstModule();
        for (const auto& info : module->getFactory().classInfos()) {
            if (info.ID().toString() != uid) {
                continue;
            }
            VST3Wrapper wrapper(*module, info, effect.GetType() == EffectTypeInstrument
                                ? VST3Wrapper::Role::Instrument : VST3Wrapper::Role::Effect);
            wrapper.InitializeComponents();
            auto saved = settings;
            wrapper.FetchSettings(saved, false);
            auto controller = wrapper.mEditController;
            const auto midiEvents = VST3Utils::MidiEventParameters(controller);
            for (Steinberg::int32 i = 0; i < controller->getParameterCount(); ++i) {
                Steinberg::Vst::ParameterInfo parameter {};
                if (controller->getParameterInfo(i, parameter) != Steinberg::kResultOk
                    || (parameter.flags & (Steinberg::Vst::ParameterInfo::kIsReadOnly | Steinberg::Vst::ParameterInfo::kIsHidden))
                    || midiEvents.contains(parameter.id)) {
                    continue;
                }
                const auto value = controller->getParamNormalized(parameter.id);
                Steinberg::Vst::String128 display {};
                controller->getParamStringByValue(parameter.id, value, display);
                result.push_back({ std::to_string(parameter.id), VST3Utils::UTF16ToStdString(parameter.title),
                                   VST3Utils::UTF16ToStdString(display), value });
            }
            break;
        }
        return result;
    }
#endif
    ParameterVisitor visitor;
    effect.VisitSettings(visitor, settings);
    return visitor.result;
}

namespace {
class NativeProcessingAutomation final : public RealtimeEffectState::ProcessingAutomation
{
public:
    NativeProcessingAutomation(const EffectInstanceFactory& effect, EffectInstance& instance,
                               EffectSettings& settings, const AutomationData& automation, double sampleRate)
        : data(automation)
    {
        for (const auto& lane : data.Lanes()) {
            if (!lane.enabled || lane.points.empty()) {
                continue;
            }
            auto binding = bindNativeEffectParameter(effect, settings, instance, lane.parameter, sampleRate);
            if (binding.set) {
                bindings.push_back({ &lane, std::move(binding) });
            }
        }
    }

    void apply(double time, bool before)
    {
        for (const auto& item : bindings) {
            if (item.binding.beforeProcessStart == before) {
                item.binding.set(AutomationData::ValueAt(item.lane, time, 0.0));
            }
        }
    }

    void BeforeProcessStart(double time) override { apply(time, true); }
    void AfterProcessStart(double time) override { apply(time, false); }
    struct Binding {
        const AutomationLaneData* lane;
        NativeEffectParameterBinding binding;
    };
    AutomationData data;
    std::vector<Binding> bindings;
};
}

void registerNativeEffectAutomation()
{
    static RealtimeEffectState::AutomationFactory::Scope factory {
        [](const EffectInstanceFactory& effect, EffectInstance& instance, EffectSettings& settings, const AutomationData& data,
           double sampleRate)
        -> std::shared_ptr<RealtimeEffectState::ProcessingAutomation> {
            auto result = std::make_shared<NativeProcessingAutomation>(effect, instance, settings, data, sampleRate);
            return result->bindings.empty() ? nullptr : result;
        }
    };
}
}
