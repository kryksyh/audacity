/*
* Audacity: A Digital Audio Editor
*/
#pragma once

#include "audio/iinstrumenteditor.h"
#include "audio/ipluginautomation.h"

namespace au::tracktionaudio {
class TracktionInstrumentEditorFactory final : public audio::IInstrumentEditorFactory, public audio::IPluginAutomation
{
public:
    std::vector<audio::InstrumentParameter> parameters(RealtimeEffectState& effect) const override;
    muse::RetVal<std::shared_ptr<audio::IInstrumentEditor> > create(const std::string& description,
                                                                    const std::string& state) const override;
};
}
