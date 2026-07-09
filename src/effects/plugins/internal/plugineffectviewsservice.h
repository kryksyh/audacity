/*
* Audacity: A Digital Audio Editor
*/
#pragma once

#include <utility>
#include <vector>

#include "effects/effects_base/iplugineffectviews.h"

namespace au::effects {
class PluginEffectViewsService : public IPluginEffectViews
{
public:
    //! effect symbol -> QML source text
    using EffectViews = std::vector<std::pair<muse::String, muse::String> >;

    void setViews(EffectViews views) { m_views = std::move(views); }

    muse::String effectViewQml(const muse::String& effectSymbol) const override;

private:
    EffectViews m_views;
};
}
