/*
* Audacity: A Digital Audio Editor
*/
#include <tracktion_engine/tracktion_engine.h>

#include "tracktionaudiocallbacks.h"

#include "audacityplugins.h"
#include "tracktiontrackmeters.h"

using namespace au::tracktionaudio;

void TracktionAudioCallbacks::setPlaybackControls(std::shared_ptr<const AudacityPlaybackControls> controls)
{
    m_controls = std::move(controls);
    m_endTime.store(-1, std::memory_order_release);
}

void TracktionAudioCallbacks::setMeters(IMeterSenderPtr input, IMeterSenderPtr output)
{
    const auto replace = [this](IMeterSenderPtr& meter, IMeterSenderPtr replacement, int channels) {
        if (meter == replacement) {
            return;
        }
        if (meter && channels > 0) {
            meter->stop();
        }
        meter = std::move(replacement);
        if (meter && channels > 0) {
            meter->start(m_sampleRate);
        }
    };
    replace(m_inputMeter, std::move(input), m_inputChannels);
    replace(m_outputMeter, std::move(output), m_outputChannels);
}

void TracktionAudioCallbacks::setInputGain(float gain)
{
    const auto db = juce::Decibels::gainToDecibels(gain, std::numeric_limits<float>::lowest());
    m_inputGain = db > 0.01f || db < -0.01f ? tracktion::dbToGain(db) : 1.0f;
}

void TracktionAudioCallbacks::setTrackMeters(std::shared_ptr<TracktionTrackMeters> meters)
{
    m_trackMeters = std::move(meters);
}

void TracktionAudioCallbacks::setCaptureTracks(std::vector<CaptureTrack> tracks)
{
    m_captureTracks = std::move(tracks);
    m_captureChannels = 0;
    for (const auto& track : m_captureTracks) {
        m_captureChannels += track.channels;
    }
}

void TracktionAudioCallbacks::prepare(double sampleRate, int blockSize, int inputChannels, int outputChannels)
{
    m_active = true;
    m_sampleRate = sampleRate;
    m_callbackInfo.Clear();
    m_endTime.store(-1, std::memory_order_release);
    if (m_inputMeter && m_inputChannels > 0) {
        m_inputMeter->stop();
    }
    if (m_outputMeter && m_outputChannels > 0) {
        m_outputMeter->stop();
    }
    m_inputChannels = inputChannels;
    m_outputChannels = outputChannels;
    m_inputMeterBuffers.assign(inputChannels + 2, std::vector<float>(blockSize));
    if (m_inputMeter && inputChannels > 0) {
        m_inputMeter->start(sampleRate);
    }
    if (m_outputMeter && outputChannels > 0) {
        m_outputMeter->start(sampleRate);
    }
}

void TracktionAudioCallbacks::stop()
{
    m_active = false;
    if (m_inputMeter) {
        m_inputMeter->stop();
    }
    if (m_outputMeter) {
        m_outputMeter->stop();
    }
    m_inputMeter.reset();
    m_outputMeter.reset();
    m_trackMeters.reset();
    m_controls.reset();
    m_captureTracks.clear();
    m_captureChannels = 0;
    m_inputMeterBuffers.clear();
    m_inputGain = 1.0f;
    m_callbackInfo.Clear();
    m_endTime.store(-1, std::memory_order_release);
    m_inputChannels = 0;
    m_outputChannels = 0;
}

void TracktionAudioCallbacks::processInput(const float* const* channels, int numChannels, int numSamples, TimePoint adcTime)
{
    if (!m_active || !m_inputMeter || numChannels == 0 || numSamples == 0) {
        return;
    }
    const int blockSize = static_cast<int>(m_inputMeterBuffers[0].size());
    for (int offset = 0; offset < numSamples; offset += blockSize) {
        const int count = std::min(blockSize, numSamples - offset);
        const auto time = adcTime
                          + std::chrono::duration_cast<TimePoint::duration>(std::chrono::duration<double>(offset / m_sampleRate));
        for (int channel = 0; channel < numChannels; ++channel) {
            auto& buffer = m_inputMeterBuffers[channel];
            for (int frame = 0; frame < count; ++frame) {
                buffer[frame] = channels[channel][offset + frame] * m_inputGain;
            }
        }
        const bool monoCapture = numChannels == 2 && m_captureChannels == 1;
        if (monoCapture) {
            auto& mono = m_inputMeterBuffers[numChannels];
            for (int frame = 0; frame < count; ++frame) {
                mono[frame] = (m_inputMeterBuffers[0][frame] + m_inputMeterBuffers[1][frame]) * 0.5f;
            }
        }
        unsigned firstInput = 0;
        for (const auto& track : m_captureTracks) {
            for (unsigned channel = 0; channel < track.channels; ++channel) {
                const auto& buffer = m_inputMeterBuffers[monoCapture ? numChannels : (firstInput + channel) % numChannels];
                m_inputMeter->push(channel, { buffer.data(), static_cast<size_t>(count), 1, time }, IMeterSender::TrackId { track.id });
            }
            firstInput += track.channels;
        }
        if (numChannels <= 2) {
            for (int channel = 0; channel < numChannels; ++channel) {
                m_inputMeter->push(channel, { m_inputMeterBuffers[channel].data(), static_cast<size_t>(count), 1, time });
            }
        } else {
            for (int channel = 0; channel < 2; ++channel) {
                auto& buffer = m_inputMeterBuffers[numChannels + channel];
                std::fill_n(buffer.data(), count, 0.0f);
                for (int input = channel; input < numChannels; input += 2) {
                    for (int frame = 0; frame < count; ++frame) {
                        const auto sample = m_inputMeterBuffers[input][frame];
                        if (std::abs(sample) > std::abs(buffer[frame])) {
                            buffer[frame] = sample;
                        }
                    }
                }
                m_inputMeter->push(channel, { buffer.data(), static_cast<size_t>(count), 1, time });
            }
        }
    }
}

void TracktionAudioCallbacks::processOutput(float* const* channels, int numChannels, int numSamples, TimePoint dacTime)
{
    if (!m_active) {
        return;
    }
    if (m_outputMeter) {
        for (int channel = 0; channel < numChannels; ++channel) {
            m_outputMeter->push(channel, { channels[channel], static_cast<size_t>(numSamples), 1, dacTime });
        }
    }
    if (m_trackMeters) {
        m_trackMeters->send(m_outputMeter.get(), numSamples, dacTime);
    }
    m_callbackInfo.Put({ dacTime, numSamples });
    if (m_controls && m_endTime.load(std::memory_order_relaxed) < 0) {
        const int offset = m_controls->endSampleOffset.load(std::memory_order_acquire);
        if (offset >= 0) {
            const auto end = dacTime
                             + std::chrono::duration_cast<TimePoint::duration>(std::chrono::duration<double>(offset / m_sampleRate));
            m_endTime.store(std::chrono::duration_cast<std::chrono::nanoseconds>(end.time_since_epoch()).count(),
                            std::memory_order_release);
        }
    }
}

std::optional<au::audio::AudioCallbackInfo> TracktionAudioCallbacks::consumeCallbackInfo()
{
    audio::AudioCallbackInfo info;
    return m_callbackInfo.Get(info) ? std::make_optional(info) : std::nullopt;
}

std::optional<AudioDeviceCallback::TimePoint> TracktionAudioCallbacks::playbackEndTime() const
{
    const auto end = m_endTime.load(std::memory_order_acquire);
    return end
           < 0 ? std::nullopt : std::make_optional(TimePoint(std::chrono::duration_cast<TimePoint::duration>(std::chrono::nanoseconds(end))));
}
