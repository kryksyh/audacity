/*
* Audacity: A Digital Audio Editor
*/
#include "instrumentprocessor.h"

#include <stdexcept>
#include <exception>
#include <QCoreApplication>
#include <QMetaObject>
#include <QThread>
#include "effects/effects_base/internal/effectsutils.h"

using namespace au::au3audio;

namespace {
template<typename Operation>
auto onInstrumentControlThread(InstrumentProcessor& processor, Operation operation) -> decltype(operation())
{
    if (!processor.requiresMainThreadLatencyRefresh()) {
        return operation();
    }
    const auto application = QCoreApplication::instance();
    if (!application || application->thread() == QThread::currentThread()) {
        return operation();
    }
    decltype(operation()) result {};
    std::exception_ptr failure;
    const bool invoked = QMetaObject::invokeMethod(application, [&] {
        try {
            result = operation();
        } catch (...) {
            failure = std::current_exception();
        }
    }, Qt::BlockingQueuedConnection);
    if (!invoked) {
        throw std::runtime_error("Cannot refresh native instrument delay on the application thread.");
    }
    if (failure) {
        std::rethrow_exception(failure);
    }
    return result;
}
}

bool au::au3audio::refreshInstrumentLatency(InstrumentProcessor& processor)
{
    return onInstrumentControlThread(processor, [&] { return processor.refreshLatency(); });
}

size_t au::au3audio::settleInstrumentLatency(InstrumentProcessor& processor)
{
    return onInstrumentControlThread(processor, [&] {
        const auto rendered = processor.reportedLatency();
        for (unsigned attempt = 0; attempt < 8; ++attempt) {
            processor.refreshLatency();
            if (!processor.latencyUpdatePending()) {
                return rendered;
            }
        }
        throw std::runtime_error("The instrument's processing delay did not stabilize.");
    });
}

std::shared_ptr<InstrumentProcessor> au::au3audio::createInstrumentProcessor(const std::string& description, const std::string& state)
{
    if (description.empty()) {
        return createBuiltinInstrument(state);
    }
    const auto id = muse::String::fromStdString(description);
    const auto family = effects::utils::parseEffectFamily(id);
    const auto path = effects::utils::parseEffectPath(id);
#if AU_NATIVE_VST
    if (family == "VST3") {
        return createVstInstrument(path, state);
    }
#endif
#if AU_NATIVE_AUDIO_UNIT
    if (family == "AudioUnit") {
        return createAudioUnitInstrument(path, state);
    }
#endif
#if AU_NATIVE_LV2
    if (family == "LV2") {
        return createLv2Instrument(path, state);
    }
#endif
    throw std::runtime_error("The instrument format is not available.");
}
