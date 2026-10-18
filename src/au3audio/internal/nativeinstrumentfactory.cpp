/*
* Audacity: A Digital Audio Editor
*/
#include "nativeinstrumentfactory.h"

#include "instrumentprocessor.h"
#include "nativeeffectparameters.h"
#include "au3-realtime-effects/RealtimeEffectState.h"

using namespace au::au3audio;

muse::RetVal<std::shared_ptr<au::audio::IInstrumentEditor> > NativeInstrumentFactory::create(const std::string& description,
                                                                                             const std::string& state) const
{
    try {
        return muse::RetVal<std::shared_ptr<audio::IInstrumentEditor> >::make_ok(createInstrumentProcessor(description, state));
    } catch (const std::exception& error) {
        return muse::make_ret(muse::Ret::Code::InternalError, error.what());
    }
}

std::vector<au::audio::InstrumentParameter> NativeInstrumentFactory::parameters(RealtimeEffectState& effect) const
{
    if (!effect.GetEffect()) {
        return {};
    }
    try {
        // Read the pending manual snapshot without waiting for processing or
        // flushing it into the document. GetSettings() may still hold the
        // previous committed preset while a floating editor is open.
        return nativeEffectParameters(*effect.GetEffect(), effect.GetAccess()->Get());
    } catch (const std::exception&) {
        return {};
    }
}
