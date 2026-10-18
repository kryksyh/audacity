/**********************************************************************

  Audacity: A Digital Audio Editor

  @file RealtimeEffectState.cpp

  Paul Licameli split from RealtimeEffectManager.cpp

 *********************************************************************/

#include "RealtimeEffectState.h"

#include <wx/arrstr.h>

#include <thread>
#include <condition_variable>

#include "au3-channel/Channel.h"
#include "au3-components/EffectInterface.h"
#include "au3-utility/MessageBuffer.h"
#include "au3-module-manager/PluginManager.h"
#include "au3-math/SampleCount.h"

//! Mediator of two-way inter-thread communication of changes of settings
class RealtimeEffectState::AccessState : public NonInterferingBase
{
public:
    AccessState(const EffectSettingsManager& effect, RealtimeEffectState& state)
        : mEffect{effect}
        , mState{state}
    {
        // Clean initial state of the counter
        state.mMainSettings.counter = 0;
        Initialize(state.mMainSettings.settings,
                   state.mMessage.get(), state.mMovedOutputs.get());
    }

    void Initialize(const EffectSettings& settings,
                    const EffectInstance::Message* pMessage,
                    const EffectOutputs* pOutputs)
    {
        mLastSettings = { settings, 0 };
        // Initialize each message buffer with two copies
        mChannelToMain.Write(ToMainSlot { { 0,
                                 pOutputs ? pOutputs->Clone() : nullptr } });
        mChannelToMain.Write(ToMainSlot { { 0,
                                 pOutputs ? pOutputs->Clone() : nullptr } });
        mChannelFromMain.Write(FromMainSlot { settings, pMessage });
        mChannelFromMain.Write(FromMainSlot { settings, pMessage });

        mMainThreadId = std::this_thread::get_id();
    }

    void MainRead()
    {
        mChannelToMain.Read<ToMainSlot::Reader>(mState.mMovedOutputs.get(),
                                                mCounter);
    }

    void MainWrite(SettingsAndCounter&& settings,
                   std::unique_ptr<EffectInstance::Message> pMessage)
    {
        // Main thread may simply swap new content into place
        mChannelFromMain.Write(FromMainSlot::Message {
            std::move(settings.settings), settings.counter, std::move(pMessage) });
    }

    void MainWrite(SettingsAndCounter::Counter counter,
                   std::unique_ptr<EffectInstance::Message> pMessage)
    {
        mChannelFromMain.Write(FromMainSlot::ShortMessage {
            counter, std::move(pMessage) });
    }

    struct CounterAndOutputs {
        Response::Counter counter;
        EffectOutputs* pOutputs{};
    };
    void WorkerRead()
    {
        // Worker thread avoids memory allocation.  It copies the contents of any
        mChannelFromMain.Read<FromMainSlot::Reader>(mEffect, mState);
    }

    void WorkerWrite()
    {
        {
            std::unique_lock lk(mLockForCV);

            // Worker thread avoids memory allocation.
            mChannelToMain.Write(CounterAndOutputs {
                mState.mWorkerSettings.counter, mState.mOutputs.get() });
        }
        mCV.notify_one();
    }

    struct ToMainSlot {
        // For initialization of the channel
        ToMainSlot() = default;
        explicit ToMainSlot(Response response)
            : mResponse{std::move(response)}
        {}
        ToMainSlot& operator=(ToMainSlot&&) = default;

        // Worker thread writes the slot
        ToMainSlot& operator=(CounterAndOutputs&& arg)
        {
            // This happens during MessageBuffer's busying of the slot
            mResponse.counter = arg.counter;
            if (mResponse.pOutputs && arg.pOutputs) {
                mResponse.pOutputs->Assign(std::move(*arg.pOutputs));
            }
            return *this;
        }

        struct Reader {
            Reader(ToMainSlot&& slot, EffectOutputs* pOutputs,
                   Response::Counter& counter)
            {
                // Main thread is not under the performance constraints of the
                // worker, but Assign is still used so that
                // members of underlying vectors or other containers do not
                // relocate
                if (pOutputs && slot.mResponse.pOutputs) {
                    pOutputs->Assign(std::move(*slot.mResponse.pOutputs));
                }
                counter = slot.mResponse.counter;
            }
        };

        Response mResponse;
    };

    struct FromMainSlot {
        struct Message : SettingsAndCounter {
            std::unique_ptr<EffectInstance::Message> pMessage;
        };
        struct ShortMessage {
            SettingsAndCounter::Counter counter;
            std::unique_ptr<EffectInstance::Message> pMessage;
        };

        // For initialization of the channel
        FromMainSlot() = default;
        explicit FromMainSlot(const EffectSettings& settings,
                              const EffectInstance::Message* pMessage)
        // Copy std::any
            : mMessage{settings, 0, pMessage ? pMessage->Clone() : nullptr}
        {}
        FromMainSlot& operator=(FromMainSlot&&) = default;

