/*
* Audacity: A Digital Audio Editor
*/
#pragma once
#include "au3-audio-io/AudioIO.h"

class MidiTrack;
namespace au::au3audio {
class LiveInstrumentSession;
// Original PortAudio callback monitoring, through the same native routing and
// original effect instances as playback. Setup/rebuild/Stop run on main only.
class NativeMonitorSource final : public LiveAudioSource
{
public:
    NativeMonitorSource(std::shared_ptr<AudacityProject>, std::shared_ptr<MidiTrack>, std::shared_ptr<LiveInstrumentSession>,
                        double position, std::weak_ptr<IMeterSender> meter = {});
    ~NativeMonitorSource() override;
    void Prepare(double sampleRate) override;
    void Render(float* output, size_t channels, size_t frames) noexcept override;
    RealtimeEffects::InitializationScope* RealtimeInitialization() override;
    bool RefreshRealtimeEffects() noexcept override;
    void Stop() noexcept override;
    bool NeedsRebuild() const;
    bool NeedsRefresh(double position) const;
    bool Refresh(double position) noexcept;
    bool ready() const;
private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};
}
