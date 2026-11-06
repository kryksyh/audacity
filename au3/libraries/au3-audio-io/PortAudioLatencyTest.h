/*
 * Audacity: A Digital Audio Editor
 */
#pragma once

#include <atomic>
#include <cstddef>

class AUDIO_IO_API PortAudioLatencyTest
{
public:
    //! Result of a round-trip measurement through a loopback (cable, or speaker to microphone)
    struct Result {
        enum class Status {
            Measured,
            Cancelled,
            DeviceError,
            NoSignal, //!< Too few bursts came back to trust the result
        };
        Status status = Status::DeviceError;
        double sampleRate = 0.0;
        long roundTripFrames = -1;
        //! What automatic latency compensation would use for this stream
        double reportedInputLatencySecs = 0.0;
        double reportedOutputLatencySecs = 0.0;
        size_t burstsFound = 0;
        size_t burstsTotal = 0;
        long spreadFrames = 0;
        double minCorrelation = 0.0;
        float inputPeak = 0.0f;
        bool inverted = false;
        int inputChannel = -1;
    };
    PortAudioLatencyTest(int outputDevice, int inputDevice, double rate, double bufferMs, int channels);

    Result Run();
    void Cancel();
    int SignalsPlayed() const;

private:
    int mOutputDevice;
    int mInputDevice;
    double mRate;
    double mBufferMs;
    int mChannels;
    std::atomic<bool> mCancelled { false };
    std::atomic<int> mSignalsPlayed { 0 };
};
