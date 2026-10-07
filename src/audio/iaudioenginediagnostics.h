/*
 * Audacity: A Digital Audio Editor
 */
#pragma once

#include <cstddef>
#include <cstdint>

#include "framework/global/modularity/imoduleinterface.h"

namespace au::audio {
//! Health of the audio stream. Counters count from the last reset.
struct AudioEngineDiagnostics {
    bool streamActive = false;
    double sampleRate = 0.0;
    size_t framesPerBuffer = 0;
    //! As reported by the audio driver, not measured
    double reportedInputLatencyMs = 0.0;
    double reportedOutputLatencyMs = 0.0;

    //! Share of the time budget of one callback; 1 is 100 %
    float averageLoad = 0.0f;
    //! Recent highest load, falling slowly after a spike
    float peakLoad = 0.0f;

    uint64_t callbacks = 0;
    //! Callbacks with at least one of the problems below
    uint64_t dropouts = 0;
    uint64_t overBudgetCallbacks = 0;
    uint64_t outputUnderflows = 0;
    uint64_t inputOverflows = 0;
    //! Playback buffer ran dry before the end of the material
    uint64_t playbackStarvations = 0;
    uint64_t lostCaptureFrames = 0;
};

class IAudioEngineDiagnostics : MODULE_GLOBAL_INTERFACE
{
    INTERFACE_ID(IAudioEngineDiagnostics)

public:
    virtual ~IAudioEngineDiagnostics() = default;

    virtual AudioEngineDiagnostics diagnostics() const = 0;
    virtual void resetCounters() = 0;
};
}
