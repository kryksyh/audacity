/*
 * Audacity: A Digital Audio Editor
 */
#include "PortAudioLatencyTest.h"
#include "LatencySignalAnalysis.h"

#include <algorithm>
#include <chrono>
#include <thread>
#include "au3-utility/MemoryX.h"
#include "portaudio.h"
#ifdef __WXMSW__
#include "pa_win_wasapi.h"
#endif

namespace {
struct LoopbackState {
    std::vector<std::vector<float> > bursts;
    size_t periodFrames = 0;
    size_t totalFrames = 0;
    unsigned outputChannels = 0;
    unsigned inputChannels = 0;
    //! One buffer per input channel, preallocated
    std::vector<std::vector<float> > capture;
    //! Callback thread only
    size_t position = 0;
    std::atomic<bool> done { false };
    std::atomic<int>* signalsPlayed = nullptr;
};

int LoopbackCallback(const void* inputBuffer, void* outputBuffer, unsigned long frames,
                     const PaStreamCallbackTimeInfo*, PaStreamCallbackFlags, void* userData)
{
    auto& s = *static_cast<LoopbackState*>(userData);
    const auto input = static_cast<const float*>(inputBuffer);
    const auto output = static_cast<float*>(outputBuffer);

    for (unsigned long i = 0; i < frames; ++i) {
        const size_t n = s.position + i;
        const size_t phase = n % s.periodFrames;
        const size_t signal = n / s.periodFrames;
        const float value = signal < s.bursts.size() && phase < s.bursts[signal].size() ? s.bursts[signal][phase] : 0.0f;
        for (unsigned ch = 0; ch < s.outputChannels; ++ch) {
            output[i * s.outputChannels + ch] = value;
        }
        if (input && n < s.totalFrames) {
            for (unsigned ch = 0; ch < s.inputChannels; ++ch) {
                s.capture[ch][n] = input[i * s.inputChannels + ch];
            }
        }
    }
    if (s.position / s.periodFrames < s.bursts.size()) {
        s.signalsPlayed->store(static_cast<int>(s.position / s.periodFrames) + 1, std::memory_order_relaxed);
    }
    s.position += frames;
    if (s.position >= s.totalFrames) {
        s.done.store(true, std::memory_order_release);
    }
    return paContinue;
}
}

PortAudioLatencyTest::PortAudioLatencyTest(int outputDevice, int inputDevice, double rate, double bufferMs, int channels)
    : mOutputDevice(outputDevice), mInputDevice(inputDevice), mRate(rate), mBufferMs(bufferMs), mChannels(channels)
{
}

void PortAudioLatencyTest::Cancel()
{
    mCancelled.store(true);
}

int PortAudioLatencyTest::SignalsPlayed() const
{
    return mSignalsPlayed.load(std::memory_order_relaxed);
}

