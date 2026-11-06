/*!********************************************************************

 Audacity: A Digital Audio Editor

 @file LatencySignalAnalysis.cpp

 **********************************************************************/

#include "LatencySignalAnalysis.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace LatencySignalAnalysis {
std::vector<float> MakeBurst(float gain, size_t signalIndex)
{
    std::vector<float> burst;
    burst.reserve(BurstLength);
    uint32_t seed = 0x1234567u + static_cast<uint32_t>(signalIndex);
    for (size_t i = 0; i < BurstLength; ++i) {
        seed = seed * 1664525u + 1013904223u;
        burst.push_back((seed >> 31) ? gain : -gain);
    }
    return burst;
}

Hit FindBurst(const std::vector<float>& capture, const std::vector<float>& burst, size_t begin, size_t span)
{
    Hit hit;
    if (span == 0 || burst.empty() || begin + span + burst.size() - 1 > capture.size()) {
        return hit;
    }
    double referenceEnergy = 0.0;
    double captureEnergy = 0.0;
    for (size_t j = 0; j < burst.size(); ++j) {
        referenceEnergy += double(burst[j]) * burst[j];
        captureEnergy += double(capture[begin + j]) * capture[begin + j];
    }
    if (referenceEnergy == 0.0) {
        return hit;
    }
    for (size_t lag = 0; lag < span; ++lag) {
        const float* x = capture.data() + begin + lag;
        double correlation = 0.0;
        for (size_t j = 0; j < burst.size(); ++j) {
            correlation += burst[j] * x[j];
        }
        const double normalized = captureEnergy > 0.0 ? correlation / std::sqrt(referenceEnergy * captureEnergy) : 0.0;
        if (std::fabs(normalized) > hit.correlation) {
            hit.correlation = std::fabs(normalized);
            hit.lag = static_cast<long>(lag);
            hit.inverted = normalized < 0.0;
        }
        if (lag + 1 < span) {
            captureEnergy += double(x[burst.size()]) * x[burst.size()] - double(x[0]) * x[0];
            captureEnergy = std::max(0.0, captureEnergy);
        }
    }
    return hit;
}

Analysis Analyse(const std::vector<float>& capture, const std::vector<std::vector<float> >& bursts, size_t periodFrames)
{
    Analysis result;
    result.burstsTotal = bursts.size();
    for (const float v : capture) {
        result.peak = std::max(result.peak, std::fabs(v));
    }

    std::vector<long> lags;
    size_t invertedCount = 0;
    for (size_t i = 0; i < bursts.size(); ++i) {
        const size_t begin = i * periodFrames;
        if (begin + bursts[i].size() > capture.size()) {
            continue;
        }
        // Search beyond one period: a slow device can return a signal after the next signal starts.
        const size_t span = capture.size() - begin - bursts[i].size() + 1;
        const Hit hit = FindBurst(capture, bursts[i], begin, span);
        if (hit.lag < 0 || hit.correlation < MinCorrelation) {
            continue;
        }
        result.minCorrelation = lags.empty() ? hit.correlation : std::min(result.minCorrelation, hit.correlation);
        lags.push_back(hit.lag);
        invertedCount += hit.inverted ? 1 : 0;
    }

    result.burstsFound = lags.size();
    if (lags.empty()) {
        return result;
    }
    std::sort(lags.begin(), lags.end());
    result.roundTripFrames = lags[lags.size() / 2];
    result.spreadFrames = lags.back() - lags.front();
    result.inverted = invertedCount * 2 > lags.size();
    return result;
}
}
