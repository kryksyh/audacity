/*
* Audacity: A Digital Audio Editor
*/
#pragma once

#include <atomic>
#include <memory>
#include <optional>

#include "portaudiodevice.h"

#include "audio/iaudioengine.h"
#include "au3-audio-devices/IMeterSender.h"
#include "au3-utility/LockFreeQueue.h"

namespace au::tracktionaudio {
struct AudacityPlaybackControls;
class TracktionTrackMeters;

class TracktionAudioCallbacks final : public AudioDeviceCallback
{
public:
    struct CaptureTrack {
        int64_t id;
        unsigned channels;
    };

    // Change the bindings only while the audio callback is stopped or locked.
    void setPlaybackControls(std::shared_ptr<const AudacityPlaybackControls> controls);
    void setMeters(IMeterSenderPtr input, IMeterSenderPtr output);
    void setInputGain(float gain);
    void setCaptureTracks(std::vector<CaptureTrack> tracks);
    void setTrackMeters(std::shared_ptr<TracktionTrackMeters> meters);
    void stop();

    void prepare(double sampleRate, int blockSize, int inputChannels, int outputChannels) override;
    void processInput(const float* const* channels, int numChannels, int numSamples, TimePoint adcTime) override;
    void processOutput(float* const* channels, int numChannels, int numSamples, TimePoint dacTime) override;

    std::optional<audio::AudioCallbackInfo> consumeCallbackInfo();
    std::optional<TimePoint> playbackEndTime() const;

private:
    std::shared_ptr<const AudacityPlaybackControls> m_controls;
    IMeterSenderPtr m_inputMeter;
    IMeterSenderPtr m_outputMeter;
    std::shared_ptr<TracktionTrackMeters> m_trackMeters;
    std::vector<CaptureTrack> m_captureTracks;
    unsigned m_captureChannels = 0;
    float m_inputGain = 1.0f;
    std::vector<std::vector<float> > m_inputMeterBuffers;
    LockFreeQueue<audio::AudioCallbackInfo> m_callbackInfo { 512 };
    std::atomic<int64_t> m_endTime { -1 };
    double m_sampleRate = 44100.0;
    int m_inputChannels = 0;
    int m_outputChannels = 0;
    bool m_active = false;
};
}
