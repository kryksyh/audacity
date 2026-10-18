/*
* Audacity: A Digital Audio Editor
*/
#pragma once

#include <memory>
#include <vector>

#include "au3-math/SampleFormat.h"
#include "au3-utility/GlobalVariable.h"

class MidiTrack;
class AudacityProject;
class WaveTrack;

namespace MixerOptions {
class Downmix;
}

class MIXER_API AudioRenderSource
{
public:
    enum class ChannelMode {
        MapChannels, Mixdown
    };

    struct Options {
        double startTime = 0.0;
        double stopTime = 0.0;
        unsigned channels = 2;
        size_t bufferSize = 512;
        bool interleaved = false;
        double sampleRate = 44100.0;
        sampleFormat format = floatSample;
        const MixerOptions::Downmix* channelMap = nullptr;
        ChannelMode channelMode = ChannelMode::MapChannels;
        bool trackEffects = true;
        bool masterEffects = false;
        std::vector<std::shared_ptr<const MidiTrack> > midiTracks;
        bool trackAutomation = true;
        bool trackRouting = true;
    };

    using Tracks = std::vector<std::shared_ptr<const WaveTrack> >;
    struct MIXER_API Factory : GlobalHook<Factory,
                                          std::unique_ptr<AudioRenderSource>(const AudacityProject&, const Tracks&, const Options&)> {};

    static std::unique_ptr<AudioRenderSource> Create(const AudacityProject& project, const Tracks& tracks, const Options& options);

    virtual ~AudioRenderSource() = default;
    virtual size_t Process() = 0;
    virtual size_t Process(size_t maximumFrames) = 0;
    virtual constSamplePtr GetBuffer() = 0;
    virtual constSamplePtr GetBuffer(int channel) = 0;
    virtual double MixGetCurrentTime() = 0;
    virtual sampleFormat EffectiveFormat() const = 0;
};