PortAudioLatencyTest::Result PortAudioLatencyTest::Run()
{
    using Status = Result::Status;
    Result result;
    if (mCancelled.load()) {
        result.status = Status::Cancelled;
        return result;
    }

    const PaDeviceIndex playDevice = mOutputDevice;
    const PaDeviceIndex recordDevice = mInputDevice;
    const PaDeviceInfo* playInfo = Pa_GetDeviceInfo(playDevice);
    const PaDeviceInfo* recordInfo = Pa_GetDeviceInfo(recordDevice);
    if (!playInfo || !recordInfo || playInfo->maxOutputChannels <= 0 || recordInfo->maxInputChannels <= 0) {
        return result;
    }
    const PaHostApiInfo* playHost = Pa_GetHostApiInfo(playInfo->hostApi);
    const PaHostApiInfo* recordHost = Pa_GetHostApiInfo(recordInfo->hostApi);
    const bool playWASAPI = playHost && playHost->type == paWASAPI;
    const bool recordWASAPI = recordHost && recordHost->type == paWASAPI;
    const bool usingJack = playHost && playHost->type == paJACK;

    double rate = mRate;
    const bool unsupportedRate = rate == 0.0;
    if (unsupportedRate && recordWASAPI) {
        rate = recordInfo->defaultSampleRate;
    }
    if (rate == 0.0) {
        return result;
    }

    const double latencyDuration = mBufferMs;

    constexpr size_t signalCount = 4;
    // Capture the delayed final signal after the output becomes silent.
    constexpr double seconds = 5.0;
    constexpr float gain = 0.5f;
    LoopbackState state;
    state.signalsPlayed = &mSignalsPlayed;
    for (size_t i = 0; i < signalCount; ++i) {
        state.bursts.push_back(LatencySignalAnalysis::MakeBurst(gain, i));
    }
    state.periodFrames = static_cast<size_t>(rate);
    state.totalFrames = static_cast<size_t>(seconds * rate);
    state.outputChannels = std::min(2, playInfo->maxOutputChannels);
    state.inputChannels = std::clamp(mChannels, 1, recordInfo->maxInputChannels);
    state.capture.assign(state.inputChannels, std::vector<float>(state.totalFrames, 0.0f));

    PaStreamParameters playParameters {};
    playParameters.device = playDevice;
    playParameters.channelCount = static_cast<int>(state.outputChannels);
    playParameters.sampleFormat = paFloat32;
    // See bug 1949 in StartPortAudioStream
    playParameters.suggestedLatency = playWASAPI ? 0.0 : latencyDuration / 1000.0;

#ifdef __WXMSW__
    PaWasapiStreamInfo wasapiStreamInfo {};
    if (playWASAPI && unsupportedRate) {
        wasapiStreamInfo.size = sizeof(PaWasapiStreamInfo);
        wasapiStreamInfo.hostApiType = paWASAPI;
        wasapiStreamInfo.version = 1;
        wasapiStreamInfo.flags = paWinWasapiAutoConvert;
        playParameters.hostApiSpecificStreamInfo = &wasapiStreamInfo;
    }
#endif

    PaStreamParameters recordParameters {};
    recordParameters.device = recordDevice;
    recordParameters.channelCount = static_cast<int>(state.inputChannels);
    recordParameters.sampleFormat = paFloat32;
    recordParameters.suggestedLatency = latencyDuration / 1000.0;

    PaStream* stream = nullptr;
    if (Pa_OpenStream(&stream, &recordParameters, &playParameters, rate, paFramesPerBufferUnspecified,
                      paNoFlag, LoopbackCallback, &state) != paNoError) {
        return result;
    }
    auto closeStream = finally([stream] { Pa_CloseStream(stream); });

    const PaStreamInfo* info = Pa_GetStreamInfo(stream);
    result.sampleRate = info->sampleRate;
    result.reportedInputLatencySecs = info->inputLatency;
    // Same JACK exception as StartPortAudioStream
    result.reportedOutputLatencySecs = usingJack ? latencyDuration / 1000.0 : info->outputLatency;

    if (Pa_StartStream(stream) != paNoError) {
        return result;
    }
    using namespace std::chrono;
    const auto deadline = steady_clock::now() + duration<double>(seconds + 3.0);
    while (!state.done.load(std::memory_order_acquire) && !mCancelled.load() && steady_clock::now() < deadline) {
        std::this_thread::sleep_for(20ms);
    }
    Pa_AbortStream(stream);
    if (mCancelled.load()) {
        result.status = Status::Cancelled;
        return result;
    }
    if (!state.done.load(std::memory_order_acquire)) {
        return result;
    }

    LatencySignalAnalysis::Analysis best;
    for (unsigned ch = 0; ch < state.inputChannels; ++ch) {
        const auto analysis = LatencySignalAnalysis::Analyse(state.capture[ch], state.bursts, state.periodFrames);
        if (ch == 0 || analysis.burstsFound > best.burstsFound
            || (analysis.burstsFound == best.burstsFound && analysis.minCorrelation > best.minCorrelation)) {
            best = analysis;
            result.inputChannel = static_cast<int>(ch);
        }
        result.inputPeak = std::max(result.inputPeak, analysis.peak);
    }

    if (mCancelled.load()) {
        result.status = Status::Cancelled;
        return result;
    }

    result.roundTripFrames = best.roundTripFrames;
    result.burstsFound = best.burstsFound;
    result.burstsTotal = best.burstsTotal;
    result.spreadFrames = best.spreadFrames;
    result.minCorrelation = best.minCorrelation;
    result.inverted = best.inverted;
    result.status = best.roundTripFrames >= 0 && best.burstsFound * 2 > best.burstsTotal
                    ? Status::Measured : Status::NoSignal;
    return result;
}
