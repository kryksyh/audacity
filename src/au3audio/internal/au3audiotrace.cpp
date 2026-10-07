/*
 * Audacity: A Digital Audio Editor
 */
#include "au3audiotrace.h"

#include <array>

#include "au3-audio-io/AudioIOTrace.h"

#include "shared/perf/perftrace.h"

using namespace au::perf;

namespace {
void drainCallbackRecords()
{
    static const int lane = Tracer::instance().createLane("Audio callback");
    static std::array<AudioIOTrace::CallbackRecord, 1024> records;
    static int64_t lastStartNs = -1;

    Tracer& tracer = Tracer::instance();
    size_t count = 0;
    do {
        uint64_t lost = 0;
        count = AudioIOTrace::ReadCallbackRecords(records.data(), records.size(), lost);
        if (lost > 0) {
            tracer.recordLaneCounter(lane, "callback records lost", records[0].startNs, static_cast<double>(lost));
            lastStartNs = -1;
        }

        for (size_t i = 0; i < count; ++i) {
            const AudioIOTrace::CallbackRecord& r = records[i];
            tracer.recordLaneZone(lane, "audio callback", Category::Audio, r.startNs, r.startNs + r.durNs);

            if (r.sampleRate > 0) {
                const double budgetNs = 1e9 * r.frames / r.sampleRate;
                tracer.recordLaneCounter(lane, "callback load %", r.startNs, 100.0 * r.durNs / budgetNs);
            }
            if (lastStartNs >= 0) {
                tracer.recordLaneCounter(lane, "callback period ms", r.startNs, (r.startNs - lastStartNs) / 1e6);
            }
            lastStartNs = r.startNs;

            if (r.outputPeak >= 0) {
                tracer.recordLaneCounter(lane, "output peak", r.startNs, r.outputPeak);
            }
            if (r.inputPeak >= 0) {
                tracer.recordLaneCounter(lane, "input peak", r.startNs, r.inputPeak);
            }
            if (r.statusFlags != 0) {
                tracer.recordLaneCounter(lane, "pa status flags", r.startNs, r.statusFlags);
            }
            if (r.ringUnderrunFrames != 0) {
                tracer.recordLaneCounter(lane, "ring underrun frames", r.startNs, r.ringUnderrunFrames);
            }
        }
    } while (count == records.size());
}
}

void au::au3audio::installAudioIOTrace()
{
    AudioIOTrace::Hooks hooks;
    hooks.isEnabled = []() {
        return Tracer::instance().isEnabled();
    };
    hooks.zone = [](const char* name, int64_t startNs, int64_t endNs) {
        Tracer::instance().recordZone(name, Category::Audio, startNs, endNs);
    };
    hooks.counter = [](const char* name, double value) {
        Tracer::instance().recordCounter(name, value);
    };
    hooks.threadName = [](const char* name) {
        Tracer::instance().setThreadName(name);
    };
    AudioIOTrace::SetHooks(hooks);

    Tracer::instance().registerFlushSource(&drainCallbackRecords);
}