        // Main thread writes the slot
        FromMainSlot& operator=(Message&& message)
        {
            mMessage.SettingsAndCounter::swap(message);
            if (message.pMessage && mMessage.pMessage) {
                // Merge the incoming message with any still unconsumed message
                mMessage.pMessage->Merge(std::move(*message.pMessage));
            }
            return *this;
        }

        // Main thread writes the slot
        FromMainSlot& operator=(ShortMessage&& message)
        {
            mMessage.counter = message.counter;
            if (message.pMessage && mMessage.pMessage) {
                // Merge the incoming message with any still unconsumed message
                mMessage.pMessage->Merge(std::move(*message.pMessage));
            }
            return *this;
        }

        // Worker thread reads the slot
        struct Reader {
            Reader(FromMainSlot&& slot,
                   const EffectSettingsManager& effect, RealtimeEffectState& state)
            {
                auto& settings = state.mWorkerSettings;
                if (slot.mMessage.counter == settings.counter) {
                    return;//copy once
                }
                settings.counter = slot.mMessage.counter;

                // This happens during MessageBuffer's busying of the slot
                effect.CopySettingsContents(
                    slot.mMessage.settings, settings.settings);
                settings.settings.extra = slot.mMessage.settings.extra;
                if (slot.mMessage.pMessage && state.mMovedMessage) {
                    // Copy the message from the buffer (not a merge)
                    state.mMovedMessage->Assign(std::move(*slot.mMessage.pMessage));
                }
            }
        };

        Message mMessage;
    };

    const EffectSettingsManager& mEffect;
    RealtimeEffectState& mState;

    MessageBuffer<FromMainSlot> mChannelFromMain;
    Response::Counter mCounter;
    SettingsAndCounter mLastSettings;
    // A main-thread cache generation, independent of the small inter-thread
    // acknowledgement counter and its resets during processor initialization.
    uint64_t mModificationCounter{ 0 };

    MessageBuffer<ToMainSlot> mChannelToMain;

    std::mutex mLockForCV;
    std::condition_variable mCV;

    std::thread::id mMainThreadId;
};

//! Main thread's interface to inter-thread communication of changes of settings
struct RealtimeEffectState::Access final : EffectSettingsAccess {
    Access() = default;
    explicit Access(RealtimeEffectState& state)
        : mwState{state.weak_from_this()}
    {
    }

    ~Access() override = default;

    const EffectSettings& Get() override
    {
        if (auto pState = mwState.lock()) {
            if (auto pAccessState = pState->GetAccessState()) {
                if (pAccessState->mState.mInitialized) {
                    // try once
                    assert(pAccessState->mState.mInitialized);
                    auto& lastSettings = pAccessState->mLastSettings;
                    // Assigns to mCounter
                    pAccessState->MainRead();
                } else {
                    // Not yet waiting on the other thread's progress
                    // Not necessarily values yet in the state's Settings objects
                }
                return pAccessState->mLastSettings.settings;
            }
        }
        // Non-modal dialog may have outlived the RealtimeEffectState
        static EffectSettings empty;
        return empty;
    }

    void Set(EffectSettings&& settings, std::unique_ptr<Message> pMessage)
    override
    {
        if (auto pState = mwState.lock()) {
            if (auto pAccessState = pState->GetAccessState()) {
                ++pAccessState->mModificationCounter;
                if (pMessage && !pAccessState->mState.mInitialized) {
                    // Other thread isn't processing.
                    // Let the instance consume the message directly.
                    if (auto pInstance = pState->mwInstance.lock()) {
                        auto& stateSettings = pState->mMainSettings.settings;
                        stateSettings = std::move(settings);
                        EffectInstance::MessagePackage package{
                            stateSettings, pMessage.get()
                        };
                        pInstance->RealtimeProcessStart(package);
                        pInstance->RealtimeProcessEnd(stateSettings);
                        pAccessState->mLastSettings.settings = stateSettings;
                        return;
                    }
                }
                auto& lastSettings = pAccessState->mLastSettings;
                // move to remember values here
                lastSettings.settings = std::move(settings);
                ++lastSettings.counter;
                // move a copy to there
                pAccessState->MainWrite(
                    SettingsAndCounter { lastSettings }, std::move(pMessage));
            }
        }
    }

