/*
 * Audacity: A Digital Audio Editor
 */
#pragma once

#include "tracktionaudio/internal/tracktionparameterextractorservice.h"

namespace au::effects {
class VstParameterExtractorService final : public TracktionParameterExtractorService
{
public:
    VstParameterExtractorService()
        : TracktionParameterExtractorService(EffectFamily::VST3) {}
};
}
