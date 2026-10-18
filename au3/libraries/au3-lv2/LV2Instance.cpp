/**********************************************************************

  Audacity: A Digital Audio Editor

  @file LV2Instance.cpp

  Paul Licameli split from LV2Effect.cpp

  Audacity(R) is copyright (c) 1999-2008 Audacity Team.
  License: GPL v2 or later.  See License.txt.

**********************************************************************/

#if defined(__GNUC__)
#pragma GCC diagnostic ignored "-Wparentheses"
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#elif defined(__clang__)
#pragma clang diagnostic ignored "-Wparentheses"
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
#endif

#include "LV2Instance.h"
#include "LV2Wrapper.h"
#include <lv2/atom/util.h>
#include <bit>
#include <cmath>
#include <limits>
#include <cstring>
#include <stdexcept>
#include "au3-exceptions/AudacityException.h"

// Keep the plugin's run() contract independent of disk/device slice sizes.
// Allocated on main; sample processing only copies into bounded storage.
struct LV2Instance::FixedProcessor {
    FixedProcessor(const LV2Ports& ports, const LV2EffectSettings& initial, size_t size)
        : settings(initial), input(ports.mAudioIn, std::vector<float>(size)),
          output(ports.mAudioOut, std::vector<float>(size)), size(size), read(size)
    {
        for (const auto& port : ports.mAtomPorts) {
            atoms.emplace_back((port->mMinimumSize + sizeof(uint64_t) - 1) / sizeof(uint64_t));
        }
        resetAtoms(ports);
    }
    void resetAtoms(const LV2Ports& ports)
    {
        for (size_t i = 0; i < atoms.size(); ++i) {
            auto* atom = reinterpret_cast<LV2_Atom*>(atoms[i].data());
            if (ports.mAtomPorts[i]->mIsInput) {
                *reinterpret_cast<LV2_Atom_Sequence*>(atom) = {
                    { sizeof(LV2_Atom_Sequence_Body), LV2Symbols::urid_Sequence }, { 0, 0 } };
            } else {
                *atom = { static_cast<uint32_t>(ports.mAtomPorts[i]->mMinimumSize - sizeof(LV2_Atom)), LV2Symbols::urid_Chunk };
            }
        }
    }
    void reset(const LV2Ports& ports)
    {
        pending = 0;
        read = size;
        for (auto& channel : input) std::fill(channel.begin(), channel.end(), 0.0f);
        for (auto& channel : output) std::fill(channel.begin(), channel.end(), 0.0f);
        resetAtoms(ports);
    }
    void appendAtoms(const LV2PortStates& source)
    {
        for (size_t i = 0; i < atoms.size(); ++i) {
            const auto& port = source.mAtomPortStates[i];
            if (!port->mpPort->mIsInput) continue;
            const auto* sequence = reinterpret_cast<const LV2_Atom_Sequence*>(port->mBuffer.get());
            auto* target = reinterpret_cast<LV2_Atom_Sequence*>(atoms[i].data());
            LV2_ATOM_SEQUENCE_FOREACH(sequence, event) {
                auto* added = lv2_atom_sequence_append_event(target, port->mpPort->mMinimumSize - sizeof(LV2_Atom), event);
                if (added) added->time.frames += pending;
            }
        }
    }
    LV2EffectSettings settings;
    std::vector<std::vector<float>> input, output;
    std::vector<std::vector<uint64_t>> atoms;
    size_t size, pending = 0, read;
};

LV2Instance::LV2Instance(
    const PerTrackEffect& effect, const LV2FeaturesList& features,
    const LV2Ports& ports)
    : PerTrackEffect::Instance{effect}
    , mFeatures{features}
    , mPorts{ports}
{
    LV2Preferences::GetUseLatency(effect, mUseLatency);

    int userBlockSize;
    LV2Preferences::GetBufferSize(effect, userBlockSize);
    mUserBlockSize = std::max(1, userBlockSize);
    lv2_atom_forge_init(&mForge, mFeatures.Base().URIDMapFeature());
}

LV2Instance::~LV2Instance() = default;

void LV2Instance::MakeMaster(const EffectSettings& settings,
                             double sampleRate)
{
    // Come here only when doing non-realtime application of the effect, in which
    // case, we don't care about capturing the output ports
    SetBlockSize(mUserBlockSize);
    if (mMaster && sampleRate == mFeatures.mSampleRate && mMasterBlockSize == GetBlockSize()) {
        // Already made but be sure to connect control ports to the right place
        mMaster->ConnectControlPorts(mPorts, GetSettings(settings), nullptr);
        return;
    }
    mMaster = MakeWrapper(settings, sampleRate, nullptr);
    mMasterBlockSize = GetBlockSize();
}