    void Set(std::unique_ptr<Message> pMessage)
    override
    {
        if (auto pState = mwState.lock()) {
            if (auto pAccessState = pState->GetAccessState()) {
                ++pAccessState->mModificationCounter;
                if (pMessage && !pAccessState->mState.mInitialized) {
                    // Other thread isn't processing.
                    // Let the instance consume the message directly.
                    if (auto pInstance = pState->mwInstance.lock()) {
                        auto& stateSettings = pState->mMainSettings.settings;
                        EffectInstance::MessagePackage package{
                            stateSettings, pMessage.get()
                        };
                        pInstance->RealtimeProcessStart(package);
                        pInstance->RealtimeProcessEnd(stateSettings);
                        // Don't need to update pAccessState->mLastSettings
                        return;
                    }
                }
                auto& lastSettings = pAccessState->mLastSettings;
                // Don't update settings, but do count
                ++lastSettings.counter;
                pAccessState->MainWrite(
                    lastSettings.counter, std::move(pMessage));
            }
        }
    }

    void Flush() override
    {
        if (auto pState = mwState.lock()) {
            if (auto pAccessState = pState->GetAccessState()) {
                assert(pAccessState->mMainThreadId == std::this_thread::get_id());

                if (pAccessState->mState.mInitialized) {
                    std::unique_lock lk(pAccessState->mLockForCV);
                    pAccessState->mCV.wait(lk,
                                           [&] {
                        auto& lastSettings = pAccessState->mLastSettings;
                        pAccessState->MainRead();
                        return pAccessState->mCounter == lastSettings.counter;
                    }
                                           );
                }

                // Update what GetSettings() will return, during play and before
                // Finalize(), but after it is confirmed that any worker thread has
                // seen the values given to the last Set().  These values will also
                // be returned by Get().
                pState->mMainSettings.Set(pAccessState->mLastSettings);
            }
        }
    }

    bool IsSameAs(const EffectSettingsAccess& other) const override
    {
        if (auto pOther = dynamic_cast<const Access*>(&other)) {
            auto& mine = mwState;
            auto& theirs = pOther->mwState;
            auto less = std::owner_less{};
            return !(less(mine, theirs) || less(theirs, mine));
        }
        return false;
    }

    uint64_t modificationCounter() const override
    {
        if (auto pState = mwState.lock()) {
            if (auto pAccessState = pState->GetAccessState()) {
                return pAccessState->mModificationCounter;
            }
        }
        return 0;
    }

    //! Store no state here but this weak pointer, so `IsSameAs` isn't lying
    std::weak_ptr<RealtimeEffectState> mwState;
};

RealtimeEffectState::RealtimeEffectState(const PluginID& id)
{
    SetID(id);
    BuildAll();
}

RealtimeEffectState& RealtimeEffectState::operator=(const RealtimeEffectState& other)
{
    if (this == &other) {
        return *this;
    }
    assert(other.mID == mID);
    mAutomation = other.mAutomation;
    mParameters = other.SnapshotParameters();
    auto settings = mPlugin ? mPlugin->MakeSettings() : EffectSettings {};
    settings.extra = other.SnapshotSettings().extra;
    if (mPlugin) {
        if (LoadParameters(settings)) {
            mParameters.clear();
        }
        auto access = GetAccess();
        access->Set(std::move(settings), nullptr);
        access->Flush();
    } else {
        mMainSettings.settings = std::move(settings);
    }
    Publish(IsEnabled() ? RealtimeEffectStateChange::EffectOn : RealtimeEffectStateChange::EffectOff);
    return *this;
}

RealtimeEffectState::~RealtimeEffectState()
{
}

std::shared_ptr<RealtimeEffectState> RealtimeEffectState::Clone() const
{
    auto pNewState{ RealtimeEffectState::make_shared(PluginID {}) };
    pNewState->mAutomation = mAutomation;
    pNewState->mParameters = mParameters;
    pNewState->mID = mID;
    pNewState->mPlugin = mPlugin;
    if (mPlugin) {
        pNewState->mOutputs = mPlugin->MakeOutputs();
        pNewState->mMovedOutputs = mPlugin->MakeOutputs();
    }
    pNewState->mMainSettings.Set(mMainSettings);
    pNewState->mMainSettings.settings = SnapshotSettings();
    return pNewState;
}

const EffectSettings& RealtimeEffectState::SnapshotSettings() const
{
    const auto access = GetAccessState();
    return access ? access->mLastSettings.settings : mMainSettings.settings;
}

RealtimeEffectState::Parameters RealtimeEffectState::SnapshotParameters() const
{
    if (!mParameters.empty()) {
        return mParameters;
    }
    Parameters result;
    CommandParameters parameters;
    if (mPlugin && mPlugin->SaveSettings(SnapshotSettings(), parameters)) {
        wxString name;
        long index;
        for (bool more = parameters.GetFirstEntry(name, index); more; more = parameters.GetNextEntry(name, index)) {
            result.emplace_back(name, parameters.Read(name, ""));
        }
    }
    return result;
}

bool RealtimeEffectState::LoadParameters(EffectSettings& settings) const
{
    if (!mPlugin) {
        return false;
    }
    CommandParameters parameters;
    for (const auto& [name, value] : mParameters) {
        if (!parameters.Write(name, value)) {
            return false;
        }
    }
    return mPlugin->LoadSettings(parameters, settings);
}

