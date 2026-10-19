/*
* Audacity: A Digital Audio Editor
*/
#include <tracktion_engine/tracktion_engine.h>

#include "tracktionrendersource.h"

#include <stdexcept>

#include "tracktionsession.h"

using namespace au::tracktionaudio;

struct TracktionRenderSource::Data final : juce::AudioFormatWriter::ThreadedWriter::IncomingDataReceiver
{
    Data(TracktionSession& session, Options renderOptions)
        : options(renderOptions), status(session.edit(), false)
    {
        if (options.endTime <= options.startTime) {
            finished = true;
            return;
        }
        auto& edit = session.edit();
        tracktion::Renderer::Parameters parameters(edit);
        parameters.audioFormat = edit.engine.getAudioFileFormatManager().getWavFormat();
        parameters.sampleRateForAudio = options.sampleRate;
        parameters.blockSizeForAudio = options.blockSize;
        parameters.bitDepth = 32;
        parameters.time = { tracktion::TimePosition::fromSeconds(options.startTime),
                            tracktion::TimePosition::fromSeconds(options.endTime) };
        parameters.canRenderInMono = false;
        parameters.mustRenderInMono = options.channels == 1;
        parameters.channelConfig = tracktion::ChannelConfiguration::canonical(options.channels);
        parameters.useMasterPlugins = true;
        parameters.checkNodesForAudio = false;
        task = std::make_unique<tracktion::Renderer::RenderTask>("", parameters, nullptr, this);
        // Prepare the render graph on the main thread before export starts.
        advance();
    }

    void reset(int channels, double, juce::int64) override
    {
        pending.setSize(channels, options.blockSize);
        pendingFrames = 0;
        pendingOffset = 0;
    }

    void addBlock(juce::int64, const juce::AudioBuffer<float>& audio, int offset, int frames) override
    {
        jassert(pendingFrames == 0);
        pending.setSize(audio.getNumChannels(), frames, false, false, true);
        for (int channel = 0; channel < audio.getNumChannels(); ++channel) {
            pending.copyFrom(channel, 0, audio, channel, offset, frames);
        }
        pendingFrames = frames;
        pendingOffset = 0;
    }

    void advance()
    {
        finished = task->runJob() == juce::ThreadPoolJob::jobHasFinished;
        if (task->errorMessage.isNotEmpty()) {
            throw std::runtime_error(task->errorMessage.toStdString());
        }
    }

    Options options;
    tracktion::Edit::ScopedRenderStatus status;
    std::unique_ptr<tracktion::Renderer::RenderTask> task;
    juce::AudioBuffer<float> pending;
    juce::AudioBuffer<float> output;
    int pendingFrames = 0;
    int pendingOffset = 0;
    int64_t framesRead = 0;
    bool finished = false;
};

TracktionRenderSource::TracktionRenderSource(TracktionSession& session, Options options)
    : m_data(std::make_unique<Data>(session, options))
{
}

TracktionRenderSource::~TracktionRenderSource() = default;

int TracktionRenderSource::process(int maximumFrames)
{
    auto& data = *m_data;
    data.output.setSize(data.options.channels, maximumFrames, false, false, true);
    int frames = 0;
    while (frames < maximumFrames) {
        while (data.pendingFrames == 0 && !data.finished) {
            data.advance();
        }
        if (data.pendingFrames == 0) {
            break;
        }
        const int count = std::min(maximumFrames - frames, data.pendingFrames);
        for (int channel = 0; channel < data.options.channels; ++channel) {
            data.output.copyFrom(channel, frames, data.pending, channel, data.pendingOffset, count);
        }
        data.pendingOffset += count;
        data.pendingFrames -= count;
        frames += count;
    }
    data.framesRead += frames;
    data.output.setSize(data.options.channels, frames, true, false, true);
    return frames;
}

const juce::AudioBuffer<float>& TracktionRenderSource::buffer() const
{
    return m_data->output;
}

double TracktionRenderSource::currentTime() const
{
    return m_data->options.startTime + m_data->framesRead / m_data->options.sampleRate;
}

void TracktionRenderSource::setCancellationCheck(std::function<bool()> check)
{
    if (m_data->task) {
        m_data->task->setCancellationCheck(std::move(check));
    }
}
