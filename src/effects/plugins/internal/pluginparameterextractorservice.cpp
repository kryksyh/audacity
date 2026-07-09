/*
* Audacity: A Digital Audio Editor
*/
#include "pluginparameterextractorservice.h"

#include <algorithm>
#include <cmath>

#include "au3wrap/internal/wxtypes_convert.h"

#include "au3-components/EffectInterface.h"
#include "au3-components/SettingsVisitor.h"
#include "au3-effects/StatefulEffectBase.h"

#include "plugineffectadapter.h"

#include "framework/global/log.h"

namespace au::effects {
namespace {
::EffectSettingsManager* settingsManager(EffectInstance* instance)
{
    auto* statefulInstance = dynamic_cast<::StatefulEffectBase::Instance*>(instance);
    if (!statefulInstance) {
        return nullptr;
    }
    return dynamic_cast<::EffectSettingsManager*>(&statefulInstance->GetEffect());
}

//! Records every visited parameter as ParameterInfo for the generated UI.
class ParamCollector : public ConstSettingsVisitor
{
public:
    ParameterInfoList params;

    void Define(bool var, const wxChar* key, bool vdefault, bool, bool, bool) override
    {
        ParameterInfo p = base(key);
        p.type = ParameterType::Toggle;
        p.minValue = 0.0;
        p.maxValue = 1.0;
        p.stepCount = 1;
        p.defaultValue = vdefault ? 1.0 : 0.0;
        p.currentValue = var ? 1.0 : 0.0;
        params.push_back(std::move(p));
    }

    void Define(size_t var, const wxChar* key, int vdefault, int vmin, int vmax, int vscl) override
    {
        defineInt(static_cast<int>(var), key, vdefault, vmin, vmax, vscl);
    }

    void Define(int var, const wxChar* key, int vdefault, int vmin, int vmax, int vscl) override
    {
        defineInt(var, key, vdefault, vmin, vmax, vscl);
    }

    void Define(float var, const wxChar* key, float vdefault, float vmin, float vmax, float) override
    {
        defineFloat(var, key, vdefault, vmin, vmax);
    }

    void Define(double var, const wxChar* key, float vdefault, float vmin, float vmax, float) override
    {
        defineFloat(var, key, vdefault, vmin, vmax);
    }

    void Define(double var, const wxChar* key, double vdefault, double vmin, double vmax, double) override
    {
        defineFloat(var, key, vdefault, vmin, vmax);
    }

    void Define(const wxString& var, const wxChar* key, wxString, wxString, wxString, wxString) override
    {
        ParameterInfo p = base(key);
        p.type = ParameterType::Text;
        p.currentValueString = au3::wxToString(var);
        params.push_back(std::move(p));
    }

    void DefineEnum(int var, const wxChar* key, int vdefault, const EnumValueSymbol strings[], size_t nStrings) override
    {
        ParameterInfo p = base(key);
        p.type = ParameterType::Dropdown;
        p.minValue = 0.0;
        p.maxValue = nStrings > 0 ? static_cast<double>(nStrings - 1) : 0.0;
        p.stepCount = static_cast<int>(nStrings);
        p.isInteger = true;
        p.defaultValue = vdefault;
        p.currentValue = var;
        for (size_t i = 0; i < nStrings; ++i) {
            const wxString label = strings[i].Translation();
            p.enumValues.push_back(au3::wxToString(label.empty() ? strings[i].Internal() : label));
            p.enumIndices.push_back(static_cast<double>(i));
        }
        params.push_back(std::move(p));
    }

private:
    ParameterInfo base(const wxChar* key) const
    {
        ParameterInfo p;
        p.id = au3::wxToString(wxString(key));
        p.name = p.id;
        return p;
    }

    void defineInt(int var, const wxChar* key, int vdefault, int vmin, int vmax, int)
    {
        ParameterInfo p = base(key);
        p.type = ParameterType::Numeric;
        p.isInteger = true;
        p.stepSize = 1.0;
        p.minValue = vmin;
        p.maxValue = vmax;
        p.defaultValue = vdefault;
        p.currentValue = var;
        params.push_back(std::move(p));
    }

    void defineFloat(double var, const wxChar* key, double vdefault, double vmin, double vmax)
    {
        ParameterInfo p = base(key);
        p.type = ParameterType::Slider;
        p.minValue = vmin;
        p.maxValue = vmax;
        p.defaultValue = vdefault;
        p.currentValue = var;
        params.push_back(std::move(p));
    }
};

//! Writes a new value into the effect member bound to the given key.
class ParamSetter : public SettingsVisitor
{
public:
    ParamSetter(const wxString& key, double value)
        : m_key(key), m_value(value) {}

    ParamSetter(const wxString& key, const wxString& str)
        : m_key(key), m_isString(true), m_string(str) {}

    bool found() const { return m_found; }

    void Define(bool& var, const wxChar* key, bool, bool, bool, bool) override
    {
        if (matches(key)) {
            var = m_value != 0.0;
        }
    }

    void Define(size_t& var, const wxChar* key, int, int vmin, int vmax, int) override
    {
        if (matches(key)) {
            var = static_cast<size_t>(clampInt(vmin, vmax));
        }
    }

    void Define(int& var, const wxChar* key, int, int vmin, int vmax, int) override
    {
        if (matches(key)) {
            var = clampInt(vmin, vmax);
        }
    }

