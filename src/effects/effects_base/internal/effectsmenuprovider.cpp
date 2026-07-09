/*
 * Audacity: A Digital Audio Editor
 */
#include "effectsmenuprovider.h"
#include "effectsutils.h"

#include "global/log.h"

namespace au::effects {
void EffectsMenuProvider::init()
{
    configuration()->effectMenuOrganizationChanged().onNotify(this, [this] {
        m_effectMenusChanged.notify();
    });

    effectsProvider()->effectMetaListChanged().onNotify(this, [this] { m_effectMenusChanged.notify(); });
}

muse::uicomponents::MenuItemList EffectsMenuProvider::destructiveEffectMenu(IEffectMenuItemFactory& effectMenu, EffectFilter filterType)
{
    // Make sure all OpenVINO effects are grouped in the same menu under /Effect,
    // whatever their type (same special case as in Audacity 3)
    auto isOpenVino = [](const EffectMeta& meta) { return meta.vendor == u"OpenVINO AI Effects"; };

    std::function<bool(const EffectMeta&)> filter;
    switch (filterType) {
    case EffectFilter::GeneratorsOnly:
        filter = [=](const EffectMeta& meta) { return meta.type != EffectType::Generator || isOpenVino(meta); };
        break;
    case EffectFilter::RealtimeProcessorsOnly:
        LOGE() << "Unexpected filter type RealtimeProcessorsOnly for destructiveEffectMenu";
    case EffectFilter::ProcessorsOnly:
        filter = [=](const EffectMeta& meta) { return meta.type != EffectType::Processor && !isOpenVino(meta); };
        break;
    case EffectFilter::ToolsOnly:
        filter = [=](const EffectMeta& meta) { return meta.type != EffectType::Tool || isOpenVino(meta); };
        break;
    case EffectFilter::AnalyzersOnly:
        filter = [=](const EffectMeta& meta) { return meta.type != EffectType::Analyzer || isOpenVino(meta); };
        break;
    default:
        LOGE() << "EffectsMenuProvider::destructiveEffectMenu: Unknown filter type:" << static_cast<int>(filterType);
        break;
    }

    IF_ASSERT_FAILED(filter) {
        return {};
    }

    return au::effects::utils::destructiveEffectMenu(configuration()->effectMenuOrganization(),
                                                     effectsProvider()->effectMetaList(), std::move(filter), effectMenu);
}

muse::uicomponents::MenuItemList EffectsMenuProvider::realtimeEffectMenu(IEffectMenuItemFactory& effectMenu)
{
    auto filter = [](const EffectMeta& meta) { return !(meta.type == EffectType::Processor && meta.isRealtimeCapable); };
    return au::effects::utils::realtimeEffectMenu(configuration()->effectMenuOrganization(),
                                                  effectsProvider()->effectMetaList(), std::move(filter), effectMenu);
}

muse::async::Notification EffectsMenuProvider::effectMenusChanged() const
{
    return m_effectMenusChanged;
}
}
