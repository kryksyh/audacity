/*
* Audacity: A Digital Audio Editor
*/
#pragma once

#include <array>
#include <atomic>
#include "async/asyncable.h"
#include "modularity/ioc.h"
#include "midi/imidiinport.h"
#include "au3-audio-io/AudioIO.h"
#include "au3-midi-track/MidiTrack.h"
#include "au3-utility/LockFreeQueue.h"
#include "instrumentprocessor.h"

namespace au::au3audio {
class LiveInstrumentSession final : public InstrumentProcessor, public LiveAudioSource, public muse::async::Asyncable
{
    muse::GlobalInject<muse::midi::IMidiInPort> midiInput;

public:
    LiveInstrumentSession(std::shared_ptr<MidiTrack> track, std::shared_ptr<InstrumentProcessor> processor);
    ~LiveInstrumentSession() override;
    std::vector<audio::InstrumentParameter> parameters() const override { return m_processor->parameters(); }
    bool setParameter(const std::string& id, double value) override { return m_processor->setParameter(id, value); }
    std::string state() const override;
    bool matchesDocument() const;
    void open();
    bool takeChanges() override { return m_processor->takeChanges(); }
    bool active() const override { return m_active.load(); }
    void close() override;
    std::vector<audio::InstrumentMidiInput> midiInputs() const override;
    bool setMidiInput(const std::string& id) override;
    void sendMidi(int status, int data1, int data2) override;
    void releaseNotes() override;
    std::vector<audio::InstrumentMidiMessage> takeMidiMessages() override;
    std::string takeMidiInputError() override;
    bool attachView(void* parent, int x, int y, int width, int height) override
    { return m_processor->attachView(parent, x, y, width, height); }
    void detachView() override { m_processor->detachView(); }
    audio::InstrumentEditorSize editorSize() const override { return m_processor->editorSize(); }

    // Transfer ownership only after the previous audio thread has stopped.
    void beginTimeline();
    void endTimeline();
    // Main thread, after the failed processor has been detached from audio.
    void reportProcessingFailure(std::string error);
    bool inTimeline() const { return m_timeline.load(); }
    void prepare(double sampleRate, size_t maximumFrames) override;
    void reset() override;
    void resetTimeline() override;
    bool releaseLoopTimeline() override;
    size_t latency() const override { return m_processor->latency(); }
    size_t reportedLatency() const override { return m_processor->reportedLatency(); }
    bool latencyUpdatePending() const noexcept override { return m_processor->latencyUpdatePending(); }
    bool requiresMainThreadLatencyRefresh() const noexcept override { return m_processor->requiresMainThreadLatencyRefresh(); }
    bool refreshLatency() override { return m_processor->refreshLatency(); }
    bool acceptsLiveInput() const override { return true; }
    bool setAutomatedParameter(const std::string& id, double value) override { return m_processor->setAutomatedParameter(id, value); }
    void restoreManualParameters() noexcept override { m_processor->restoreManualParameters(); }
    bool restoreManualParameter(const std::string& id) noexcept override { return m_processor->restoreManualParameter(id); }
    void process(float* const* output, size_t channels, size_t frames, std::span<const MidiEvent> events,
                 const InstrumentProcessContext& context) override;
    void Prepare(double sampleRate) override;
    void Render(float* output, size_t channels, size_t frames) noexcept override;

private:
    using InputSource = audio::InstrumentMidiMessage::Source;
    void sendInputMidi(InputSource source, int status, int data1, int data2);
    void releaseInputSource(InputSource source);
    void queueInputMidi(InputSource source, int status, int data1, int data2);
    void appendInputMidi(InputSource source, int status, int data1, int data2);
    void appendMergedInput(MidiEvent event);
    void releaseAudioInput(InputSource source);
    void drainDriverRecording();
    void rememberRecordedInput(int status, int data1, int data2, double time);
    void releaseRecordedInput(double time);
    struct ControllerState {
        std::array<std::array<int16_t, 128>, 16> cc {}, poly {};
        std::array<int16_t, 16> pitch {}, pressure {};
        ControllerState() { clear(); }
        void clear()
        {
            for (auto& channel : cc) {
                channel.fill(-1);
            }
            for (auto& channel : poly) {
                channel.fill(-1);
            }
            pitch.fill(-1);
            pressure.fill(-1);
        }

