/*!********************************************************************

 Audacity: A Digital Audio Editor

 @file AudioIOTrace.cpp

 **********************************************************************/

#include "AudioIOTrace.h"

#include <algorithm>
#include <array>
#include <atomic>

namespace AudioIOTrace {
namespace {
// About 10 s of callbacks at 64 frames / 48 kHz; the reader drains far more often
constexpr size_t kCapacity = 1 << 13;

Hooks sHooks;

std::array<CallbackRecord, kCapacity> sRecords;
std::atomic<uint64_t> sWritten{ 0 };
uint64_t sRead = 0;
}

void SetHooks(const Hooks& hooks)
{
    sHooks = hooks;
}

bool IsEnabled()
{
    return sHooks.isEnabled && sHooks.isEnabled();
}

void PushCallbackRecord(const CallbackRecord& record)
{
    const uint64_t index = sWritten.load(std::memory_order_relaxed);
    sRecords[index % kCapacity] = record;
    sWritten.store(index + 1, std::memory_order_release);
}

size_t ReadCallbackRecords(CallbackRecord* out, size_t maxCount, uint64_t& lost)
{
    const uint64_t written = sWritten.load(std::memory_order_acquire);
    lost = 0;
    if (written - sRead > kCapacity) {
        // Keep a margin: the writer may be filling the oldest slot right now
        const uint64_t oldestSafe = written - kCapacity + 64;
        lost = oldestSafe - sRead;
        sRead = oldestSafe;
    }

    const size_t count = static_cast<size_t>(std::min<uint64_t>(written - sRead, maxCount));
    for (size_t i = 0; i < count; ++i) {
        out[i] = sRecords[(sRead + i) % kCapacity];
    }
    sRead += count;
    return count;
}

void Zone(const char* name, int64_t startNs, int64_t endNs)
{
    if (sHooks.zone) {
        sHooks.zone(name, startNs, endNs);
    }
}

void Counter(const char* name, double value)
{
    if (sHooks.counter && IsEnabled()) {
        sHooks.counter(name, value);
    }
}

void ThreadName(const char* name)
{
    if (sHooks.threadName) {
        sHooks.threadName(name);
    }
}
}
