/*
* Audacity: A Digital Audio Editor
*/
#include <tracktion_engine/tracktion_engine.h>

#include "tracktiontrackmeters.h"

using namespace au::tracktionaudio;

struct TracktionTrackMeters::Data
{
    struct Track {
        Track(tracktion::AudioTrack& source, tracktion::LevelMeterPlugin* peakMeter, tracktion::LevelMeterPlugin* rmsMeter,
              int64_t trackId, unsigned channelCount)
            : owner(&source), peak(peakMeter), rms(rmsMeter), id(trackId), channels(channelCount)
        {
            peakClient.setNumChannelsUsed(2);
            rmsClient.setNumChannelsUsed(2);
            peak->measurer.setMode(tracktion::LevelMeasurer::peakMode);
            rms->measurer.setMode(tracktion::LevelMeasurer::RMSAccumulatedMode);
            peak->measurer.addClient(peakClient);
            rms->measurer.addClient(rmsClient);
        }

        ~Track()
        {
            peak->measurer.removeClient(peakClient);
            rms->measurer.removeClient(rmsClient);
        }

        tracktion::AudioTrack::Ptr owner;
        juce::ReferenceCountedObjectPtr<tracktion::LevelMeterPlugin> peak;
        juce::ReferenceCountedObjectPtr<tracktion::LevelMeterPlugin> rms;
        tracktion::LevelMeasurer::Client peakClient;
        tracktion::LevelMeasurer::Client rmsClient;
        int64_t id;
        unsigned channels;
    };

    std::vector<std::unique_ptr<Track> > tracks;
};

TracktionTrackMeters::TracktionTrackMeters()
    : m_data(std::make_unique<Data>())
{
}

TracktionTrackMeters::~TracktionTrackMeters() = default;

void TracktionTrackMeters::addTrack(tracktion::AudioTrack& track, int64_t id, unsigned channels)
{
    auto* peak = track.getLevelMeterPlugin();
    auto plugin = track.edit.getPluginCache().createNewPlugin(tracktion::LevelMeterPlugin::create());
    auto* rms = static_cast<tracktion::LevelMeterPlugin*>(plugin.get());
    track.pluginList.insertPlugin(plugin, track.pluginList.indexOf(peak), nullptr);
    m_data->tracks.push_back(std::make_unique<Data::Track>(track, peak, rms, id, channels));
}

void TracktionTrackMeters::send(IMeterSender* sender, size_t frames, TimePoint dacTime)
{
    for (auto& track : m_data->tracks) {
        const auto& transport = track->peak->edit.getTransport();
        const bool active = transport.isPlaying() && !transport.isPaused() && track->owner->shouldBePlayed();
        const int channels = std::max(track->peakClient.getNumChannelsUsed(), track->rmsClient.getNumChannelsUsed());
        IMeterSender::LevelData mono { 0.0f, 0.0f, frames, dacTime };
        for (int channel = 0; channel < channels; ++channel) {
            const auto peak = track->peakClient.getAndClearAudioLevel(channel);
            const auto rms = track->rmsClient.getAndClearAudioLevel(channel);
            const IMeterSender::LevelData levels {
                active ? tracktion::dbToGain(peak.dB) : 0.0f,
                active ? tracktion::dbToGain(rms.dB) : 0.0f,
                frames, dacTime
            };
            if (track->channels == 1) {
                mono.peak = std::max(mono.peak, levels.peak);
                mono.rms = std::max(mono.rms, levels.rms);
            } else if (sender && channel < static_cast<int>(track->channels)) {
                sender->pushLevels(channel, levels, IMeterSender::TrackId { track->id });
            }
        }
        if (sender && track->channels == 1) {
            sender->pushLevels(0, mono, IMeterSender::TrackId { track->id });
        }
    }
}

void TracktionTrackMeters::clear()
{
    m_data->tracks.clear();
}
