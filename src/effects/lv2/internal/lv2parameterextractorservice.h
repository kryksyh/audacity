/*
 * Audacity: A Digital Audio Editor
 */
#pragma once

#include "tracktionaudio/internal/tracktionparameterextractorservice.h"

namespace au::effects {
class Lv2ParameterExtractorService final : public TracktionParameterExtractorService
{
public:
    Lv2ParameterExtractorService()
        : TracktionParameterExtractorService(EffectFamily::LV2) {}
};
}
