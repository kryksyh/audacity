/*
* Audacity: A Digital Audio Editor
*/
#pragma once

#include <string>
#include <vector>

#include "au3-components/ComponentInterfaceSymbol.h"
#include "au3-effects/StatefulEffect.h"

#include "pluginhost/api/audacity_plugin.h"

namespace au::effects {

//! Presents a stable-ABI effect as an AU3 effect: the generated settings UI
//! is driven by the plugin's parameter model via VisitSettings, and Process
//! runs the plugin's offline-processing capability over the selection.
class PluginEffectAdapter final : public StatefulEffect
{
public:
    PluginEffectAdapter(const auplug_effect_factory_t& factory, const auplug_effect_desc_t& desc);
    ~PluginEffectAdapter() override;

    //! False if the plugin failed to create the effect instance
    bool isValid() const { return m_fx != nullptr; }

    static PluginPath pathForId(const char* effectId);

    // ComponentInterface implementation
    PluginPath GetPath() const override;
    ComponentInterfaceSymbol GetSymbol() const override;
    VendorSymbol GetVendor() const override;
    wxString GetVersion() const override;
    TranslatableString GetDescription() const override;

    // EffectDefinitionInterface implementation
    ::EffectType GetType() const override;
    EffectFamilySymbol GetFamily() const override;
    bool IsInteractive() const override;

    bool VisitSettings(SettingsVisitor& visitor, EffectSettings& settings) override;
    bool VisitSettings(ConstSettingsVisitor& visitor, const EffectSettings& settings) const override;

    bool Process(::EffectInstance& instance, ::EffectSettings& settings) override;

    //! Display label for a settings key; empty if the key is unknown
    wxString labelForKey(const wxString& key) const;

private:
    //! Owned mirror of one plugin parameter: the ABI only guarantees
    //! param-info strings until the next get_info call, so everything is
    //! deep-copied here.
    struct Param {
        auplug_param_id_t id = 0; // used at the C boundary; key/wxKey are host-only
        std::string key;          // UTF-8, persisted settings key / QML lookup name
        wxString wxKey;           // the same key for the AU3 visitors
        wxString label;
        int32_t type = AUPLUG_PARAM_DOUBLE;
        double minValue = 0.0;
        double maxValue = 0.0;
        double defaultValue = 0.0;
        wxString defaultString;

        double num = 0.0;
        int inum = 0;
        bool flag = false;
        wxString str;
        std::vector<EnumValueSymbol> symbols;
    };

    template<typename Self, typename Visitor>
    static bool visitImpl(Self& self, Visitor& visitor);

    void initParams();
    void pushParams();

    auplug_effect_desc_t m_desc {};
    auplug_effect_t* m_fx = nullptr;
    const auplug_params_ext_t* m_paramsExt = nullptr;
    std::vector<Param> m_params;
};
}
