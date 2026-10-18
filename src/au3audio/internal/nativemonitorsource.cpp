/*
* Audacity: A Digital Audio Editor
*/
#include "nativemonitorsource.h"
#include "liveinstrumentsession.h"
#include "midiplayablesequence.h"
#include "nativeplaybackprocessor.h"
#include <array>
#include <cmath>
#include <limits>
#include <stdexcept>
#include "au3-playable-track/TrackRouting.h"
#include "au3-playable-track/TrackAutomation.h"
#include "au3-numeric-formats/ProjectTimeSignature.h"
#include "au3-realtime-effects/RealtimeEffectManager.h"
#include "au3-stretching-sequence/StretchingSequence.h"
#include "au3-track/Track.h"
#include "au3-wave-track/WaveTrack.h"

namespace au::au3audio {
struct NativeMonitorSource::Impl {
    static constexpr size_t capacity = 512;
    Impl(std::shared_ptr<AudacityProject> project, std::shared_ptr<MidiTrack> track,
         std::shared_ptr<LiveInstrumentSession> session, double position, std::weak_ptr<IMeterSender> meter)
        : project(std::move(project)), track(std::move(track)), session(std::move(session)), position(position), meter(std::move(meter))
    {
        if (!std::isfinite(position)) {
            throw std::invalid_argument("Invalid monitoring cursor position.");
        }
        times.fill(position);
    }

    void prepare(double sampleRate)
    {
        stop();
        if (!std::isfinite(sampleRate) || sampleRate <= 0.0
            || std::abs(position) >= static_cast<long double>(std::numeric_limits<int64_t>::max()) / sampleRate) {
            throw std::invalid_argument("Invalid native monitoring sample rate or position.");
        }
        rate = sampleRate;
        // This sequence supplies group/channel metadata only. Monitoring pulls
        // raw synth samples directly; it never plays the document's clips.
        sequences.push_back(std::make_shared<MidiPlayableSequence>(track, session, rate, false, false));
        ids.push_back({ static_cast<int64_t>(track->GetId()), 2 });
        for (const auto* bus : TrackList::Get(*project).Any<const WaveTrack>()) {
            if (!TrackRouting::Get(*bus).busId.empty()) {
                sequences.push_back(StretchingSequence::Create(*bus, bus->GetClipInterfaces()));
                ids.push_back({ static_cast<int64_t>(bus->GetId()), bus->NChannels() });
            }
        }
        inputs = { synth[0].data(), synth[1].data() };
        for (size_t index = 1; index < sequences.size(); ++index) {
            for (size_t channel = 0; channel < sequences[index]->NChannels(); ++channel) {
                inputs.push_back(zero.data());
            }
        }
        initialization = std::make_unique<RealtimeEffects::InitializationScope>(project, rate, 2, capacity);
        for (size_t index = 0; index < sequences.size(); ++index) {
            // The instrument and every bus are stereo processing groups.
            initialization->AddGroup(*sequences[index]->FindChannelGroup(), 2, rate, capacity);
        }
        rebuild();
    }

    void rebuild()
    {
        available.store(false, std::memory_order_release);
        snapshot(position);
        graph = std::make_unique<NativePlaybackProcessor>(*project, sequences, rate, 2, capacity, false);
        for (auto& channel : synth) {
            channel.fill(0.0f);
        }
        RealtimeEffects::ProcessingScope processing(*initialization, project);
        // Compile/probe and allocate delays on main, before publishing this
        // source to the device. Retain actual FX latency in live monitoring.
        graph->Process(processing, inputs, times);
        graph->SetRealtimeMode();
        available.store(true, std::memory_order_release);
    }

    void snapshot(double newPosition)
    {
        if (!std::isfinite(newPosition)
            || std::abs(newPosition) >= static_cast<long double>(std::numeric_limits<int64_t>::max()) / rate) {
            throw std::invalid_argument("Invalid native monitoring cursor position.");
        }
        position = newPosition;
        sample = static_cast<int64_t>(std::llround(position * rate));
        times.fill(position);
        const auto& signature = ProjectTimeSignature::Get(*project);
        tempo = signature.GetTempo();
        numerator = signature.GetUpperTimeSignature();
        denominator = signature.GetLowerTimeSignature();
        for (const auto& [id, value] : parameters) {
            session->restoreManualParameter(id);
        }
        automation = TrackAutomation::Get(*track);
        parameters.clear();
        constexpr std::string_view prefix = "instrument/";
        for (const auto& lane : automation.Lanes()) {
            if (lane.enabled && !lane.points.empty() && lane.parameter.starts_with(prefix)) {
                parameters.emplace_back(lane.parameter.substr(prefix.size()), AutomationData::ValueAt(&lane, position, 0.0));
            }
        }
    }

    bool needsRefresh(double newPosition) const
    {
        const auto& signature = ProjectTimeSignature::Get(*project);
        return graph && (session->latencyUpdatePending() || graph->NeedsRebuild() || position != newPosition || tempo != signature.GetTempo()
                         || numerator != signature.GetUpperTimeSignature() || denominator != signature.GetLowerTimeSignature()
                         || automation.Lanes() != TrackAutomation::Get(*track).Lanes());
    }

    void refresh(double newPosition)
    {
        available.store(false, std::memory_order_release);
        // Main thread, with the callback detached. Monitoring has one causal
        // instrument source, so its intrinsic delay is retained rather than
        // compensated against a timeline. Reactivation chases voices on the
        // next DSP call without consuming queued input here.
        for (unsigned attempt = 0; attempt < 8 && session->latencyUpdatePending(); ++attempt) {
            session->refreshLatency();
        }
        if (session->latencyUpdatePending()) {
            throw std::runtime_error("The instrument latency did not stabilize.");
        }
        const bool recreate = !graph || graph->NeedsRebuild() || automation.Lanes() != TrackAutomation::Get(*track).Lanes();
        snapshot(newPosition);
        if (recreate) {
            rebuild();
        } else {
            // Cursor/tempo edits preserve sounding voices, effect delay lines
            // and the PortAudio stream. Only immutable transport data changes.
            graph->SetTransportContext(tempo, numerator, denominator);
            available.store(true, std::memory_order_release);
        }
    }

