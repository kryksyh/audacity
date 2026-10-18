/*
* Audacity: A Digital Audio Editor
*/
#pragma once

#include "audio/iinstrumenteditor.h"
#include "audio/ipluginautomation.h"

namespace au::au3audio {
class NativeInstrumentFactory final : public audio::IInstrumentEditorFactory, public audio::IPluginAutomation
{
public:
    muse::RetVal<std::shared_ptr<audio::IInstrumentEditor> > create(const std::string& description,
                                                                    const std::string& state) const override;
    std::vector<audio::InstrumentParameter> parameters(RealtimeEffectState& effect) const override;
};
}
