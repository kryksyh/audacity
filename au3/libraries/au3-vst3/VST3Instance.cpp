#include "VST3Instance.h"

#include <public.sdk/source/vst/utility/uid.h>

#include "au3-exceptions/AudacityException.h"
#include "VST3Wrapper.h"
#include "au3-module-manager/ConfigInterface.h"
#include "VST3Utils.h"

VST3Instance::VST3Instance(const PerTrackEffect& effect, VST3::Hosting::Module& module, const VST3::Hosting::ClassInfo& effectClassInfo)
    : Instance(effect)
{
    ReloadUserOptions();
    mWrapper = std::make_unique<VST3Wrapper>(module, effectClassInfo);
    mWrapper->InitializeComponents();
}

VST3Instance::~VST3Instance() = default;

bool VST3Instance::RealtimeLatencyUpdatePending() const noexcept
{
    if (!mUseLatency) return false;
    if (mWrapper->LatencyUpdatePending()) return true;
    return std::any_of(mProcessors.begin(), mProcessors.end(), [](const auto& processor) { return processor->mWrapper->LatencyUpdatePending(); });
}

bool VST3Instance::RefreshRealtimeLatency()
{
    bool changed = mWrapper->RefreshLatency();
    for (const auto& processor : mProcessors) changed |= processor->mWrapper->RefreshLatency();
    if (changed && mUseLatency) mLatencyRevision.fetch_add(1, std::memory_order_release);
    return changed && mUseLatency;
}

void VST3Instance::SetRealtimeProcessingContext(size_t group, const EffectProcessingContext& context)
{
    if (group == 0) mWrapper->SetProcessingContext(context);
    else if (group - 1 < mProcessors.size()) mProcessors[group - 1]->mWrapper->SetProcessingContext(context);
}

size_t VST3Instance::GetTailSize() const
{
    return Instance::GetTailSize();
}

bool VST3Instance::RealtimeAddProcessor(EffectSettings& settings,
                                        EffectOutputs*, unsigned, float sampleRate)
{
    if (!mRecruited) {
        // Assign self to the first processor
        mRecruited = true;
        return true;
    }
    // Assign another instance with independent state to other processors
    auto& effect = static_cast<const PerTrackEffect&>(mProcessor);
    auto uProcessor
        =std::make_unique<VST3Instance>(effect, mWrapper->GetModule(), mWrapper->GetEffectClassInfo());
    assert(m_audioThreadBufferSize > 0);
    if (!uProcessor->RealtimeInitialize(settings, sampleRate, m_audioThreadBufferSize)) {
        return false;
    }
    mProcessors.push_back(move(uProcessor));
    return true;
}

bool VST3Instance::RealtimeFinalize(EffectSettings& settings) noexcept
{
    return GuardedCall<bool>([&]{
        mRecruited = false;
        mWrapper->Finalize(&settings);
        for (auto& pProcessor : mProcessors) {
            pProcessor->mWrapper->Finalize(nullptr);
        }
        mProcessors.clear();
        return true;
    });
}

bool VST3Instance::RealtimeInitialize(EffectSettings& settings, double sampleRate, size_t audioThreadBufferSize)
{
    if (mWrapper->Initialize(settings, sampleRate, Steinberg::Vst::kRealtime, mProcessingBlockSize)) {
        m_audioThreadBufferSize = audioThreadBufferSize;
        return true;
    }
    return false;
}

size_t VST3Instance::RealtimeProcess(size_t group, EffectSettings& settings, const float* const* inBuf,
                                     float* const* outBuf, size_t numSamples)
{
    if (!mRecruited) {
        // unexpected!
        return 0;
    }
    if (group == 0) {
        return mWrapper->Process(inBuf, outBuf, numSamples);
    } else if (--group < mProcessors.size()) {
        return mProcessors[group]->mWrapper->Process(inBuf, outBuf, numSamples);
    }
    return 0;
}

bool VST3Instance::RealtimeProcessEnd(EffectSettings& settings) noexcept
{
    return true;
}