        void observe(const MidiEvent& event);
        bool matches(const MidiEvent& event) const;
    };
    struct ControllerOrder {
        std::array<std::array<uint64_t, 128>, 16> cc {}, poly {};
        std::array<uint64_t, 16> pitch {}, pressure {};
    };
    struct TimedControllerState : ControllerState {
        ControllerOrder order;
        void clear() { ControllerState::clear(); order = {}; }
        void observe(const MidiEvent& event, uint64_t revision);
    };
    TimedControllerState m_timelineControls;
    ControllerState m_outputControls;
    uint64_t m_controllerRevision = 0;
    void appendEvent(const MidiEvent& event);
    void appendResetEvent(const MidiEvent& event);
    void resetInputControllers(InputSource source, int channel);
    void resetTimelineControllers(int channel, int64_t sample);
    void restoreTimelineControllers(int channel);
    void restoreTimelineParameters();
    std::array<std::array<bool, 130>, 16> m_restoredControllerParameters {};
    struct DriverInput;
    std::shared_ptr<DriverInput> m_driverInput;
    bool m_directInput = false;
    uint64_t m_audioInputGeneration = 0;
    void receiveMidi(const muse::midi::Event& event, muse::midi::IMidiInPort::InputTime time);
    std::shared_ptr<MidiTrack> m_track;
    std::shared_ptr<InstrumentProcessor> m_processor;
    const std::string m_description;
    mutable std::string m_documentState;
    std::string m_inputId;
    enum class InputAction {
        Message, Release, Panic
    };
    struct InputMessage {
        muse::midi::IMidiInPort::InputTime time {};
        MidiEvent event {};
        InputSource source = InputSource::Computer;
        InputAction action = InputAction::Message;
        uint64_t generation = 0;
    };
    LockFreeQueue<InputMessage> m_pending { 4096 };
    std::optional<InputMessage> m_nextMain, m_nextDriver;
    std::atomic<bool> m_release { false };
    std::atomic<bool> m_active { true };
    std::atomic<bool> m_timeline { false };
    std::atomic<bool> m_processingFailed { false };
    double m_preparedRate = 0.0;
    size_t m_preparedFrames = 0;
    // Audio-thread MIDI state, retained when ownership moves between threads.
    std::array<std::array<uint8_t, 128>, 16> m_liveNotes {}, m_liveSustained {};
    std::array<std::array<int16_t, 128>, 16> m_livePolyPressure {};
    std::array<std::array<int16_t, 128>, 16> m_liveControllers {};
    std::array<int16_t, 16> m_livePrograms {}, m_livePitch {}, m_livePressure {};
    bool m_replayInput = false;
    bool m_resetTimeline = false;
    bool m_preserveVoices = false;
    std::array<std::array<uint8_t, 128>, 16> m_timelineNotes {}, m_timelineSustained {};
    std::array<uint8_t, 16> m_timelinePedal {};
    std::array<bool, 16> m_timelinePrograms {};
    void clearTimeline();
    void appendTimeline(const MidiEvent& event);
    void releaseInputNotes(bool panic);
    void rememberInput(const MidiEvent& event);
    void chaseInput(bool notes = true, const std::array<bool, 16>* channels = nullptr, const std::array<bool, 16>* programs = nullptr);
    std::vector<audio::InstrumentMidiMessage> m_received;
    std::vector<MidiEvent> m_events;
    // Audio-thread ownership. Each producer has a separate SPSC queue; source
    // arbitration happens only here, after both queues reach the audio thread.
    struct InputNotes {
        std::array<std::array<uint8_t, 128>, 16> notes {};
        std::array<uint8_t, 16> pedal {};
    };
    struct InputState : InputNotes {
        TimedControllerState controls;
    };
    std::array<InputState, 2> m_inputs {};
    InputNotes m_recordedInput {}; // main-thread recording bookkeeping only
    std::string m_inputError;
    std::array<std::array<float, 512>, 2> m_buffers {};
    int64_t m_sample = 0;
    int64_t m_renderSample = 0;
};
}
