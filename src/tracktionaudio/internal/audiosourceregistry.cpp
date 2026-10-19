/*
* Audacity: A Digital Audio Editor
*/
#include "audiosourceregistry.h"

#include "audacityaudiosource.h"

using namespace au::tracktionaudio;

namespace {
class AudacityAudioFormatReader final : public juce::AudioFormatReader
{
public:
    AudacityAudioFormatReader(juce::InputStream* stream, std::shared_ptr<const AudacityAudioSource> source)
        : juce::AudioFormatReader(stream, "Audacity"), m_source(std::move(source))
    {
        sampleRate = m_source->sampleRate();
        numChannels = m_source->channelCount();
        lengthInSamples = m_source->sampleCount();
        bitsPerSample = 32;
        usesFloatingPointData = true;
    }

    bool readSamples(int* const* destinations, int channelCount, int destinationOffset, juce::int64 start, int count) override
    {
        bool success = true;
        for (int channel = 0; channel < channelCount; ++channel) {
            if (destinations[channel]) {
                success = m_source->read(channel, reinterpret_cast<float*>(destinations[channel]) + destinationOffset, start, count)
                          && success;
            }
        }
        return success;
    }

private:
    std::shared_ptr<const AudacityAudioSource> m_source;
};

class AudacityAudioFormat final : public juce::AudioFormat
{
public:
    explicit AudacityAudioFormat(std::shared_ptr<AudioSourceRegistry> registry)
        : juce::AudioFormat("Audacity", ".ausource"), m_registry(std::move(registry))
    {
    }

    juce::Array<int> getPossibleSampleRates() override { return {}; }
    juce::Array<int> getPossibleBitDepths() override { return { 32 }; }
    bool canDoStereo() override { return true; }
    bool canDoMono() override { return true; }

    juce::AudioFormatReader* createReaderFor(juce::InputStream* stream, bool deleteOnFailure) override
    {
        if (stream->readNextLine() == "AUDACITY_AUDIO_SOURCE") {
            auto source = m_registry->find(stream->readNextLine());
            if (source) {
                return new AudacityAudioFormatReader(stream, std::move(source));
            }
        }
        if (deleteOnFailure) {
            delete stream;
        }
        return nullptr;
    }

    std::unique_ptr<juce::AudioFormatWriter> createWriterFor(std::unique_ptr<juce::OutputStream>&,
                                                             const juce::AudioFormatWriterOptions&) override
    {
        return nullptr;
    }

private:
    std::shared_ptr<AudioSourceRegistry> m_registry;
};
}

AudioSourceRegistry::AudioSourceRegistry()
    : m_directory(juce::File::getSpecialLocation(juce::File::tempDirectory)
                  .getChildFile("audacity-tracktion-" + juce::Uuid().toString()))
{
}

AudioSourceRegistry::~AudioSourceRegistry()
{
    m_directory.deleteRecursively();
}

juce::File AudioSourceRegistry::add(std::shared_ptr<const AudacityAudioSource> source)
{
    const auto result = m_directory.createDirectory();
    if (result.failed()) {
        return {};
    }

    const auto id = juce::Uuid().toString();
    const auto file = m_directory.getChildFile(id + ".ausource");
    if (!file.replaceWithText("AUDACITY_AUDIO_SOURCE\n" + id + "\n")) {
        return {};
    }

    const std::lock_guard lock(m_mutex);
    m_sources.emplace(id, std::move(source));
    return file;
}

void AudioSourceRegistry::remove(const juce::File& file)
{
    {
        const std::lock_guard lock(m_mutex);
        m_sources.erase(file.getFileNameWithoutExtension());
    }
    file.deleteFile();
}

std::unique_ptr<juce::AudioFormat> AudioSourceRegistry::createFormat()
{
    return std::make_unique<AudacityAudioFormat>(shared_from_this());
}

std::shared_ptr<const AudacityAudioSource> AudioSourceRegistry::find(const juce::String& id) const
{
    const std::lock_guard lock(m_mutex);
    const auto found = m_sources.find(id);
    return found == m_sources.end() ? nullptr : found->second;
}
