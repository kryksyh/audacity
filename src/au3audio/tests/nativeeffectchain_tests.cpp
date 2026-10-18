/*
* Audacity: A Digital Audio Editor
*/
#include <gtest/gtest.h>
#include <algorithm>
#include <cmath>
#include "../internal/nativeeffectchain.h"
#include "../internal/nativeofflinegraph.h"
#include "au3-builtin-effects/BassTrebleBase.h"
#include "au3-realtime-effects/RealtimeEffectState.h"
#include "au3-realtime-effects/RealtimeEffectList.h"
#include "au3-xml/XMLFileReader.h"

namespace au::au3audio {
namespace {
class ToneControls final : public BassTrebleBase
{
public:
    std::shared_ptr<EffectInstance> MakeInstance() const override { return std::make_shared<Instance>(*this); }
};

class UnstableLatencyControls final : public BassTrebleBase
{
public:
    class ProbeInstance final : public BassTrebleBase::Instance
    {
    public:
        explicit ProbeInstance(const UnstableLatencyControls& owner)
            : BassTrebleBase::Instance(owner), owner(owner) {}
        bool RealtimeLatencyUpdatePending() const noexcept override { return true; }
        bool RefreshRealtimeLatency() override { ++owner.refreshes; return false; }
        bool RealtimeFinalize(EffectSettings& settings) noexcept override
        {
            ++owner.finalized;
            return BassTrebleBase::Instance::RealtimeFinalize(settings);
        }

    private:
        const UnstableLatencyControls& owner;
    };
    std::shared_ptr<EffectInstance> MakeInstance() const override
    {
        ++created;
        return std::make_shared<ProbeInstance>(*this);
    }

    mutable unsigned refreshes = 0, finalized = 0, created = 0;
};

class ResumeDelayControls final : public BassTrebleBase
{
    class Instance final : public BassTrebleBase::Instance
    {
    public:
        explicit Instance(const ResumeDelayControls& owner)
            : BassTrebleBase::Instance(owner) {}
        bool LatencyRefreshRequiresMainThread() const noexcept override { return true; }
        SampleCount GetLatency(const EffectSettings&, double) const override { return delay; }
        SampleCount GetReportedLatency(const EffectSettings&, double) const override { return delay; }
        bool RealtimeLatencyUpdatePending() const noexcept override { return pending; }
        bool RefreshRealtimeLatency() override { pending = false; delay = 9; return true; }
        bool RealtimeSuspend() override
        {
            history.fill(0);
            head = 0;
            return true;
        }

        size_t RealtimeProcess(size_t, EffectSettings&, const float* const* input, float* const* output, size_t frames) override
        {
            for (size_t frame = 0; frame < frames; ++frame) {
                history[head] = input[0][frame];
                output[0][frame] = history[(head + history.size() - delay) % history.size()];
                head = (head + 1) % history.size();
            }
            if (++calls == 2) {
                pending = true;               // Initial probe, then first actual block.
            }
            return frames;
        }

