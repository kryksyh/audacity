/*
* Audacity: A Digital Audio Editor
*/
#pragma once
#include "au3-mixer/AudioRenderSource.h"

namespace au::au3audio {
void registerNativeAudioRenderer();
class NativeAudioRenderSource final : public AudioRenderSource
{
public:
    NativeAudioRenderSource(const AudacityProject& project, const Tracks& tracks, const Options& options);
    ~NativeAudioRenderSource() override;
    size_t Process() override;
    size_t Process(size_t maximumFrames) override;
    constSamplePtr GetBuffer() override;
    constSamplePtr GetBuffer(int channel) override;
    double MixGetCurrentTime() override;
    sampleFormat EffectiveFormat() const override;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};
}
