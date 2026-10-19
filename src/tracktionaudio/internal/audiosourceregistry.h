/*
* Audacity: A Digital Audio Editor
*/
#pragma once

#include <map>
#include <memory>
#include <mutex>

#include <juce_audio_formats/juce_audio_formats.h>

namespace au::tracktionaudio {
class AudacityAudioSource;

class AudioSourceRegistry : public std::enable_shared_from_this<AudioSourceRegistry>
{
public:
    AudioSourceRegistry();
    ~AudioSourceRegistry();

    juce::File add(std::shared_ptr<const AudacityAudioSource> source);
    void remove(const juce::File& file);
    std::unique_ptr<juce::AudioFormat> createFormat();
    std::shared_ptr<const AudacityAudioSource> find(const juce::String& id) const;

private:
    juce::File m_directory;
    mutable std::mutex m_mutex;
    std::map<juce::String, std::shared_ptr<const AudacityAudioSource> > m_sources;
};
}