std::unique_ptr<LV2Wrapper>
LV2Instance::MakeWrapper(const EffectSettings& settings,
                         double sampleRate, EffectOutputs* pOutputs)
{
    return LV2Wrapper::Create(mFeatures, mPorts, mPortStates,
                              GetSettings(settings), sampleRate, pOutputs);
}

size_t LV2Instance::SetBlockSize(size_t maxBlockSize)
{
    // An already instantiated fixed-block processor cannot change its run size.
    if (mFeatures.mFixedBlockLength && !mSlaves.empty()) return GetBlockSize();
    auto size = std::max(mFeatures.mMinBlockSize,
                         std::min({ maxBlockSize, mUserBlockSize, mFeatures.mMaxBlockSize }));
    if (mFeatures.mPowerOf2BlockLength) {
        size = std::bit_floor(size);
        if (size < mFeatures.mMinBlockSize) size = std::bit_ceil(mFeatures.mMinBlockSize);
        if (size > mFeatures.mMaxBlockSize) throw std::runtime_error("The LV2 block-size restrictions are inconsistent.");
    }
    mFeatures.mBlockSize = size;
    mFeatures.UpdateBlockSizeOptions();
    if (mMaster) {
        mMaster->SendBlockSize();
    }
    for (auto& pSlave : mSlaves) {
        pSlave->SendBlockSize();
    }
    return GetBlockSize();
}

size_t LV2Instance::GetBlockSize() const
{
    return mFeatures.mBlockSize;
}

unsigned LV2Instance::GetAudioInCount() const
{
    return mPorts.mAudioIn;
}

unsigned LV2Instance::GetAudioOutCount() const
{
    return mPorts.mAudioOut;
}

auto LV2Instance::GetLatency(const EffectSettings&, double) const
-> SampleCount
{
    const auto adapter = mFixedProcessors.empty() ? 0 : GetBlockSize() - 1;
    if (mUseLatency && mPorts.mLatencyPort >= 0) {
        if (!mSlaves.empty()) return adapter + mReportedLatency.load(std::memory_order_acquire);
        const auto* wrapper = mMaster.get();
        if (wrapper) return adapter + std::max(0.0f, wrapper->GetLatency());
    }
    return adapter;
}

// Start of destructive processing path
bool LV2Instance::ProcessInitialize(EffectSettings& settings,
                                    double sampleRate, ChannelNames chanMap)
{
    MakeMaster(settings, sampleRate);
    if (!mMaster) {
        return false;
    }
    for (auto& state : mPortStates.mCVPortStates) {
        state.mBuffer.reinit(GetBlockSize(), state.mpPort->mIsInput);
    }
    if (mFeatures.mFixedBlockLength) {
        mMasterInput.assign(mPorts.mAudioIn, std::vector<float>(GetBlockSize()));
        mMasterOutput.assign(mPorts.mAudioOut, std::vector<float>(GetBlockSize()));
    }
    mMaster->Activate();
    return true;
}

size_t LV2Instance::ProcessBlock(EffectSettings&,
                                 const float* const* inbuf, float* const* outbuf, size_t size)
{
    using namespace LV2Symbols;
    if (size > GetBlockSize()) {
        return 0;
    }
    assert(mMaster); // else ProcessInitialize() returned false, I'm not called
    const auto instance = &mMaster->GetInstance();

    const bool pad = mFeatures.mFixedBlockLength && size != GetBlockSize();
    size_t i = 0, o = 0;
    for (const auto& port : mPorts.mAudioPorts) {
        float* buffer;
        if (port->mIsInput) {
            buffer = const_cast<float*>(inbuf[i]);
            if (pad) {
                buffer = mMasterInput[i].data();
                std::copy_n(inbuf[i], size, buffer);
                std::fill(buffer + size, buffer + GetBlockSize(), 0.0f);
            }
            ++i;
        } else {
            buffer = pad ? mMasterOutput[o].data() : outbuf[o];
            ++o;
        }
        lilv_instance_connect_port(instance, port->mIndex, buffer);
    }

    for (auto& state : mPortStates.mAtomPortStates) {
        state->SendToInstance(mForge, mPositionFrame, mPositionSpeed);
    }

    // Only the final destructive block may be short. Pad it to retain the
    // fixed run size, then trim output without adding a streaming delay.
    lilv_instance_run(instance, pad ? GetBlockSize() : size);
    if (pad) for (size_t channel = 0; channel < mPorts.mAudioOut; ++channel) {
        std::copy_n(mMasterOutput[channel].data(), size, outbuf[channel]);
    }

    // Main thread consumes responses
    mMaster->ConsumeResponses();

    for (auto& state : mPortStates.mAtomPortStates) {
        state->ResetForInstanceOutput();
    }

    return size;
}

