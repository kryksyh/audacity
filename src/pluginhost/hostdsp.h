/*
* Audacity: A Digital Audio Editor
*/
#pragma once

#include <cstddef>
#include <vector>

#include "pluginhost/api/audacity_plugin.h"

namespace au::pluginhost::hostdsp {
//! Constant-rate high-quality resample of planar float data.
std::vector<std::vector<float> > resample(const float* const* channels, size_t channelCount, size_t frameCount,
                                          double srcRate, double targetRate);

//! The host-side "audacity.dsp/1" extension table.
const auplug_dsp_ext_t* hostDspExt();
}
