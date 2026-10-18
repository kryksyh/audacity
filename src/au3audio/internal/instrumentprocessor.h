/*
* Audacity: A Digital Audio Editor
*/
#pragma once

#include <span>
#include <optional>

#include "audio/iinstrumenteditor.h"
#include "au3-midi-track/MidiSequence.h"

namespace au::au3audio {
struct InstrumentProcessContext {
    int64_t sample = 0;
    double tempo = 120.0;
    int numerator = 4;
    int denominator = 4;
    bool playing = true;
    bool cycling = false;
    double cycleStart = 0.0;
    double cycleEnd = 0.0;
    // Project position without repeating a playback loop.
    std::optional<int64_t> continuousSample;
    // The processor's render clock, including stopped live monitoring.
    std::optional<int64_t> renderSample;
};

class InstrumentProcessor : public audio::IInstrumentEditor
{
public:
    virtual void prepare(double sampleRate, size_t maximumFrames) = 0;
    virtual void reset() = 0;
    // A document seek need not restart voices from a live keyboard.
    virtual void resetTimeline() { reset(); }
    // Called at the instrument's input loop boundary, which may precede the
    // audible wrap by its latency. True means timeline ownership handled the
    // release; otherwise the sequence sends explicit document note-offs.
    // Neither path may clear the instrument's queued audio or release tails.
    virtual bool releaseLoopTimeline() { return false; }
    virtual void process(float* const* output, size_t channels, size_t frames, std::span<const MidiEvent> events,
                         const InstrumentProcessContext& context) = 0;
    // Read only a published snapshot, including from the audio thread.
    virtual size_t latency() const { return 0; }
    virtual bool latencyUpdatePending() const noexcept { return false; }
    virtual bool requiresMainThreadLatencyRefresh() const noexcept { return false; }
    // Query only while quiescent on the native control thread. This measures
    // the block just rendered, before a refresh may rebuild DSP with a new delay.
    virtual size_t reportedLatency() const { return latency(); }
    // Native control operation: caller must own the quiescent processor.
    // A live VST3 refresh can reactivate its DSP; coordinate voice restoration
    // and compensation before using this operation during playback.
    virtual bool refreshLatency() { return false; }
    virtual bool acceptsLiveInput() const { return false; }
    //! Processing override: leave the manual preset intact and never invoke a native UI controller.
    virtual bool setAutomatedParameter(const std::string& id, double value) { return setParameter(id, value); }
    virtual void restoreManualParameters() noexcept {}
    // Main thread, for live input only. VST3 separates the native editor's
    // MIDI controller state from the sample-timed processor parameter input.
    // Keep their UI and saved preset in step without treating document
    // playback/automation as edits to that preset.
    virtual void syncLiveMidiControllers(std::span<const audio::InstrumentMidiMessage>) {}
    // Audio/control thread while processing is quiescent. Restore a mapped
    // controller's saved parameter before the next block's automation is set.
    // True means the native parameter restores its default without a MIDI CC.
    virtual bool restoreMidiController(int, int) noexcept { return false; }
    // Restore the channel's saved program parameter after a timeline override.
    virtual bool restoreMidiProgram(int) noexcept { return false; }
    // Restore only this override when other MIDI controllers must keep sounding.
    virtual bool restoreManualParameter(const std::string&) noexcept { return false; }
};

std::shared_ptr<InstrumentProcessor> createBuiltinInstrument(const std::string& state);
std::shared_ptr<InstrumentProcessor> createVstInstrument(const std::string& path, const std::string& state);
std::shared_ptr<InstrumentProcessor> createAudioUnitInstrument(const std::string& path, const std::string& state);
std::shared_ptr<InstrumentProcessor> createLv2Instrument(const std::string& path, const std::string& state);
std::shared_ptr<InstrumentProcessor> createInstrumentProcessor(const std::string& description, const std::string& state);
// Private offline processors may render on an export worker. Marshal their
// native control operation to the application thread while that worker waits.
bool refreshInstrumentLatency(InstrumentProcessor& processor);
// Private offline processors only: measure the rendered block, then settle
// bounded native restart requests on the control thread. Live workers never wait here.
size_t settleInstrumentLatency(InstrumentProcessor& processor);
}
