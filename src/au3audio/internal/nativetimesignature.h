/*
* Audacity: A Digital Audio Editor
*/
#pragma once

#include <atomic>
#include <cstdint>
#include "au3-registries/ClientData.h"
#include "au3-utility/Observer.h"

class AudacityProject;
namespace au::au3audio {
// Transient publication of document meter edits. Construct/Get on main;
// the audio threads only load the coherent pair and never visit the document.
// Tempo stays in the playback sequence snapshot used to compile MIDI events.
class NativeTimeSignature final : public ClientData::Base
{
public:
    struct Meter {
        int numerator, denominator;
    };
    static NativeTimeSignature& Get(AudacityProject&);
    explicit NativeTimeSignature(AudacityProject&);
    Meter meter() const noexcept;
private:
    void publish(int numerator, int denominator) noexcept;
    std::atomic<uint64_t> m_meter { 0 };
    Observer::Subscription m_subscription;
};
}
