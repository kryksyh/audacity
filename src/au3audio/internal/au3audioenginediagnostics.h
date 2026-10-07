/*
 * Audacity: A Digital Audio Editor
 */
#pragma once

#include "audio/iaudioenginediagnostics.h"

namespace au::au3audio {
class Au3AudioEngineDiagnostics : public audio::IAudioEngineDiagnostics
{
public:
    audio::AudioEngineDiagnostics diagnostics() const override;
    void resetCounters() override;

private:
    audio::AudioEngineDiagnostics m_baseline;
};
}