bool LV2Instance::RealtimeInitialize(EffectSettings&, double, size_t)
{
    mTransport.reset();
    for (auto& state : mPortStates.mCVPortStates) {
        state.mBuffer.reinit(GetBlockSize(), state.mpPort->mIsInput);
    }
    return true;
}

bool LV2Instance::RealtimeFinalize(EffectSettings&) noexcept
{
    return GuardedCall<bool>([&]{
        mSlaves.clear();
        mFixedProcessors.clear();
        for (auto& state : mPortStates.mCVPortStates) {
            state.mBuffer.reset();
        }
        return true;
    });
}

bool LV2Instance::RealtimeAddProcessor(EffectSettings& settings,
                                       EffectOutputs* pOutputs, unsigned, float sampleRate)
{
    // Connect to outputs only if this is the first processor for the track.
    // (What's right when a mono effect is on a stereo channel?  Unclear, but
    // this definitely causes connection with the first channel.)
    auto pInstance = LV2Wrapper::Create(mFeatures,
                                        mPorts, mPortStates, GetSettings(settings), sampleRate,
                                        mSlaves.empty() ? static_cast<LV2EffectOutputs*>(pOutputs) : nullptr);
    if (!pInstance) {
        return false;
    }
    if (mFeatures.mFixedBlockLength) {
        auto buffer = std::make_unique<FixedProcessor>(mPorts, GetSettings(settings), GetBlockSize());
        pInstance->ConnectControlPorts(mPorts, buffer->settings, mSlaves.empty() ? pOutputs : nullptr);
        for (size_t i = 0; i < mPorts.mAtomPorts.size(); ++i) {
            lilv_instance_connect_port(&pInstance->GetInstance(), mPorts.mAtomPorts[i]->mIndex, buffer->atoms[i].data());
        }
        mFixedProcessors.push_back(std::move(buffer));
    }
    pInstance->Activate();
    mSlaves.push_back(move(pInstance));
    return true;
}

bool LV2Instance::RealtimeSuspend()
{
    if (mMaster) {
        mMaster->Deactivate();
    }
    for (auto& pSlave : mSlaves) {
        pSlave->Deactivate();
    }

    for (auto& buffer : mFixedProcessors) buffer->reset(mPorts);
    mPositionSpeed = 0.0;
    mPositionFrame = 0;
    mRolling = false;

    return true;
}

bool LV2Instance::RealtimeResume()
{
    if (mMaster) {
        mMaster->Activate();
    }
    for (auto& pSlave : mSlaves) {
        pSlave->Activate();
    }

    mPositionSpeed = 1.0;
    mPositionFrame = 0;
    mRolling = true;

    return true;
}

bool LV2Instance::RealtimeProcessStart(MessagePackage&)
{
    mNumSamples = 0;
    for (auto& state : mPortStates.mAtomPortStates) {
        state->SendToInstance(mForge, mPositionFrame, mPositionSpeed);
    }
    return true;
}

void LV2Instance::SetRealtimeProcessingContext(size_t, const EffectProcessingContext& context)
{
    mTransport = context;
}

