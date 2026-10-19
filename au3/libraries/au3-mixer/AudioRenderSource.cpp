/*
* Audacity: A Digital Audio Editor
*/
#include "AudioRenderSource.h"

#include <stdexcept>

std::unique_ptr<AudioRenderSource> AudioRenderSource::Create(const AudacityProject& project, const Tracks& tracks, const Options& options)
{
    auto source = Factory::Call(project, tracks, options);
    if (!source) {
        throw std::logic_error("An audio render service is required.");
    }
    return source;
}
