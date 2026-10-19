/*
* Audacity: A Digital Audio Editor
*/
#pragma once

#include <memory>

#include "au3-mixer/AudioRenderSource.h"

class AudacityProject;

namespace au::tracktionaudio {
class TracktionRuntime;

class TracktionMixSource final : public AudioRenderSource
{
public:
    TracktionMixSource(TracktionRuntime& runtime, const AudacityProject& project, const Tracks& tracks, Options options);
    ~TracktionMixSource() override;

    size_t Process() override;
    size_t Process(size_t maximumFrames) override;
    constSamplePtr GetBuffer() override;
    constSamplePtr GetBuffer(int channel) override;
    double MixGetCurrentTime() override;
    sampleFormat EffectiveFormat() const override;

private:
    struct Data;
    std::unique_ptr<Data> m_data;
};
}
