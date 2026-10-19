/*
* Audacity: A Digital Audio Editor
*/
#pragma once

#include <tracktion_engine/tracktion_engine.h>
#include "audio/iinstrumenteditor.h"

namespace au::tracktionaudio {
tracktion::Plugin::Ptr createTracktionInstrument(tracktion::Edit& edit, const std::string& description, const std::string& state);
std::shared_ptr<audio::IInstrumentEditor> createLiveInstrumentEditor(tracktion::Edit& edit, tracktion::AudioTrack& track);
}
