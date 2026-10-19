/*
* Audacity: A Digital Audio Editor
*/
#pragma once

#include "framework/global/modularity/ioc.h"

#include "audio/iaudioengine.h"
#include "audio/iinstrumenteditor.h"
#include "record/irecordconfiguration.h"
#include "audio/driver/iaudiodrivercontroller.h"
#include "framework/global/iglobalconfiguration.h"
#include "framework/audioplugins/iknownaudiopluginsregister.h"

namespace au::tracktionaudio {
class TracktionAudioEngine final : public au::audio::IAudioEngine, public audio::IInstrumentPlayback
{
public:
    TracktionAudioEngine();
    ~TracktionAudioEngine() override;

    muse::GlobalInject<muse::IGlobalConfiguration> globalConfiguration;
    muse::GlobalInject<muse::audioplugins::IKnownAudioPluginsRegister> knownPlugins;
    muse::GlobalInject<audio::IAudioDriverController> audioDriverController;
    muse::GlobalInject<au::record::IRecordConfiguration> recordConfiguration;

    muse::RetVal<std::shared_ptr<audio::IInstrumentEditor> > openInstrument(AudacityProject& project, MidiTrack& track) override;

    void closeInstrument(const std::shared_ptr<audio::IInstrumentEditor>& editor) override;

    void init();
    void deinit();

    bool isBusy() const override;
    bool isCapturing() const override;
    bool isStreamActive(AudacityProject& project) const override;
    double streamTime() const override;
    bool isMonitoring() const override;
    std::optional<audio::AudioStreamDescriptor> currentStream() const override;

    int startStream(const TransportSequences& sequences, double startTime, double endTime, double mixerEndTime, AudacityProject& project,
                    const StartStreamOptions& options) override;

    void stopStream() override;
    void pauseStream(bool pause) override;
    void seekStream(double time) override;

    std::shared_ptr<RealtimeEffectState> addRealtimeEffectState(AudacityProject& project, ChannelGroup* group,
                                                                const std::string& effectId) override;
    void removeRealtimeEffectState(AudacityProject& project, ChannelGroup* group,
                                   const std::shared_ptr<RealtimeEffectState>& state) override;
    std::shared_ptr<RealtimeEffectState> replaceRealtimeEffectState(AudacityProject& project, ChannelGroup* group, size_t effectListIndex,
                                                                    const std::string& newEffectId) override;

    void startMonitoring(AudacityProject& project) override;
    void stopMonitoring() override;

    void setInputVolume(float newInputVolume) override;
    float getInputVolume() const override;
    void setPlaybackVolume(float newPlaybackVolume) override;
    float getPlaybackVolume() const override;

    bool canStopAudioStream(AudacityProject& project) const override;

    void handleDeviceChange() override;
    muse::String lastErrorString() const override;
    double getPlaybackSampleRate() const override;
    void updateTimePosition(unsigned long newlyConsumedSamples) override;
    std::optional<au::audio::AudioCallbackInfo> consumeNextCallbackInfo() override;

    muse::async::Notification recordingUpdateRequested() const override;
    muse::async::Notification recordingCommitRequested() const override;
    muse::async::Notification recordingFinished() const override;
    muse::async::Notification streamStopped() const override;
    muse::async::Channel<au3::Au3TrackId, au3::Au3ClipId> recordingClipChanged() const override;

private:
    struct Data;
    std::unique_ptr<Data> m_data;

    void updateStream();
};
}