void RealtimeEffectState::SetID(const PluginID& id)
{
    bool empty = id.empty();
    if (mID.empty() && !empty) {
        mID = id;
        GetEffect();
    } else {
        // Set mID to non-empty at most once
        assert(empty);
    }
}

const PluginID& RealtimeEffectState::GetID() const noexcept
{
    return mID;
}

const EffectInstanceFactory* RealtimeEffectState::GetEffect()
{
    if (!mPlugin) {
        mPlugin = EffectFactory::Call(mID);
        if (mPlugin) {
            // Also make EffectSettings, but preserve activation
            auto wasActive = mMainSettings.settings.extra.GetActive();
            mMainSettings.counter = 0;
            mMainSettings.settings = mPlugin->MakeSettings();
            mMainSettings.settings.extra.SetActive(wasActive);
            if (!mParameters.empty() && LoadParameters(mMainSettings.settings)) {
                mParameters.clear();
            }
            mOutputs = mPlugin->MakeOutputs();
            mMovedOutputs = mPlugin->MakeOutputs();
        }
    }
    return mPlugin;
}

std::shared_ptr<EffectInstance> RealtimeEffectState::MakeInstance()
{
    if (const auto access = GetAccessState()) {
        mMainSettings.Set(access->mLastSettings);
    }
    mMovedMessage.reset();
    mMessage.reset();
    auto result = mPlugin->MakeInstance();
    if (result) {
        // Allocate presized containers in messages, so later
        // copies of contents might avoid free store operations
        mMessage = result->MakeMessage();
        mMovedMessage = result->MakeMessage();
        if (auto state = GetAccessState()) {
            state->Initialize(mMainSettings.settings,
                              mMessage.get(), mMovedOutputs.get());
        }
    }
    return result;
}

std::shared_ptr<EffectInstance>
RealtimeEffectState::EnsureInstance(double sampleRate, size_t audioThreadBufferSize)
{
    if (!mPlugin) {
        return {};
    }

    auto pInstance = mwInstance.lock();
    if (!mInitialized) {
        if (const auto access = GetAccessState()) {
            mMainSettings.Set(access->mLastSettings);
        }
        //! copying settings in the main thread while worker isn't yet running
        mWorkerSettings = mMainSettings;
        mLastActive = IsActive();

        //! If there was already an instance, recycle it; else make one here
        if (!pInstance) {
            mwInstance = pInstance = MakeInstance();
        }
        if (!pInstance) {
            return {};
        }

        // PRL: conserving pre-3.2.0 behavior, but I don't know why this arbitrary
        // number was important
        pInstance->SetBlockSize(512);

        if (!pInstance->RealtimeInitialize(mMainSettings.settings, sampleRate, audioThreadBufferSize)) {
            return {};
        }
        mInitialized = true;
        mProcessingRate = sampleRate;
        RefreshProcessingAutomation();
        return pInstance;
    }
    return pInstance;
}

std::shared_ptr<EffectInstance> RealtimeEffectState::GetInstance()
{
    //! If there was already an instance, recycle it; else make one here
    auto pInstance = mwInstance.lock();
    if (!pInstance && mPlugin) {
        mwInstance = pInstance = MakeInstance();
    }
    return pInstance;
}

std::shared_ptr<EffectInstance>
RealtimeEffectState::Initialize(double sampleRate, size_t audioThreadBufferSize)
{
    if (!mPlugin) {
        return {};
    }

    mCurrentProcessor = 0;
    mGroups.clear();
    mRawDelays.clear();
    mLatency = {};
    return EnsureInstance(sampleRate, audioThreadBufferSize);
}

namespace {
// The caller passes the number of channels to process and specifies
// the number of input and output buffers.  There will always be the
// same number of output buffers as there are input buffers.
//
// Effects require a certain number of input and output buffers.
// The number of channels we're currently processing may mismatch
// the effect's requirements.  Allocate some inputs repeatedly to a processor
// that needs more, or allocate multiple processors if they accept too few.
// Continue until the output buffers are all allocated.
template<typename F>
void AllocateChannelsToProcessors(
    unsigned chans, const unsigned numAudioIn, const unsigned numAudioOut,
    const F& f)
{
    unsigned indx = 0;
    for (unsigned ondx = 0; ondx < chans; ondx += numAudioOut) {
        // Pass the function indices into the arrays of buffers
        if (!f(indx, ondx)) {
            return;
        }
        indx += numAudioIn;
        indx %= chans;
    }
}
}

