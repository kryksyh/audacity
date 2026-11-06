/*!********************************************************************

 Audacity: A Digital Audio Editor

 @file LatencySignalAnalysis.h
 @brief Round-trip latency from distinct test signals

 Each test signal has a different waveform. Its position in the captured
 input is compared with its position in the output stream.

 **********************************************************************/

#ifndef __AUDACITY_LATENCY_SIGNAL_ANALYSIS__
#define __AUDACITY_LATENCY_SIGNAL_ANALYSIS__

#include <cstddef>
#include <vector>

namespace LatencySignalAnalysis {
constexpr size_t BurstLength = 512;
//! Minimum normalized correlation between the emitted and captured waveforms.
constexpr double MinCorrelation = 0.3;

AUDIO_IO_API std::vector<float> MakeBurst(float gain, size_t signalIndex);

struct Hit {
    long lag = -1;
    double correlation = 0.0;
    bool inverted = false;
};

//! Best match of `burst` in `capture[begin, begin + span + burst.size() - 1)`
AUDIO_IO_API Hit FindBurst(const std::vector<float>& capture, const std::vector<float>& burst, size_t begin, size_t span);

struct Analysis {
    //! Median over the found bursts; -1 when none was found
    long roundTripFrames = -1;
    size_t burstsFound = 0;
    size_t burstsTotal = 0;
    //! Largest minus smallest offset; clock drift between devices shows here
    long spreadFrames = 0;
    double minCorrelation = 0.0;
    bool inverted = false;
    float peak = 0.0f;
};

//! `capture` holds one channel that starts at the first played frame
AUDIO_IO_API Analysis Analyse(const std::vector<float>& capture, const std::vector<std::vector<float> >& bursts, size_t periodFrames);
}

#endif