bool VST3Instance::RealtimeProcessStart(MessagePackage& package)
{
    auto& settings = package.settings;
    mWrapper->ProcessBlockStart(settings);
    for (auto& pProcessor : mProcessors) {
        pProcessor->mWrapper->ProcessBlockStart(settings);
    }
    return true;
}

bool VST3Instance::RealtimeResume()
{
    mWrapper->ResumeProcessing();
    for (auto& pProcessor : mProcessors) {
        pProcessor->mWrapper->ResumeProcessing();
    }
    return true;
}

bool VST3Instance::RealtimeSuspend()
{
    mWrapper->SuspendProcessing();
    for (auto& pProcessor : mProcessors) {
        pProcessor->mWrapper->SuspendProcessing();
    }
    return true;
}

auto VST3Instance::GetLatency(const EffectSettings& settings, double sampleRate)
const -> SampleCount
{
    if (mUseLatency) {
        return mWrapper->CachedLatencySamples();
    }
    return 0;
}

auto VST3Instance::GetReportedLatency(const EffectSettings&, double) const -> SampleCount
{
    return mUseLatency ? static_cast<SampleCount>(std::max<Steinberg::int32>(0, mWrapper->GetLatencySamples())) : 0;
}

bool VST3Instance::ProcessFinalize() noexcept
{
    return GuardedCall<bool>([&]
    {
        mWrapper->Finalize(nullptr);
        return true;
    });
}

bool VST3Instance::ProcessInitialize(EffectSettings& settings, double sampleRate, ChannelNames chanMap)
{
    if (mWrapper->Initialize(settings, sampleRate, Steinberg::Vst::kOffline, mProcessingBlockSize)) {
        return true;
    }
    return false;
}

size_t VST3Instance::GetBlockSize() const
{
    return mProcessingBlockSize;
}

size_t VST3Instance::SetBlockSize(size_t maxBlockSize)
{
    mProcessingBlockSize
        =static_cast<Steinberg::int32>(std::min(maxBlockSize, mUserBlockSize));
    return mProcessingBlockSize;
}

size_t VST3Instance::ProcessBlock(EffectSettings& settings, const float* const* inBlock, float* const* outBlock,
                                  size_t blockLen)
{
    return mWrapper->Process(inBlock, outBlock, blockLen);
}

VST3Wrapper& VST3Instance::GetWrapper()
{
    return *mWrapper;
}

void VST3Instance::QueueRealtimeParameterChange(Steinberg::uint32 id, double value)
{
    mWrapper->QueueParameterChange(id, value);
    for (const auto& processor : mProcessors) processor->mWrapper->QueueParameterChange(id, value);
}

unsigned VST3Instance::GetAudioInCount() const
{
    //setupProcessing should be called first
    return VST3Utils::CountChannels(
        mWrapper->mEffectComponent,
        Steinberg::Vst::kAudio,
        Steinberg::Vst::kInput,
        Steinberg::Vst::kMain
        );
}

unsigned VST3Instance::GetAudioOutCount() const
{
    //setupProcessing should be called first
    return VST3Utils::CountChannels(
        mWrapper->mEffectComponent,
        Steinberg::Vst::kAudio,
        Steinberg::Vst::kOutput,
        Steinberg::Vst::kMain
        );
}

void VST3Instance::ReloadUserOptions()
{
    // Reinitialize configuration settings
    int userBlockSize;
    GetConfig(mProcessor, PluginSettings::Shared, wxT("Options"),
              wxT("BufferSize"), userBlockSize, 8192);
    mUserBlockSize = std::max(1, userBlockSize);
    GetConfig(mProcessor, PluginSettings::Shared, wxT("Options"),
              wxT("UseLatency"), mUseLatency, true);

    SetBlockSize(mUserBlockSize);
}

Steinberg::IPtr<Steinberg::Vst::IEditController> VST3Instance::vstEditController() const
{
    return mWrapper->mEditController;
}

Steinberg::IPtr<Steinberg::Vst::IComponent> VST3Instance::effectComponent() const
{
    return mWrapper->mEffectComponent;
}