//! Set up processors to be visited repeatedly in Process.
/*! The iteration over channels in AddGroup and Process must be the same */
std::shared_ptr<EffectInstance>
RealtimeEffectState::AddGroup(
    const ChannelGroup* group, unsigned chans, float sampleRate, size_t audioThreadBufferSize)
{
    auto pInstance = EnsureInstance(sampleRate, audioThreadBufferSize);
    if (!pInstance) {
        return {};
    }
    if (!mPlugin) {
        return {};
    }
    auto first = mCurrentProcessor;
    const auto numAudioIn = pInstance->GetAudioInCount();
    const auto numAudioOut = pInstance->GetAudioOutCount();
    AllocateChannelsToProcessors(chans, numAudioIn, numAudioOut,
                                 [&](unsigned, unsigned){
        // Add a NEW processor
        if (pInstance->RealtimeAddProcessor(
                mWorkerSettings.settings, mOutputs.get(), numAudioIn, sampleRate)
            ) {
            mCurrentProcessor++;
            return true;
        } else {
            return false;
        }
    });
    if (mCurrentProcessor > first) {
        // Remember the sampleRate of the group, so latency can be computed
        // later
        mGroups[group] = { first, sampleRate };
        mRawDelays.try_emplace(group);
        return pInstance;
    }
    return {};
}

bool RealtimeEffectState::ProcessStart(bool running)
{
    // Get state changes from the main thread
    // Note that it is only here that the answer of IsActive() may be changed,
    // and it is important that for each state the answer is unchanging in one
    // processing scope.
    if (auto pAccessState = TestAccessState()) {
        pAccessState->WorkerRead();
    }

    // Detect transitions of activity state
    auto pInstance = mwInstance.lock();
    if (!mInitialized) {
        mLastActive = false;
        return false;
    }
    bool active = IsActive() && running;
    if (active != mLastActive) {
        if (pInstance) {
            bool success = active
                           ? pInstance->RealtimeResume()
                           : pInstance->RealtimeSuspend();
            if (!success) {
                return false;
            }
        }
        mLastActive = active;
    }

    bool result = false;
    if (pInstance) {
        // Consume messages even if not processing
        // (issue #3855: plain UI for VST 2 effects)

        // Assuming we are in a processing scope, use the worker settings
        EffectInstance::MessagePackage package{
            mWorkerSettings.settings, mMovedMessage.get()
        };
        result = pInstance->RealtimeProcessStart(package);
    }

    if (!pInstance || !active) {
        return false;
    } else {
        return result;
    }
}

#define stackAllocate(T, count) static_cast<T*>(alloca(count * sizeof(T)))