    void stop() noexcept
    {
        available.store(false, std::memory_order_release);
        for (const auto& [id, value] : parameters) {
            session->restoreManualParameter(id);
        }
        parameters.clear();
        graph.reset();
        initialization.reset();
        sequences.clear();
        inputs.clear();
        ids.clear();
    }

    void render(float* output, size_t channels, size_t frames) noexcept
    {
        std::fill_n(output, channels * frames, 0.0f);
        if (!available.load(std::memory_order_acquire) || !session->active() || session->inTimeline()) {
            return;
        }
        try {
            const auto sender = meter.lock();
            const auto latency = AudioIO::Get() ? AudioIO::Get()->GetHardwarePlaybackLatencyMs() / 1000.0 : 0.0;
            const auto dac = std::chrono::steady_clock::now() + std::chrono::duration_cast<TimePoint::duration>(std::chrono::duration<double>(
                                                                                                                    latency));
            for (size_t offset = 0; offset < frames;) {
                const auto count = std::min(capacity, frames - offset);
                float* synthOutput[] { synth[0].data(), synth[1].data() };
                for (const auto& [id, value] : parameters) {
                    session->setAutomatedParameter(id, value);
                }
                InstrumentProcessContext context { sample, tempo, numerator, denominator, false };
                session->process(synthOutput, 2, count, {}, context);
                RealtimeEffects::ProcessingScope processing(*initialization, project);
                graph->Process(processing, inputs, { times.data(), count });
                for (size_t frame = 0; frame < count; ++frame) {
                    for (size_t channel = 0; channel < channels; ++channel) {
                        output[(offset + frame) * channels + channel] = channels == 1
                                                                        ? 0.5f * (graph->Output(0)[frame] + graph->Output(1)[frame])
                                                                        : graph->Output(std::min<size_t>(channel, 1))[frame];
                    }
                }
                if (sender) {
                    const auto time = dac + std::chrono::duration_cast<TimePoint::duration>(std::chrono::duration<double>(offset / rate));
                    for (size_t index = 0; index < ids.size(); ++index) {
                        for (size_t channel = 0; channel < std::min<size_t>(2, ids[index].second); ++channel) {
                            sender->push(channel, { graph->Meter(index,
                                                                 channel), count, 1, time }, IMeterSender::TrackId { ids[index].first });
                        }
                    }
                }
                offset += count;
            }
        } catch (...) {
            available.store(false, std::memory_order_release);
            std::fill_n(output, channels * frames, 0.0f);
        }
    }

    std::shared_ptr<AudacityProject> project;
    std::shared_ptr<MidiTrack> track;
    std::shared_ptr<LiveInstrumentSession> session;
    double position, rate = 48000.0, tempo = 120.0;
    int64_t sample = 0;
    int numerator = 4, denominator = 4;
    std::weak_ptr<IMeterSender> meter;
    AudioPlaybackProcessor::Sequences sequences;
    std::vector<std::pair<int64_t, size_t> > ids;
    std::array<std::array<float, capacity>, 2> synth {};
    std::array<float, capacity> zero {};
    std::array<double, capacity> times {};
    std::vector<const float*> inputs;
    std::unique_ptr<NativePlaybackProcessor> graph;
    std::unique_ptr<RealtimeEffects::InitializationScope> initialization;
    std::atomic<bool> available { false };
    AutomationData automation;
    std::vector<std::pair<std::string, double> > parameters;
};

NativeMonitorSource::NativeMonitorSource(std::shared_ptr<AudacityProject> project, std::shared_ptr<MidiTrack> track,
                                         std::shared_ptr<LiveInstrumentSession> session, double position, std::weak_ptr<IMeterSender> meter)
    : m_impl(std::make_unique<Impl>(std::move(project), std::move(track), std::move(session), position, std::move(meter))) {}
NativeMonitorSource::~NativeMonitorSource() { m_impl->stop(); }
void NativeMonitorSource::Prepare(double rate) { m_impl->prepare(rate); }
void NativeMonitorSource::Render(float* output, size_t channels, size_t frames) noexcept { m_impl->render(output, channels, frames); }
RealtimeEffects::InitializationScope* NativeMonitorSource::RealtimeInitialization() { return m_impl->initialization.get(); }
bool NativeMonitorSource::RefreshRealtimeEffects() noexcept
{
    try {
        if (!m_impl->initialization || !ready()) {
            return false;
        }
        m_impl->rebuild();
        return true;
    } catch (...) {
        m_impl->available.store(false, std::memory_order_release);
        return false;
    }
}

void NativeMonitorSource::Stop() noexcept { m_impl->stop(); }
bool NativeMonitorSource::NeedsRebuild() const { return m_impl->graph && m_impl->graph->NeedsRebuild(); }
bool NativeMonitorSource::NeedsRefresh(double position) const { return m_impl->needsRefresh(position); }
bool NativeMonitorSource::Refresh(double position) noexcept
{
    try {
        if (!m_impl->initialization || !ready()) {
            return false;
        }
        m_impl->refresh(position);
        return true;
    } catch (...) {
        m_impl->available.store(false, std::memory_order_release);
        return false;
    }
}

bool NativeMonitorSource::ready() const { return m_impl->available.load(std::memory_order_acquire); }
}
