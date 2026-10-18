/*
* Audacity: A Digital Audio Editor
*/
#pragma once
#include <cstdint>
#include "au3-components/EffectProcessingContext.h"

namespace RealtimeEffects {
// The caller owns this object for one processing call. Signed offsets allow
// downstream inserts to address the content preceding the current input block.
class ProcessingTime
{
public:
    virtual ~ProcessingTime() = default;
    virtual double TimeAt(int64_t sampleOffset) const = 0;
    virtual EffectProcessingContext ContextAt(int64_t sampleOffset) const
    {
        EffectProcessingContext result;
        result.time = TimeAt(sampleOffset);
        return result;
    }
};
}