//! Visit the effect processors that were added in AddGroup
/*! The iteration over channels in AddGroup and Process must be the same */
size_t RealtimeEffectState::Process(
    const ChannelGroup* group, unsigned chans,
    const float* const* inbuf, float* const* outbuf, float* const dummybuf,
    size_t numSamples, const RealtimeEffects::ProcessingTime* time, bool rawLatency, bool probe)
{
    const auto pInstance = mwInstance.lock();
    const auto found = mGroups.find(group);
    if (!mInitialized || found == mGroups.end()) {
        // A native plug-in can refuse the current format while its editor is
        // still open. Leave the samples dry; never process that UI instance.
        for (size_t channel = 0; channel < chans; ++channel) std::copy_n(inbuf[channel], numSamples, outbuf[channel]);
        return 0;
    }
    struct RestoreProbe {
        EffectInstance* instance;
        bool active;
        ~RestoreProbe() { if (instance) { instance->RealtimeSuspend(); if (active) instance->RealtimeResume(); } }
    } restore { probe ? pInstance.get() : nullptr, mLastActive };
    if (probe && pInstance) {
        pInstance->RealtimeResume();
        EffectInstance::MessagePackage package { mWorkerSettings.settings, nullptr };
        pInstance->RealtimeProcessStart(package);
    }
    const auto automation = time ? std::atomic_load_explicit(&mProcessingAutomation, std::memory_order_acquire) : nullptr;
    const auto& pair = found->second;
    RawDelay* delay = rawLatency ? &mRawDelays[group] : nullptr;
    // Probes run on the disk worker or on main with monitoring detached. A
    // device callback only requests this rebuild; it never resizes histories.
    if (probe && delay && pInstance && delay->revision != pInstance->RealtimeLatencyRevision()) delay->ready = false;
    const bool delayReady = delay && delay->ready;
    const auto dry = [&](bool produce) {
        if (!delay || !delay->latency) {
            if (produce) for (size_t channel = 0; channel < chans; ++channel) std::copy_n(inbuf[channel], numSamples, outbuf[channel]);
            return;
        }
        for (size_t frame = 0; frame < numSamples; ++frame) {
            for (size_t channel = 0; channel < chans; ++channel) {
                const auto source = inbuf[channel][frame];
                if (produce) outbuf[channel][frame] = delay->history[channel][delay->position];
                delay->history[channel][delay->position] = source;
            }
            if (++delay->position == delay->latency) delay->position = 0;
        }
    };
    if (delayReady && (mLastActive || probe)) dry(false);
    const float** const clientIn
        =pInstance ? stackAllocate(const float*, pInstance->GetAudioInCount())
          : nullptr;

    const auto PointAtCorrectInputBuffers
        =[&](unsigned numAudioIn, unsigned indx) {
        // Point at the correct input buffers
        unsigned copied = std::min(chans - indx, numAudioIn);
        std::copy(inbuf + indx, inbuf + indx + copied, clientIn);
        // If there are too few input channels for what the processor requires,
        // re-use input channels from the beginning
        while (auto need = numAudioIn - copied)
        {
            auto moreCopied = std::min(chans, need);
            std::copy(inbuf, inbuf + moreCopied, clientIn + copied);
            copied += moreCopied;
        }
    };

    if (!mPlugin || !pInstance || (!mLastActive && !probe)) {
        // Process trivially
        dry(true);
        if (pInstance) {
            auto processor = pair.first;
            const auto numAudioIn = pInstance->GetAudioInCount();
            const auto numAudioOut = pInstance->GetAudioOutCount();
            AllocateChannelsToProcessors(
                chans, numAudioIn, numAudioOut, [&](unsigned indx, unsigned ondx) {
                PointAtCorrectInputBuffers(numAudioIn, indx);

                // Inner loop over blocks
                const auto blockSize = pInstance->GetBlockSize();
                for (size_t block = 0; block < numSamples; block += blockSize) {
                    auto cnt = std::min(numSamples - block, blockSize);
                    pInstance->RealtimePassThrough(
                        processor, mWorkerSettings.settings, clientIn, cnt);
                    for (size_t i = 0; i < numAudioIn; i++) {
                        if (clientIn[i]) {
                            clientIn[i] += cnt;
                        }
                    }
                }
                ++processor;
                return true;
            });
        }
        return 0;
    }
    const auto numAudioIn = pInstance->GetAudioInCount();
    const auto numAudioOut = pInstance->GetAudioOutCount();
    const auto clientOut = stackAllocate(float*, numAudioOut);
    size_t len = 0;
    size_t rawMissing = 0;
    auto processor = pair.first;
    // Outer loop over processors
    AllocateChannelsToProcessors(
        chans, numAudioIn, numAudioOut, [&](unsigned indx, unsigned ondx) {
        PointAtCorrectInputBuffers(numAudioIn, indx);

        // Point at the correct output buffers
        unsigned copied = std::min(chans - ondx, numAudioOut);
        std::copy(outbuf + ondx, outbuf + ondx + copied, clientOut);
        if (copied < numAudioOut) {
            // Make determinate pointers
            std::fill(clientOut + copied, clientOut + numAudioOut, dummybuf);
        }

        // Inner loop over blocks
        size_t processorMissing = 0;
        const auto blockSize = pInstance->GetBlockSize();
        for (size_t block = 0; block < numSamples; block += blockSize) {
            auto cnt = std::min(numSamples - block, blockSize);
            if (time) {
                auto context = time->ContextAt(static_cast<int64_t>(block));
                context.sampleRate = pair.second;
                pInstance->SetRealtimeProcessingContext(processor, context);
            }
            if (automation) {
                const auto position = time->TimeAt(static_cast<int64_t>(block));
                automation->BeforeProcessStart(position);
                // The outer processing scope already consumed GUI messages.
                // Refresh cached coefficients without consuming a message twice.
                EffectInstance::MessagePackage package { mWorkerSettings.settings, nullptr };
                pInstance->RealtimeProcessStart(package);
                automation->AfterProcessStart(position);
            }
            // Assuming we are in a processing scope, use the worker settings
            auto processed = pInstance->RealtimeProcess(
                processor, mWorkerSettings.settings, clientIn, clientOut, cnt);
            if (probe) {
                // A fixed-block adapter may need several small device slices
                // before the plug-in runs and publishes its latency port. Warm
                // a complete block before reading it, then RestoreProbe resets
                // the processor. Reuse these bounded silent buffers.
                for (size_t warmed = cnt; warmed < blockSize; warmed += cnt) {
                    pInstance->RealtimeProcess(processor, mWorkerSettings.settings, clientIn, clientOut,
                                               std::min(cnt, blockSize - warmed));
                }
            }
            if (rawLatency) {
                processed = std::min(processed, cnt);
                processorMissing += cnt - processed;
                for (size_t channel = 0; channel < numAudioOut; ++channel) {
                    std::fill(clientOut[channel] + processed, clientOut[channel] + cnt, 0.0f);
                }
            }
            if (!mLatency) {
                // Find latency once only per initialization scope,
                // after processing one block
                mLatency.emplace(
                    pInstance->GetLatency(mWorkerSettings.settings, pair.second));
            }
            if (delay && !delay->ready) {
                delay->latency = static_cast<size_t>(pInstance->GetLatency(mWorkerSettings.settings, pair.second));
                delay->history.assign(chans, std::vector<float>(delay->latency));
                delay->position = 0;
                delay->revision = pInstance->RealtimeLatencyRevision();
                delay->ready = true;
            }
            for (size_t i = 0; i < numAudioIn; i++) {
                if (clientIn[i]) {
                    clientIn[i] += cnt;
                }
            }
            for (size_t i = 0; i < numAudioOut; i++) {
                if (clientOut[i]) {
                    clientOut[i] += cnt;
                }
            }
            if (ondx == 0) {
                // For the first processor only
                len += processed;
                auto discard = rawLatency ? 0 : limitSampleBufferSize(len, *mLatency);
                len -= discard;
                *mLatency -= discard;
            }
        }
        rawMissing = std::max(rawMissing, processorMissing);
        ++processor;
        return true;
    });
    if (delay && !delayReady) dry(false);
    // Report the number discardable during the processing scope
    // We are assuming len as calculated above is the same in case of multiple
    // processors
    return rawLatency ? rawMissing : numSamples - len;
}