    void Define(float& var, const wxChar* key, float, float vmin, float vmax, float) override
    {
        if (matches(key)) {
            var = static_cast<float>(std::clamp(m_value, double(vmin), double(vmax)));
        }
    }

    void Define(double& var, const wxChar* key, float, float vmin, float vmax, float) override
    {
        if (matches(key)) {
            var = std::clamp(m_value, double(vmin), double(vmax));
        }
    }

    void Define(double& var, const wxChar* key, double, double vmin, double vmax, double) override
    {
        if (matches(key)) {
            var = std::clamp(m_value, vmin, vmax);
        }
    }

    void Define(wxString& var, const wxChar* key, wxString, wxString, wxString, wxString) override
    {
        if (m_isString && matches(key)) {
            var = m_string;
        }
    }

    void DefineEnum(int& var, const wxChar* key, int, const EnumValueSymbol*, size_t nStrings) override
    {
        if (matches(key)) {
            var = clampInt(0, nStrings > 0 ? static_cast<int>(nStrings - 1) : 0);
        }
    }

private:
    bool matches(const wxChar* key)
    {
        const bool m = !m_found && m_key == wxString(key);
        if (m) {
            m_found = true;
        }
        return m;
    }

    int clampInt(int vmin, int vmax) const
    {
        return std::clamp(static_cast<int>(std::lround(m_value)), vmin, vmax);
    }

    wxString m_key;
    double m_value = 0.0;
    bool m_isString = false;
    wxString m_string;
    bool m_found = false;
};

ParameterInfoList collect(EffectInstance* instance, const EffectSettingsAccessPtr& settingsAccess)
{
    ::EffectSettingsManager* manager = settingsManager(instance);
    if (!manager) {
        LOGW() << "not a stateful AU3 effect instance";
        return {};
    }

    ParamCollector collector;
    if (settingsAccess) {
        manager->VisitSettings(collector, settingsAccess->Get());
    } else {
        const ::EffectSettings dummy;
        manager->VisitSettings(collector, dummy);
    }

    // The visitor protocol carries no display labels; plugin effects declare
    // them in their parameter model
    if (const auto* adapter = dynamic_cast<const PluginEffectAdapter*>(manager)) {
        for (ParameterInfo& p : collector.params) {
            const wxString label = adapter->labelForKey(au3::wxFromString(p.id));
            if (!label.empty()) {
                p.name = au3::wxToString(label);
            }
        }
    }

    return std::move(collector.params);
}
} // namespace

ParameterInfoList PluginParameterExtractorService::extractParameters(EffectInstance* instance,
                                                                     EffectSettingsAccessPtr settingsAccess) const
{
    return collect(instance, settingsAccess);
}

ParameterInfo PluginParameterExtractorService::getParameter(EffectInstance* instance, const muse::String& parameterId) const
{
    ParameterInfoList params = collect(instance, nullptr);
    const auto it = std::find_if(params.begin(), params.end(), [&](const ParameterInfo& p) {
        return p.id == parameterId;
    });
    return it != params.end() ? *it : ParameterInfo();
}

double PluginParameterExtractorService::getParameterValue(EffectInstance* instance, const muse::String& parameterId) const
{
    return getParameter(instance, parameterId).currentValue;
}

bool PluginParameterExtractorService::setParameterValue(EffectInstance* instance, const muse::String& parameterId,
                                                        double fullRangeValue, EffectSettingsAccessPtr settingsAccess)
{
    ::EffectSettingsManager* manager = settingsManager(instance);
    if (!manager) {
        return false;
    }

    ParamSetter setter(au3::wxFromString(parameterId), fullRangeValue);
    if (settingsAccess) {
        settingsAccess->ModifySettings([&](::EffectSettings& settings) {
            manager->VisitSettings(setter, settings);
            return nullptr;
        });
    } else {
        ::EffectSettings dummy;
        manager->VisitSettings(setter, dummy);
    }
    return setter.found();
}

bool PluginParameterExtractorService::setParameterStringValue(EffectInstance* instance, const muse::String& parameterId,
                                                              const muse::String& stringValue, EffectSettingsAccessPtr settingsAccess)
{
    ::EffectSettingsManager* manager = settingsManager(instance);
    if (!manager) {
        return false;
    }

    ParamSetter setter(au3::wxFromString(parameterId), au3::wxFromString(stringValue));
    if (settingsAccess) {
        settingsAccess->ModifySettings([&](::EffectSettings& settings) {
            manager->VisitSettings(setter, settings);
            return nullptr;
        });
    } else {
        ::EffectSettings dummy;
        manager->VisitSettings(setter, dummy);
    }
    return setter.found();
}

muse::String PluginParameterExtractorService::getParameterValueString(EffectInstance* instance, const muse::String& parameterId,
                                                                      double value) const
{
    const ParameterInfo param = getParameter(instance, parameterId);
    if (!param.isValid()) {
        return muse::String();
    }
    if (param.type == ParameterType::Dropdown) {
        const size_t idx = static_cast<size_t>(std::lround(value));
        return idx < param.enumValues.size() ? param.enumValues[idx] : muse::String();
    }
    if (param.isInteger) {
        return muse::String::number(static_cast<int>(std::lround(value)));
    }
    return muse::String::number(value, param.numDecimals());
}
} // namespace au::effects