size_t LV2Instance::RealtimeProcess(size_t group, EffectSettings& settings,
                                    const float* const* inbuf, float* const* outbuf, size_t numSamples)
{
    if (group >= mSlaves.size()) {
        return 0;
    }
    assert(numSamples <= (size_t)GetBlockSize());
    if (mTransport) for (auto& state : mPortStates.mAtomPortStates) state->UpdatePosition(mForge, *mTransport);
    if (!mFixedProcessors.empty()) mFixedProcessors[group]->appendAtoms(mPortStates);

    const auto slave = mSlaves[group].get();
    const auto instance = &slave->GetInstance();

    const auto run = [&](const float* const* inputs, float* const* outputs, size_t count) {
        size_t i = 0, o = 0;
        for (const auto& port : mPorts.mAudioPorts) {
            lilv_instance_connect_port(instance, port->mIndex,
                const_cast<float*>(port->mIsInput ? inputs[i++] : outputs[o++]));
        }
        lilv_instance_run(instance, count);
    };
    mNumSamples = std::max(numSamples, mNumSamples);
    if (!mRolling) {
        for (size_t channel = 0; channel < mPorts.mAudioOut; ++channel) {
            if (mPorts.mAudioIn) std::copy_n(inbuf[std::min<size_t>(channel, mPorts.mAudioIn - 1)], numSamples, outbuf[channel]);
            else std::fill_n(outbuf[channel], numSamples, 0.0f);
        }
    } else if (mFixedProcessors.empty()) {
        run(inbuf, outbuf, numSamples);
    } else {
        auto& buffer = *mFixedProcessors[group];
        // Input controls belong to the start of the buffered input block.
        // A later GUI/automation update must not change its earlier samples.
        for (size_t frame = 0; frame < numSamples; ++frame) {
            if (!buffer.pending) {
                const auto& values = GetSettings(settings).values;
                for (size_t i = 0; i < values.size(); ++i) {
                    if (mPorts.mControlPorts[i]->mIsInput) buffer.settings.values[i] = values[i];
                }
            }
            for (size_t channel = 0; channel < mPorts.mAudioIn; ++channel) buffer.input[channel][buffer.pending] = inbuf[channel][frame];
            if (++buffer.pending == buffer.size) {
                size_t i = 0, o = 0;
                for (const auto& port : mPorts.mAudioPorts) {
                    lilv_instance_connect_port(instance, port->mIndex,
                        port->mIsInput ? buffer.input[i++].data() : buffer.output[o++].data());
                }
                lilv_instance_run(instance, buffer.size);
                for (size_t index = 0; index < mPorts.mAtomPorts.size(); ++index) {
                    const auto& port = mPortStates.mAtomPortStates[index];
                    if (!port->mpPort->mIsInput) {
                        const auto* atom = reinterpret_cast<const LV2_Atom*>(buffer.atoms[index].data());
                        if (atom->type == LV2Symbols::urid_Sequence && atom->size >= sizeof(LV2_Atom_Sequence_Body)
                            && sizeof(LV2_Atom) + atom->size <= port->mpPort->mMinimumSize) {
                            std::memcpy(port->mBuffer.get(), atom, sizeof(LV2_Atom) + atom->size);
                            port->ReceiveFromInstance();
                        }
                    }
                }
                buffer.resetAtoms(mPorts);
                buffer.pending = 0;
                buffer.read = 0;
            }
            for (size_t channel = 0; channel < mPorts.mAudioOut; ++channel) {
                outbuf[channel][frame] = buffer.read < buffer.size ? buffer.output[channel][buffer.read] : 0.0f;
            }
            ++buffer.read;
        }
    }

    // Background thread consumes responses from yet another worker thread
    slave->ConsumeResponses();

    for (auto& state : mPortStates.mAtomPortStates) {
        state->ResetForInstanceOutput();
    }

    if (group == 0) {
        mPositionFrame += numSamples;
        if (mUseLatency && mPorts.mLatencyPort >= 0) {
            const double reported = slave->GetLatency();
            // Keep invalid values out of unsigned sample counts.
            const auto latency = std::isfinite(reported) && reported > 0.0
                && reported < static_cast<double>(std::numeric_limits<int64_t>::max()) ? static_cast<SampleCount>(reported) : 0;
            if (mReportedLatency.exchange(latency, std::memory_order_acq_rel) != latency) {
                mLatencyRevision.fetch_add(1, std::memory_order_release);
            }
        }
    }

    return numSamples;
}

bool LV2Instance::RealtimeProcessEnd(EffectSettings&) noexcept
{
    return GuardedCall<bool>([&]{
        // Nothing to do if we did process any samples
        if (mNumSamples == 0) {
            return true;
        }

        // Why is this not also done on the destructive processing path?
        // Because it is soon dimissing the modal dialog anyway.
        if (mFixedProcessors.empty()) for (auto& state : mPortStates.mAtomPortStates) {
            state->ReceiveFromInstance();
        }

        mNumSamples = 0;

        return true;
    });
}