bool RealtimeEffectState::ProcessEnd()
{
    auto pInstance = mwInstance.lock();
    bool result = mInitialized && pInstance
                  &&// Assuming we are in a processing scope, use the worker settings
                  pInstance->RealtimeProcessEnd(mWorkerSettings.settings)
                  && IsActive() && mLastActive;

    if (auto pAccessState = TestAccessState()) {
        // Always done, regardless of activity
        // Some dialogs require communication back from the processor so that
        // they can update their appearance in idle time, and some plug-in
        // libraries (like lv2) require the host program to mediate the
        // communication
        pAccessState->WorkerWrite();
    }

    return result;
}

void RealtimeEffectState::RefreshProcessingAutomation()
{
    std::erase_if(mRetiredAutomation, [](const auto& item) { return item.use_count() == 1; });
    std::shared_ptr<ProcessingAutomation> next;
    if (const auto instance = mwInstance.lock(); mInitialized && mPlugin && instance) {
        next = AutomationFactory::Call(*mPlugin, *instance, mWorkerSettings.settings, mAutomation, mProcessingRate);
    }
    if (auto previous = std::atomic_exchange_explicit(&mProcessingAutomation, std::move(next), std::memory_order_acq_rel)) {
        mRetiredAutomation.push_back(std::move(previous));
    }
}

uint64_t RealtimeEffectState::ProcessingLatencyRevision() const
{
    const auto instance = mwInstance.lock();
    return instance ? instance->RealtimeLatencyRevision() : 0;
}

bool RealtimeEffectState::LatencyUpdatePending() const
{
    const auto instance = mwInstance.lock();
    return mInitialized && instance && instance->RealtimeLatencyUpdatePending();
}

bool RealtimeEffectState::RefreshRealtimeLatency()
{
    const auto instance = mwInstance.lock();
    return mInitialized && instance && instance->RefreshRealtimeLatency();
}

size_t RealtimeEffectState::ProcessingLatency(const ChannelGroup* group) const
{
    const auto delay = mRawDelays.find(group);
    if (delay != mRawDelays.end() && delay->second.ready) return delay->second.latency;
    const auto instance = mwInstance.lock();
    const auto found = mGroups.find(group);
    return instance && found != mGroups.end()
        ? static_cast<size_t>(instance->GetLatency(mWorkerSettings.settings, found->second.second)) : 0;
}

bool RealtimeEffectState::IsEnabled() const noexcept
{
    return mMainSettings.settings.extra.GetActive();
}

bool RealtimeEffectState::IsActive() const noexcept
{
    return mWorkerSettings.settings.extra.GetActive();
}

void RealtimeEffectState::SetActive(bool active)
{
    if (!mPlugin) {
        // There is no processing thread or functional settings access for a
        // missing plugin. Bypass still belongs to the document.
        mMainSettings.settings.extra.SetActive(active);
    } else {
        auto access = GetAccess();
        access->ModifySettings([&](EffectSettings& settings) {
            settings.extra.SetActive(active);
            return nullptr;
        });
        access->Flush();
    }

    Publish(active
            ? RealtimeEffectStateChange::EffectOn
            : RealtimeEffectStateChange::EffectOff);
}

bool RealtimeEffectState::Finalize() noexcept
{
    std::atomic_store_explicit(&mProcessingAutomation, std::shared_ptr<ProcessingAutomation> {}, std::memory_order_release);
    mRetiredAutomation.clear();
    mRawDelays.clear();
    mGroups.clear();
    mCurrentProcessor = 0;

    auto pInstance = mwInstance.lock();
    if (!pInstance) {
        return false;
    }

    if (!pInstance->UsesMessages()) {
        // This is the main thread cleaning up a state not now used in processing
        mMainSettings = mWorkerSettings;
    }

    auto result = pInstance->RealtimeFinalize(mMainSettings.settings);
    mLatency = {};
    mInitialized = false;
    return result;
}

