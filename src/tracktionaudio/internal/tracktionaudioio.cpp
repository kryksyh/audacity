/*
* Audacity: A Digital Audio Editor
*/
#include "tracktionaudioio.h"

#include <cassert>

#include <portaudio.h>
#if USE_PORTMIXER
#include <portmixer.h>
#endif

#include "audio/iaudioengine.h"

using namespace au::tracktionaudio;

std::string TracktionAudioIO::init(audio::IAudioEngine& engine)
{
    assert(!ugAudioIO);
    auto state = std::unique_ptr<TracktionAudioIO>(new TracktionAudioIO(engine));
    const auto error = Pa_Initialize();
    state->m_initialised = error == paNoError;
    ugAudioIO = std::move(state);
    return error == paNoError ? std::string() : Pa_GetErrorText(error);
}

void TracktionAudioIO::deinit()
{
    ugAudioIO.reset();
}

TracktionAudioIO& TracktionAudioIO::instance()
{
    return static_cast<TracktionAudioIO&>(*AudioIOBase::Get());
}

TracktionAudioIO::TracktionAudioIO(audio::IAudioEngine& engine)
    : m_engine(engine)
{
    mRate = 44100.0;
    mPortStreamV19 = nullptr;
    mInputMixerWorks = false;
#if USE_PORTMIXER
    mPortMixer = nullptr;
    mPreviousHWPlaythrough = -1.0f;
#endif
    m_playbackVolume.store(AudioIOPlaybackVolume.Read());
}

TracktionAudioIO::~TracktionAudioIO()
{
    clearStream();
    closeMixer();
    if (m_initialised) {
        Pa_Terminate();
    }
}

void TracktionAudioIO::StopStream()
{
    m_engine.stopStream();
}

void TracktionAudioIO::StopMonitoring()
{
    m_engine.stopMonitoring();
    closeMixer();
}

void TracktionAudioIO::setStream(const std::shared_ptr<AudacityProject>& owner, PaStream* stream, int token, double sampleRate)
{
    mOwningProject = owner;
    mPortStreamV19 = stream;
    mStreamToken.store(token);
    mRate = sampleRate;
    mPaused.store(false);
}

void TracktionAudioIO::clearStream()
{
    StopMeters();
    mPortStreamV19 = nullptr;
    mStreamToken.store(0);
    mPaused.store(false);
    mOwningProject.reset();
    mInputMeter.reset();
    mOutputMeter.reset();
}

void TracktionAudioIO::setPaused(bool paused)
{
    mPaused.store(paused);
}

void TracktionAudioIO::refreshDevices()
{
    closeMixer();
    mCurrentPlaybackIndex = -1;
    mCurrentCaptureIndex = -1;
    mCachedPlaybackRates.clear();
    mCachedCaptureRates.clear();
    mCachedSampleRates.clear();
    HandleDeviceChange();
}

void TracktionAudioIO::closeMixer()
{
#if USE_PORTMIXER
    if (mPortMixer) {
#if __WXMAC__
        if (Px_SupportsPlaythrough(mPortMixer) && mPreviousHWPlaythrough >= 0.0f) {
            Px_SetPlaythrough(mPortMixer, mPreviousHWPlaythrough);
        }
#endif
        Px_CloseMixer(mPortMixer);
        mPortMixer = nullptr;
    }
    mPreviousHWPlaythrough = -1.0f;
#endif
    mInputMixerWorks = false;
}

float TracktionAudioIO::inputVolume() const
{
#if USE_PORTMIXER
    if (mPortMixer && mInputMixerWorks) {
        return Px_GetInputVolume(mPortMixer);
    }
#endif
    return m_inputVolume.load(std::memory_order_relaxed);
}

void TracktionAudioIO::setInputVolume(float volume)
{
    m_inputVolume.store(volume, std::memory_order_relaxed);
#if USE_PORTMIXER
    if (mPortMixer && mInputMixerWorks) {
        Px_SetInputVolume(mPortMixer, volume);
    }
#endif
}

float TracktionAudioIO::softwareInputGain() const
{
    return mInputMixerWorks ? 1.0f : m_inputVolume.load(std::memory_order_relaxed);
}

float TracktionAudioIO::playbackVolume() const
{
    return m_playbackVolume.load(std::memory_order_relaxed);
}

void TracktionAudioIO::setPlaybackVolume(float volume)
{
    m_playbackVolume.store(volume, std::memory_order_relaxed);
    AudioIOPlaybackVolume.Write(volume);
}
