/*!********************************************************************

 Audacity: A Digital Audio Editor

 @file AudioIOTrace.h
 @brief Latency and dropout diagnostics for the audio engine

 Callback records go through a preallocated single-writer ring, so the
 PortAudio callback never allocates or locks. Zones and counters from other
 threads are forwarded to hooks installed by the application.

 **********************************************************************/

#ifndef __AUDACITY_AUDIO_IO_TRACE__
#define __AUDACITY_AUDIO_IO_TRACE__

#include <chrono>
#include <cstddef>
#include <cstdint>

namespace AudioIOTrace {
struct CallbackRecord {
    int64_t startNs = 0;
    int64_t durNs = 0;
    //! PortAudio stream clock of the first output frame reaching the DAC, in seconds
    double outputDacTime = 0.0;
    double sampleRate = 0.0;
    uint32_t frames = 0;
    uint32_t statusFlags = 0;
    //! Frames zero-padded because the playback ring buffer ran dry
    uint32_t ringUnderrunFrames = 0;
    //! Absolute peak of the final output buffer and of the input buffer; -1 when absent
    float outputPeak = -1.0f;
    float inputPeak = -1.0f;
};

struct Hooks {
    bool (*isEnabled)() = nullptr;
    void (*zone)(const char* name, int64_t startNs, int64_t endNs) = nullptr;
    void (*counter)(const char* name, double value) = nullptr;
    void (*threadName)(const char* name) = nullptr;
};

//! Call before any stream starts; names must be string literals
AUDIO_IO_API void SetHooks(const Hooks& hooks);

AUDIO_IO_API bool IsEnabled();

//! Same clock as au::perf::Tracer::nowNs()
inline int64_t NowNs()
{
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

//! Audio callback thread only
void PushCallbackRecord(const CallbackRecord& record);

//! Single reader. Returns the number of records copied; `lost` gets the
//! records overwritten before they were read
AUDIO_IO_API size_t ReadCallbackRecords(CallbackRecord* out, size_t maxCount, uint64_t& lost);

//! Not for the audio callback thread
void Zone(const char* name, int64_t startNs, int64_t endNs);
void Counter(const char* name, double value);
void ThreadName(const char* name);

class ScopedZone
{
public:
    explicit ScopedZone(const char* name)
        : mName(name), mStartNs(IsEnabled() ? NowNs() : -1) {}
    ~ScopedZone()
    {
        if (mStartNs >= 0) {
            Zone(mName, mStartNs, NowNs());
        }
    }

    ScopedZone(const ScopedZone&) = delete;
    ScopedZone& operator=(const ScopedZone&) = delete;

private:
    const char* mName;
    int64_t mStartNs;
};
}

#endif
