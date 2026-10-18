/*!********************************************************************

  Audacity: A Digital Audio Editor

  @file AudioUnitInstance.h

  Dominic Mazzoni
  Leland Lucius

**********************************************************************/

#ifndef __AUDACITY_AUDIO_UNIT_INSTANCE__
#define __AUDACITY_AUDIO_UNIT_INSTANCE__

#include "au3-effects/PerTrackEffect.h"
#include <atomic>

#include "AudioUnitWrapper.h"
struct AudioUnitEvent;

class AudioUnitInstance : public PerTrackEffect::Instance, public AudioUnitWrapper
{
public:
    uint64_t RealtimeLatencyRevision() const noexcept override { return mLatencyRevision.load(std::memory_order_acquire); }
    bool RealtimeLatencyUpdatePending() const noexcept override;
    bool RefreshRealtimeLatency() override;
    bool LatencyRefreshRequiresMainThread() const noexcept override { return mUseLatency; }
    SampleCount GetReportedLatency(const EffectSettings& settings, double sampleRate) const override;
    void SetRealtimeProcessingContext(size_t group, const EffectProcessingContext& context) override;
    void SetRealtimeParameter(AudioUnitParameterID id, AudioUnitParameterValue value);
    using Instance::mProcessor;

    AudioUnitInstance(const PerTrackEffect& effect, AudioComponent component, Parameters& parameters, const wxString& identifier,
                      unsigned audioIns, unsigned audioOuts, bool useLatency);
    ~AudioUnitInstance() override;

    bool Initialize();

    bool IsInitialized() const { return static_cast<bool>(mInitialization); }

    void EventListener(const AudioUnitEvent* inEvent, AudioUnitParameterValue inParameterValue);

    // Override the virtual function to allocate an empty message
    std::unique_ptr<Message> MakeMessage() const override;

    // A non-virtual overload makes a non-empty message
    std::unique_ptr<Message>
    MakeMessage(AudioUnitParameterID id, AudioUnitParameterValue value) const;

private:
    size_t InitialBlockSize() const;
    SampleCount GetLatency(const EffectSettings& settings, double sampleRate)
    const override;

    size_t GetBlockSize() const override;
    size_t SetBlockSize(size_t maxBlockSize) override;

    unsigned GetAudioInCount() const override;
    unsigned GetAudioOutCount() const override;

    bool ProcessInitialize(EffectSettings& settings, double sampleRate, ChannelNames chanMap) override;
    bool ProcessFinalize() noexcept override;
    std::string GetLastError() const override;
    size_t ProcessBlock(EffectSettings& settings, const float* const* inBlock, float* const* outBlock, size_t blockLen)
    override;

    bool RealtimeInitialize(EffectSettings& settings, double sampleRate, size_t audioThreadBufferSize)
    override;
    bool RealtimeAddProcessor(EffectSettings& settings, EffectOutputs* pOutputs, unsigned numChannels, float sampleRate) override;
    bool RealtimeFinalize(EffectSettings& settings) noexcept override;
    bool RealtimeSuspend() override;
    bool RealtimeResume() override;

    bool UsesMessages() const noexcept override;
    bool RealtimeProcessStart(MessagePackage& package) override;
    size_t RealtimeProcess(size_t group, EffectSettings& settings, const float* const* inbuf, float* const* outbuf, size_t numSamples)
    override;
    bool RealtimeProcessEnd(EffectSettings& settings) noexcept override;

    static OSStatus RenderCallback(void* inRefCon, AudioUnitRenderActionFlags* inActionFlags, const AudioTimeStamp* inTimeStamp,
                                   UInt32 inBusNumber, UInt32 inNumFrames, AudioBufferList* ioData);
    OSStatus Render(AudioUnitRenderActionFlags* inActionFlags, const AudioTimeStamp* inTimeStamp, UInt32 inBusNumber, UInt32 inNumFrames,
                    AudioBufferList* ioData);

    bool BypassEffect(bool bypass);

private:
    struct LatencyListenerState { std::atomic<bool> pending { false }; };
    static void LatencyPropertyChanged(void*, AudioUnit, AudioUnitPropertyID, AudioUnitScope, AudioUnitElement);
    bool UpdateCachedLatency();
    std::shared_ptr<LatencyListenerState> mLatencyListener = std::make_shared<LatencyListenerState>();
    bool mLatencyListenerInstalled = false;
    std::atomic<double> mCachedLatencySeconds { 0.0 };
    std::atomic<uint64_t> mLatencyRevision { 0 };
    static OSStatus BeatAndTempo(void*, Float64*, Float64*);
    static OSStatus MusicalTime(void*, UInt32*, Float32*, UInt32*, Float64*);
    static OSStatus TransportState(void*, Boolean*, Boolean*, Float64*, Boolean*, Float64*, Float64*);
    EffectProcessingContext mTransport;
    double mNextTransportTime = 0.0;
    bool mHaveTransport = false;
    bool mTransportChanged = true;
    //! Whether the master instance is now allocated to a group number
    bool mRecruited{ false };
    std::vector<std::unique_ptr<AudioUnitInstance> > mSlaves;

    AudioUnitCleanup<AudioUnit, AudioUnitUninitialize> mInitialization;
    AudioTimeStamp mTimeStamp{};
    PackedArray::Ptr<AudioBufferList> mInputList;
    PackedArray::Ptr<AudioBufferList> mOutputList;

    const wxString& mIdentifier; // for debug messages only
    const size_t mBlockSize;
    const bool mUseLatency;
    double mInitializedSampleRate = 0.0;
    std::string mLastError;
    std::atomic<bool> mRealtimeErrorReported{ false };
};
#endif
