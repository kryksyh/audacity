/*
* Audacity: A Digital Audio Editor
*/
#include "hostdsp.h"

#include <algorithm>

#include "au3-math/Resample.h"

namespace au::pluginhost::hostdsp {
std::vector<std::vector<float> > resample(const float* const* channels, size_t channelCount, size_t frameCount,
                                          double srcRate, double targetRate)
{
    std::vector<std::vector<float> > out(channelCount);

    if (srcRate == targetRate || srcRate <= 0 || targetRate <= 0) {
        for (size_t c = 0; c < channelCount; ++c) {
            out[c].assign(channels[c], channels[c] + frameCount);
        }
        return out;
    }

    const double factor = targetRate / srcRate;
    const size_t bufsize = 65536;
    std::vector<float> outBuffer(bufsize);

    for (size_t c = 0; c < channelCount; ++c) {
        ::Resample resampler(true, factor, factor);
        out[c].reserve(static_cast<size_t>(frameCount * factor) + 16);

        size_t pos = 0;
        size_t outGenerated = 0;
        while (pos < frameCount || outGenerated > 0) {
            const size_t inLen = std::min(bufsize, frameCount - pos);
            const bool isLast = (pos + inLen) == frameCount;

            const auto results = resampler.Process(factor, channels[c] + pos, inLen, isLast,
                                                   outBuffer.data(), bufsize);
            pos += results.first;
            outGenerated = results.second;
            out[c].insert(out[c].end(), outBuffer.begin(), outBuffer.begin() + outGenerated);
        }
    }

    // channels resampled independently may differ by a sample; keep them equal
    size_t minLen = out.empty() ? 0 : out[0].size();
    for (const auto& ch : out) {
        minLen = std::min(minLen, ch.size());
    }
    for (auto& ch : out) {
        ch.resize(minLen);
    }

    return out;
}

namespace {
bool dsp_resample(void*, const auplug_audio_buffer_t* in, double target_rate, auplug_audio_buffer_t* out)
{
    if (!in || !out || !in->channels || in->channel_count == 0 || target_rate <= 0) {
        return false;
    }

    thread_local std::vector<std::vector<float> > data;
    thread_local std::vector<float*> ptrs;

    // 'in' may point into the previous result on this thread (chained
    // resamples): resample() is fully evaluated before the assignment
    // releases the old storage.
    data = resample(in->channels, in->channel_count, static_cast<size_t>(in->frame_count),
                    in->sample_rate, target_rate);

    ptrs.resize(data.size());
    for (size_t c = 0; c < data.size(); ++c) {
        ptrs[c] = data[c].data();
    }

    out->channel_count = static_cast<uint32_t>(data.size());
    out->frame_count = data.empty() ? 0 : data[0].size();
    out->sample_rate = target_rate;
    out->channels = ptrs.data();
    return true;
}

const auplug_dsp_ext_t s_dsp_ext = {
    dsp_resample,
};
}

const auplug_dsp_ext_t* hostDspExt()
{
    return &s_dsp_ext;
}
}