    private:
        std::array<float, 64> history {};
        size_t head = 0, delay = 3, calls = 0;
        bool pending = false;
    };
public:
    std::shared_ptr<EffectInstance> MakeInstance() const override { return std::make_shared<Instance>(*this); }
};

wxString effectXml(RealtimeEffectState& state)
{
    XMLStringWriter writer;
    state.WriteXML(writer);
    return writer;
}
}

TEST(NativeEffectChainTests, SavingAnInitializedEffectReadsPendingManualSettingsWithoutWaitingForProcessing)
{
    ToneControls effect;
    RealtimeEffectState::EffectFactory::Scope factory([&](const PluginID&) { return &effect; });
    const auto state = RealtimeEffectState::make_shared("tone-controls");
    ASSERT_TRUE(state->Automation().SetLane({ "Gain", true, { { 0.0, 0.9, 0.0 } } }));
    const auto instance = state->Initialize(48000.0, 64);
    ASSERT_NE(instance, nullptr);
    const auto finalize = finally([&] { state->Finalize(); });
    const auto access = state->GetAccess();
    access->ModifySettings([](EffectSettings& settings) {
        settings.cast<BassTrebleSettings>()->mGain = -6.0;
        settings.extra.SetActive(false);
        return nullptr;
    });
    EXPECT_DOUBLE_EQ(state->GetSettings().cast<BassTrebleSettings>()->mGain, 0.0);
    const auto generation = access->modificationCounter();
    const auto restored = RealtimeEffectState::make_shared(PluginID {});
    XMLFileReader reader;
    ASSERT_TRUE(reader.ParseString(restored.get(), effectXml(*state)));
    EXPECT_DOUBLE_EQ(restored->GetSettings().cast<BassTrebleSettings>()->mGain, -6.0);
    EXPECT_FALSE(restored->IsEnabled());
    ASSERT_NE(restored->Automation().Find("Gain"), nullptr);
    EXPECT_DOUBLE_EQ(restored->Automation().Find("Gain")->points.front().value, 0.9);
    EXPECT_EQ(access->modificationCounter(), generation);
    EXPECT_DOUBLE_EQ(state->GetSettings().cast<BassTrebleSettings>()->mGain, 0.0);
}

TEST(NativeEffectChainTests, DeepCopiesKeepPendingManualSettingsAndRemainIndependent)
{
    ToneControls effect;
    RealtimeEffectState::EffectFactory::Scope factory([&](const PluginID&) { return &effect; });
    const auto source = RealtimeEffectState::make_shared("tone-controls");
    source->GetAccess()->ModifySettings([](EffectSettings& settings) {
        settings.cast<BassTrebleSettings>()->mGain = -6.0;
        settings.extra.SetActive(false);
        return nullptr;
    });
    const auto clone = source->Clone();
    const auto assigned = RealtimeEffectState::make_shared("tone-controls");
    const auto assignedAccess = assigned->GetAccess();
    *assigned = *source;
    for (const auto& copy : { clone, assigned }) {
        EXPECT_DOUBLE_EQ(copy->GetAccess()->Get().cast<BassTrebleSettings>()->mGain, -6.0);
        EXPECT_FALSE(copy->GetAccess()->Get().extra.GetActive());
    }
    source->GetAccess()->ModifySettings([](EffectSettings& settings) {
        settings.cast<BassTrebleSettings>()->mGain = 12.0;
        return nullptr;
    });
    EXPECT_DOUBLE_EQ(clone->GetAccess()->Get().cast<BassTrebleSettings>()->mGain, -6.0);
    EXPECT_DOUBLE_EQ(assignedAccess->Get().cast<BassTrebleSettings>()->mGain, -6.0);
}

TEST(NativeEffectChainTests, MissingEffectsKeepOpaqueParametersAcrossSerializationCopiesAndLateAvailability)
{
    ToneControls effect;
    bool available = false;
    const RealtimeEffectState::EffectFactory::Scope factory([&](const PluginID&) -> const EffectInstanceFactory* {
        return available ? &effect : nullptr;
    });
    const auto state = RealtimeEffectState::make_shared(PluginID {});
    XMLFileReader reader;
    ASSERT_TRUE(reader.ParseString(state.get(), wxString::FromUTF8(
                                       "<effect id=\"missing-tone\" active=\"0\"><parameters>"
                                       "<parameter name=\"Gain\" value=\"-6\"/><parameter name=\"Bass\" value=\"15\"/>"
                                       "<parameter name=\"Opaque\" value=\"é &amp; &quot;quoted&quot; = C:\\presets\\a\"/>"
                                       "</parameters><automation><lane parameter=\"Gain\" enabled=\"1\">"
                                       "<point time=\"0\" value=\"0.9\" curve=\"0\"/></lane></automation></effect>")));
    const auto assigned = RealtimeEffectState::make_shared("missing-tone");
    *assigned = *state;
    for (const auto& copy : { state, state->Clone(), assigned }) {
        const auto xml = effectXml(*copy);
        EXPECT_TRUE(xml.Contains("name=\"Opaque\""));
        EXPECT_TRUE(xml.Contains("é &amp; &quot;quoted&quot; = C:\\presets\\a"));
        const auto again = RealtimeEffectState::make_shared(PluginID {});
        XMLFileReader copyReader;
        ASSERT_TRUE(copyReader.ParseString(again.get(), xml));
        EXPECT_EQ(effectXml(*again), xml);
        EXPECT_FALSE(again->IsEnabled());
        ASSERT_NE(again->Automation().Find("Gain"), nullptr);
    }
    // Bypass must remain editable while the plugin is absent, so the project
    // can still render without destroying the missing processor's settings.
    state->SetActive(true);
    EXPECT_TRUE(state->IsEnabled());
    RealtimeEffectList effects;
    XMLFileReader listReader;
    ASSERT_TRUE(listReader.ParseString(&effects, "<effects active=\"1\">" + effectXml(*state) + "</effects>"));
    EXPECT_THROW(NativeEffectChain::specifications(effects), std::runtime_error);
    effects.GetStateAt(0)->SetActive(false);
    EXPECT_TRUE(NativeEffectChain::specifications(effects).empty());
    state->SetActive(false);
    EXPECT_FALSE(state->IsEnabled());
    available = true;
    ASSERT_NE(state->GetEffect(), nullptr);
    EXPECT_DOUBLE_EQ(state->GetSettings().cast<BassTrebleSettings>()->mGain, -6.0);
    EXPECT_DOUBLE_EQ(state->GetSettings().cast<BassTrebleSettings>()->mBass, 15.0);
    EXPECT_FALSE(state->IsEnabled());
}

TEST(NativeEffectChainTests, AnEffectWhoseDelayNeverSettlesFailsAndFinalizesInsteadOfHangingTheRender)
{
    UnstableLatencyControls effect;
    EXPECT_THROW((NativeEffectChain { { { &effect, effect.MakeSettings(), {} } }, 48000.0, 16 }), std::runtime_error);
    EXPECT_GT(effect.refreshes, 0u);
    EXPECT_LT(effect.refreshes, 100u);
    EXPECT_GT(effect.created, 0u);
    EXPECT_EQ(effect.finalized, effect.created);
}

TEST(NativeEffectChainTests, PullKeepsCompletedAndUpcomingDelaySeparateWhenARefreshChangesTheNextBlock)
{
    ResumeDelayControls effect;
    NativeEffectChain chain({ { &effect, effect.MakeSettings(), {} } }, 48000.0, 16);
    int64_t frontier = 0;
    const NativeRoutingGraph::Processor input = [&](auto output, size_t frames, int64_t sample) {
        EXPECT_EQ(sample, frontier);
        for (size_t frame = 0; frame < frames; ++frame) {
            output[0][frame] = output[1][frame] = static_cast<float>(sample + frame + 1);
        }
        frontier += frames;
    };
    std::array<float, 64> left {}, right {};
    chain.pull({ left.data(), right.data() }, left.size(), 0, input);
    for (size_t frame = 0; frame < left.size(); ++frame) {
        EXPECT_FLOAT_EQ(left[frame], frame + 1) << frame;
        EXPECT_FLOAT_EQ(right[frame], frame + 1) << frame;
    }
    EXPECT_EQ(frontier, 80);
}

TEST(NativeOfflineGraphTests, SharedSourceIsProcessedOnceAcrossPreAndPostFaderRoutesWithChangingEffectDelay)
{
    ResumeDelayControls delayed;
    ToneControls tone;
    NativeEffectChain mainEffects({ { &delayed, delayed.MakeSettings(), {} } }, 48000.0, 16);
    NativeEffectChain sideEffects({ { &tone, tone.MakeSettings(), {} } }, 48000.0, 16);
    NativeOfflineGraph::Node source, main, side;
    int64_t frontier = 0;
    size_t requests = 0;
    source.source = [&](auto output, size_t frames, int64_t sample) {
        EXPECT_EQ(sample, frontier);
        EXPECT_LE(frames, 7u);
        ++requests;
        for (size_t frame = 0; frame < frames; ++frame) {
            output[0][frame] = static_cast<float>(sample + frame + 1);
            output[1][frame] = -output[0][frame];
        }
        frontier += frames;
    };
    source.routing.output = "main";
    source.routing.sends.push_back({ "side", 0.25, true });
    source.fader = [](auto left, auto right, int64_t) {
        std::fill(left.begin(), left.end(), 0.5f);
        std::fill(right.begin(), right.end(), 0.25f);
    };
    main.routing.busId = "main";
    main.effects = [&](auto output, size_t frames, int64_t sample, const auto& upstream) {
        mainEffects.pull(output, frames, sample, upstream);
    };
    side.routing.busId = "side";
    side.effects = [&](auto output, size_t frames, int64_t sample, const auto& upstream) {
        sideEffects.pull(output, frames, sample, upstream);
    };
    NativeOfflineGraph graph({ std::move(source), std::move(main), std::move(side) }, 7, 0);
    std::array<float, 2048> left {}, right {};
    for (size_t offset = 0; offset < left.size();) {
        const auto count = std::min<size_t>(73, left.size() - offset);
        graph.process({ left.data() + offset, right.data() + offset }, count, offset);
        offset += count;
    }
    for (size_t frame = 0; frame < left.size(); ++frame) {
        EXPECT_FLOAT_EQ(left[frame], 0.75f * (frame + 1)) << frame;
        EXPECT_FLOAT_EQ(right[frame], -0.5f * (frame + 1)) << frame;
    }
    EXPECT_GE(frontier, static_cast<int64_t>(left.size()));
    EXPECT_LE(frontier, static_cast<int64_t>(left.size() + 32));
    EXPECT_GT(requests, 0u);
}

TEST(NativeOfflineGraphTests, FeedbackRoutesAreRejectedBeforeAnySourceIsProcessed)
{
    NativeOfflineGraph::Node first, second;
    unsigned calls = 0;
    first.source = [&](auto, size_t, int64_t) { ++calls; };
    first.routing.busId = "first";
    first.routing.output = "second";
    second.routing.busId = "second";
    second.routing.sends.push_back({ "first", 0.5, false });
    EXPECT_THROW((NativeOfflineGraph { { first, second }, 7, 0 }), std::invalid_argument);
    EXPECT_EQ(calls, 0u);
}

TEST(NativeEffectChainTests, BuiltInEffectsProcessStereoAndKeepTheirManualSettings)
{
    ToneControls effect;
    auto settings = effect.MakeSettings();
    settings.cast<BassTrebleSettings>()->mGain = -6.0;
    NativeEffectChain chain({ { &effect, settings, {} } }, 48000.0, 16);
    EXPECT_EQ(chain.latency(), 15u);
    std::vector<float> left(64, 1.0f), right(64, 0.5f);
    chain.process({ left.data(), right.data() }, left.size(), 0);
    for (size_t i = 0; i < left.size(); ++i) {
        EXPECT_NEAR(left[i], i < 15 ? 0.0 : std::pow(10.0, -6.0 / 20.0), 1e-6);
        EXPECT_NEAR(right[i], left[i] * 0.5f, 1e-6);
    }
    EXPECT_DOUBLE_EQ(settings.cast<BassTrebleSettings>()->mGain, -6.0);
}

TEST(NativeEffectChainTests, BuiltInAutomationUpdatesProcessingSettingsAtTheInputTime)
{
    ToneControls effect;
    AutomationData automation;
    ASSERT_TRUE(automation.SetLane({ "Gain", true, { { 0.0, 0.4, 0.0 }, { 16.0 / 48000.0, 0.5, 0.0 } } }));
    NativeEffectChain chain({ { &effect, effect.MakeSettings(), automation } }, 48000.0, 16);
    std::vector<float> left(64, 1.0f), right(64, 1.0f);
    chain.process({ left.data(), right.data() }, left.size(), 0);
    // -6 dB at the first block, then 0 dB. The streaming delay is compensated by the graph/renderer.
    for (size_t i = 0; i < left.size(); ++i) {
        const double expected = i < 15 ? 0.0 : i < 31 ? std::pow(10.0, -6.0 / 20.0) : 1.0;
        EXPECT_NEAR(left[i], expected, 1e-6) << i;
    }
    EXPECT_EQ(left, right);
}

TEST(NativeEffectChainTests, SerialEffectsKeepTheirDelaysAndAutomationInTheSameTimeDomain)
{
    ToneControls effect;
    AutomationData automation;
    ASSERT_TRUE(automation.SetLane({ "Gain", true, { { 0.0, 0.4, 0.0 }, { 16.0 / 48000.0, 0.5, 0.0 } } }));
    NativeEffectChain chain({ { &effect, effect.MakeSettings(), {} }, { &effect, effect.MakeSettings(), automation } }, 48000.0, 16);
    ASSERT_EQ(chain.latency(), 30u);
    std::vector<float> left(64, 1.0f), right(64, 1.0f);
    chain.process({ left.data(), right.data() }, left.size(), 0);
    for (size_t i = 0; i < left.size(); ++i) {
        const double expected = i < 30 ? 0.0 : i < 46 ? std::pow(10.0, -6.0 / 20.0) : 1.0;
        EXPECT_NEAR(left[i], expected, 1e-6) << i;
    }
}

TEST(NativeEffectChainTests, PartialRequestsPreserveSamplesAndEmptyChainsPassThrough)
{
    ToneControls effect;
    NativeEffectChain chain({ { &effect, effect.MakeSettings(), {} } }, 48000.0, 16);
    std::vector<float> result;
    int64_t sample = 0;
    for (size_t count : { 5u, 7u, 3u, 21u }) {
        std::vector<float> left(count, 0.25f), right(count, 0.25f);
        chain.process({ left.data(), right.data() }, count, sample);
        result.insert(result.end(), left.begin(), left.end());
        sample += count;
    }
    for (size_t i = 0; i < result.size(); ++i) {
        EXPECT_FLOAT_EQ(result[i], i < 15 ? 0.0f : 0.25f);
    }
    NativeEffectChain empty({}, 48000.0, 16);
    EXPECT_EQ(empty.latency(), 0u);
    std::vector<float> data(16, 0.75f);
    empty.process({ data.data(), data.data() }, data.size(), 0);
    EXPECT_TRUE(std::all_of(data.begin(), data.end(), [](float value) { return value == 0.75f; }));
}
}