const std::string& RealtimeEffectState::XMLTag()
{
    static const std::string result{ "effect" };
    return result;
}

static const auto idAttribute = "id";
static const auto versionAttribute = "version";
static const auto parametersAttribute = "parameters";
static const auto parameterAttribute = "parameter";
static const auto nameAttribute = "name";
static const auto valueAttribute = "value";
static constexpr auto activeAttribute = "active";

static wxArrayString splitEffectId(const PluginID& id)
{
    wxArrayString parts;
    wxString part;
    for (size_t i = 0; i < id.length(); ++i) {
        const wxChar ch = id[i];
        if (ch == '\\' && i + 1 < id.length() && id[i + 1] == '_') {
            part += '_';
            ++i;
        } else if (ch == '_') {
            parts.push_back(part);
            part.clear();
        } else {
            part += ch;
        }
    }
    parts.push_back(part);
    return parts;
}

static PluginID normalizeEffectIdPathSeparators(const PluginID& id)
{
    auto parts = splitEffectId(id);
    if (parts.size() != 5 || parts[0] != wxString("Effect")) {
        return id;
    }

    wxString& path = parts[4];
    if (path.Find('\\') == wxNOT_FOUND) {
        return id;
    }

    path.Replace("\\", "/");
    return wxJoin(parts, '_');
}

static PluginID resolveEffectId(const PluginID& id)
{
    if (const PluginID resolved = RealtimeEffectState::EffectIdResolver::Call(id); !resolved.empty()) {
        return resolved;
    }
    return id;
}

bool RealtimeEffectState::HandleXMLTag(
    const std::string_view& tag, const AttributesList& attrs)
{
    if (tag == XMLTag()) {
        mAutomation.Clear();
        mParameters.clear();
        mPlugin = nullptr;
        mID.clear();
        for (auto&[attr, value] : attrs) {
            if (attr == idAttribute) {
                SetID(resolveEffectId(normalizeEffectIdPathSeparators(value.ToWString())));
                if (!mPlugin) {
                    // TODO - complain!!!!
                }
            } else if (attr == activeAttribute) {
                // Updating the EffectSettingsExtra although we haven't yet built
                // the settings
                mMainSettings.settings.extra.SetActive(value.Get<bool>());
            }
        }
        return true;
    } else if (tag == parametersAttribute) {
        return true;
    } else if (tag == parameterAttribute) {
        wxString n;
        wxString v;
        for (auto&[attr, value] : attrs) {
            if (attr == nameAttribute) {
                n = value.ToWString();
            } else if (attr == valueAttribute) {
                v = value.ToWString();
            }
        }
        mParameters.emplace_back(std::move(n), std::move(v));
        return true;
    } else {
        return false;
    }
}

void RealtimeEffectState::HandleXMLEndTag(const std::string_view& tag)
{
    if (tag == XMLTag()) {
        if (!mParameters.empty() && LoadParameters(mMainSettings.settings)) {
            mParameters.clear();
        }
    }
}

XMLTagHandler* RealtimeEffectState::HandleXMLChild(const std::string_view& tag)
{
    if (tag == AutomationData::XMLTag) {
        return &mAutomation;
    }
    // Tag may be for the state, or the list of parameters, or for one parameter.
    // See the writing method below.  All are handled by this
    return this;
}

void RealtimeEffectState::WriteXML(XMLWriter& xmlFile)
{
    xmlFile.StartTag(XMLTag());
    const auto active = SnapshotSettings().extra.GetActive();
    xmlFile.WriteAttr(activeAttribute, active);
    xmlFile.WriteAttr(idAttribute, mID);

    const auto parameters = SnapshotParameters();
    if (!parameters.empty()) {
        xmlFile.StartTag(parametersAttribute);
        for (const auto& [name, value] : parameters) {
            xmlFile.StartTag(parameterAttribute);
            xmlFile.WriteAttr(nameAttribute, name);
            xmlFile.WriteAttr(valueAttribute, value);
            xmlFile.EndTag(parameterAttribute);
        }
        xmlFile.EndTag(parametersAttribute);
    }

    mAutomation.WriteXML(xmlFile);
    xmlFile.EndTag(XMLTag());
}

std::shared_ptr<EffectSettingsAccess> RealtimeEffectState::GetAccess()
{
    if (!GetEffect()) {
        // Effect not found!
        // Return a dummy
        return std::make_shared<Access>();
    }

    // Only the main thread assigns to the atomic pointer, here and
    // once only in the lifetime of the state
    if (!GetAccessState()) {
        mpAccessState.emplace(*mPlugin, *this);
    }

    return std::make_shared<Access>(*this);
}
