/*
 * Audacity: A Digital Audio Editor
 */
#include "au3audioenginediagnostics.h"

#include "au3-audio-io/AudioIO.h"

using namespace au::au3audio;

namespace {
au::audio::AudioEngineDiagnostics readEngine()
{
    const AudioIOStreamHealth d = AudioIO::Get()->GetStreamHealth();

    au::audio::AudioEngineDiagnostics result;
    result.streamActive = d.streamActive;
    result.sampleRate = d.sampleRate;
    result.framesPerBuffer = d.framesPerBuffer;
    result.reportedInputLatencyMs = d.reportedInputLatencyMs;
    result.reportedOutputLatencyMs = d.reportedOutputLatencyMs;
    result.averageLoad = d.averageLoad;
    result.peakLoad = d.peakLoad;
    result.callbacks = d.callbacks;
    result.dropouts = d.dropouts;
    result.overBudgetCallbacks = d.overBudgetCallbacks;
    result.outputUnderflows = d.outputUnderflows;
    result.inputOverflows = d.inputOverflows;
    result.playbackStarvations = d.playbackStarvations;
    result.lostCaptureFrames = d.lostCaptureFrames;
    return result;
}
}

au::audio::AudioEngineDiagnostics Au3AudioEngineDiagnostics::diagnostics() const
{
    audio::AudioEngineDiagnostics result = readEngine();
    result.callbacks -= m_baseline.callbacks;
    result.dropouts -= m_baseline.dropouts;
    result.overBudgetCallbacks -= m_baseline.overBudgetCallbacks;
    result.outputUnderflows -= m_baseline.outputUnderflows;
    result.inputOverflows -= m_baseline.inputOverflows;
    result.playbackStarvations -= m_baseline.playbackStarvations;
    result.lostCaptureFrames -= m_baseline.lostCaptureFrames;
    return result;
}

void Au3AudioEngineDiagnostics::resetCounters()
{
    m_baseline = readEngine();
}
