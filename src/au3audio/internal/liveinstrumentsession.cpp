/*
* Audacity: A Digital Audio Editor
*/
#include "liveinstrumentsession.h"
#include "au3-midi-track/MidiController.h"

#include <algorithm>
#include <chrono>
#include <utility>

using namespace au::au3audio;

// The port serializes producer calls; audio and UI each consume their own
// bounded queue. This object never owns a plugin, view, or session pointer.
struct LiveInstrumentSession::DriverInput final : muse::midi::IMidiInPort::RealtimeReceiver {
    using InputTime = muse::midi::IMidiInPort::InputTime;
    LockFreeQueue<InputMessage> audio { 4096 }, recording { 16384 };
    std::atomic<uint64_t> generation { 1 };
    std::atomic<bool> failed { false };
    std::atomic<int64_t> failureNanos { 0 };
    bool reported = false; // UI only

    void fail(InputTime time) noexcept
    {
        failureNanos = std::chrono::duration_cast<std::chrono::nanoseconds>(time.time_since_epoch()).count();
        failed = true;
        ++generation;
    }

    void receive(InputTime time, const muse::midi::Event& event) noexcept override
    {
        if (failed) {
            return;
        }
        try {
            // MIDI 2 -> MIDI 1 conversion may allocate on the driver thread,
            // never on the audio thread. It also preserves bank/RPN sequences.
            for (const auto& message : event.toMIDI10()) {
                unsigned char bytes[3] {};
                if (!message.toMidi10Bytes(bytes) || bytes[0] < 0x80 || bytes[0] >= 0xf0
                    || bytes[1] > 127 || bytes[2] > 127) {
                    continue;
                }
                const InputMessage input { time, { 0, bytes[0], bytes[1], bytes[2] }, InputSource::MidiDevice,
                                           InputAction::Message, generation.load() };
                if (!audio.Put(input) || !recording.Put(input)) {
                    fail(time);
                    return;
                }
            }
        } catch (...) {
            fail(time);
        }
    }

    void lost(InputTime time) noexcept override
    {
        ++generation;
        if (!recording.Put({ time, {}, InputSource::MidiDevice, InputAction::Release })) {
            fail(time);
        }
    }

    void restart() noexcept
    {
        // Called only after the port has finished clearing the old receiver.
        failed = false;
        reported = false;
    }
};

void LiveInstrumentSession::ControllerState::observe(const MidiEvent& event)
{
    const auto channel = event.status & 15;
    switch (event.status & 0xf0) {
    case 0xb0: if (event.data1 < 120) {
            cc[channel][event.data1] = event.data2;
    }
        break;
    case 0xa0: poly[channel][event.data1] = event.data2;
        break;
    case 0xe0: pitch[channel] = event.data1 | (event.data2 << 7);
        break;
    case 0xd0: pressure[channel] = event.data1;
        break;
    }
}

void LiveInstrumentSession::TimedControllerState::observe(const MidiEvent& event, uint64_t revision)
{
    ControllerState::observe(event);
    const auto channel = event.status & 15;
    switch (event.status & 0xf0) {
    case 0xb0: if (event.data1 < 120) {
            order.cc[channel][event.data1] = revision;
    }
        break;
    case 0xa0: order.poly[channel][event.data1] = revision;
        break;
    case 0xe0: order.pitch[channel] = revision;
        break;
    case 0xd0: order.pressure[channel] = revision;
        break;
    }
}

bool LiveInstrumentSession::ControllerState::matches(const MidiEvent& event) const
{
    const auto channel = event.status & 15;
    switch (event.status & 0xf0) {
    case 0xb0: return event.data1 < 120 && cc[channel][event.data1] == event.data2;
    case 0xa0: return poly[channel][event.data1] == event.data2;
    case 0xe0: return pitch[channel] == (event.data1 | (event.data2 << 7));
    case 0xd0: return pressure[channel] == event.data1;
    default: return false;
    }
}

void LiveInstrumentSession::appendEvent(const MidiEvent& event)
{
    m_outputControls.observe(event);
    m_events.push_back(event);
}

void LiveInstrumentSession::appendResetEvent(const MidiEvent& event)
{
    if (!m_outputControls.matches(event)) {
        appendEvent(event);
    }
}

LiveInstrumentSession::LiveInstrumentSession(std::shared_ptr<MidiTrack> track, std::shared_ptr<InstrumentProcessor> processor)
    : m_track(std::move(track)), m_processor(std::move(processor)),
    m_description(m_track->GetInstrumentDescription().utf8_string()), m_documentState(m_track->GetInstrumentState().utf8_string())
{
    m_driverInput = std::make_shared<DriverInput>();
    m_events.reserve(65536);
    for (auto& channel : m_liveControllers) {
        channel.fill(-1);
    }
    for (auto& channel : m_livePolyPressure) {
        channel.fill(-1);
    }
    m_livePrograms.fill(-1);
    m_livePitch.fill(-1);
    m_livePressure.fill(-1);
}

LiveInstrumentSession::~LiveInstrumentSession()
{
    close();
}

void LiveInstrumentSession::Prepare(double sampleRate)
{
    prepare(sampleRate, 512);
    m_sample = 0;
}

void LiveInstrumentSession::close()
{
    if (m_active) {
        releaseInputNotes(false);
    }
    m_active = false;
    setMidiInput({});
    m_processor->detachView();
}

std::string LiveInstrumentSession::state() const
{
    auto value = m_processor->state();
    // The piano-roll model writes this snapshot back immediately. Keep the
    // expected document state so undo/preset changes invalidate the cache.
    m_documentState = value;
    return value;
}

bool LiveInstrumentSession::matchesDocument() const
{
    return m_description == m_track->GetInstrumentDescription().utf8_string()
           && m_documentState == m_track->GetInstrumentState().utf8_string();
}

void LiveInstrumentSession::open() { m_active = true; }
void LiveInstrumentSession::beginTimeline() { m_timeline = true; }
void LiveInstrumentSession::endTimeline() { m_processor->restoreManualParameters(); m_timeline = false; }

void LiveInstrumentSession::reportProcessingFailure(std::string error)
{
    m_processingFailed.store(true, std::memory_order_release);
    // Native reactivation can leave DSP inactive. A same-format retry must
    // prepare again instead of taking the voice-preserving handoff shortcut.
    m_preparedRate = 0.0;
    m_preparedFrames = 0;
    m_inputError = std::move(error);
}

void LiveInstrumentSession::prepare(double rate, size_t maximumFrames)
{
    if (rate == m_preparedRate && maximumFrames == m_preparedFrames) {
        // The previous owner is quiescent. Release its document notes on the
        // next processing call, retaining live voices and their release tails.
        restoreTimelineParameters();
        m_preserveVoices = true;
        m_resetTimeline = true;
        return;
    }
    m_processor->prepare(rate, maximumFrames);
    m_preparedRate = rate;
    m_preparedFrames = maximumFrames;
    m_renderSample = 0;
    m_timelineNotes = {};
    m_timelineSustained = {};
    m_timelinePedal = {};
    m_timelinePrograms = {};
    m_timelineControls.clear();
    m_resetTimeline = false;
    m_preserveVoices = false;
    m_replayInput = true;
    m_processingFailed.store(false, std::memory_order_release);
}

void LiveInstrumentSession::reset()
{
    m_processor->reset();
    m_timelineNotes = {};
    m_timelineSustained = {};
    m_timelinePedal = {};
    m_timelinePrograms = {};
    m_timelineControls.clear();
    m_resetTimeline = false;
    m_preserveVoices = false;
    m_replayInput = true;
}

void LiveInstrumentSession::resetTimeline()
{
    // MidiPlayableSequence applies this block's automation after this call.
    restoreTimelineParameters();
    m_resetTimeline = true;
}

bool LiveInstrumentSession::releaseLoopTimeline()
{
    restoreTimelineParameters();
    m_preserveVoices = true;
    m_resetTimeline = true;
    return true;
}

void LiveInstrumentSession::clearTimeline()
{
    const auto hasNotes = [](const auto& channels) {
        return std::any_of(channels.begin(), channels.end(), [](const auto& notes) {
            return std::any_of(notes.begin(), notes.end(), [](auto value) { return value != 0; });
        });
    };
    const auto preserve = std::exchange(m_preserveVoices, false);
    const bool liveVoices = hasNotes(m_liveNotes) || hasNotes(m_liveSustained);
    for (int channel = 0; channel < 16; ++channel) {
        restoreTimelineControllers(channel);
    }
    if (!preserve && !liveVoices) {
        // Ordinary playback keeps the original hard seek/reset behavior.
        reset();
        return;
    }
    for (int channel = 0; channel < 16; ++channel) {
        for (int note = 0; note < 128; ++note) {
            if ((m_timelineNotes[channel][note] || m_timelineSustained[channel][note]) && !m_liveNotes[channel][note]) {
                appendEvent({ 0, static_cast<uint8_t>(0x80 | channel), static_cast<uint8_t>(note), 0 });
            }
        }
    }
    const auto programs = m_timelinePrograms;
    m_timelineNotes = {};
    m_timelineSustained = {};
    m_timelinePedal = {};
    m_timelinePrograms = {};
    m_timelineControls.clear();
    m_resetTimeline = false;
    // Restore keyboard controllers without retriggering its sounding notes.
    chaseInput(false, nullptr, &programs);
}

void LiveInstrumentSession::restoreTimelineParameters()
{
    m_restoredControllerParameters = {};
    for (int channel = 0; channel < 16; ++channel) {
        if (m_timelinePrograms[channel] && m_livePrograms[channel] < 0) {
            m_processor->restoreMidiProgram(channel);
        }
        for (int cc = 0; cc < 120; ++cc) {
            if (m_timelineControls.cc[channel][cc] >= 0 && m_liveControllers[channel][cc] < 0) {
                m_restoredControllerParameters[channel][cc] = m_processor->restoreMidiController(channel, cc);
            }
        }
        if (m_timelineControls.pitch[channel] >= 0 && m_livePitch[channel] < 0) {
            m_restoredControllerParameters[channel][129] = m_processor->restoreMidiController(channel, 129);
        }
        if (m_timelineControls.pressure[channel] >= 0 && m_livePressure[channel] < 0) {
            m_restoredControllerParameters[channel][128] = m_processor->restoreMidiController(channel, 128);
        }
    }
}

void LiveInstrumentSession::restoreTimelineControllers(int channel)
{
    for (int cc = 0; cc < 120; ++cc) {
        if (m_timelineControls.cc[channel][cc] < 0) {
            continue;
        }
        const auto live = m_liveControllers[channel][cc];
        const auto value = live >= 0 ? live : MidiControllerResetValue(cc);
        if (value >= 0 && (live >= 0 || !m_restoredControllerParameters[channel][cc])) {
            appendResetEvent({ 0, static_cast<uint8_t>(0xb0 | channel), static_cast<uint8_t>(cc), static_cast<uint8_t>(value) });
        }
    }
    if (m_timelineControls.pitch[channel] >= 0 && (m_livePitch[channel] >= 0 || !m_restoredControllerParameters[channel][129])) {
        const auto value = m_livePitch[channel] >= 0 ? m_livePitch[channel] : 8192;
        appendResetEvent({ 0, static_cast<uint8_t>(0xe0 | channel), static_cast<uint8_t>(value & 127), static_cast<uint8_t>(value >> 7) });
    }
    if (m_timelineControls.pressure[channel] >= 0 && (m_livePressure[channel] >= 0 || !m_restoredControllerParameters[channel][128])) {
        appendResetEvent({ 0, static_cast<uint8_t>(0xd0 | channel), static_cast<uint8_t>(std::max<int>(0, m_livePressure[channel])), 0 });
    }
    for (int note = 0; note < 128; ++note) {
        if (m_timelineControls.poly[channel][note] >= 0) {
            appendResetEvent({ 0, static_cast<uint8_t>(0xa0 | channel), static_cast<uint8_t>(note),
                               static_cast<uint8_t>(std::max<int>(0, m_livePolyPressure[channel][note])) });
        }
    }
    if (m_timelinePedal[channel] >= 64 && m_liveControllers[channel][64] < 64) {
        m_liveSustained[channel].fill(0);
    }
}

void LiveInstrumentSession::resetTimelineControllers(int channel, int64_t sample)
{
    m_timelinePedal[channel] = 0;
    for (int cc = 0; cc < 120; ++cc) {
        const auto initial = MidiControllerResetValue(cc);
        if (initial < 0 || (m_timelineControls.cc[channel][cc] < 0 && !MidiControllerResetRequired(cc))) {
            continue;
        }
        m_timelineControls.cc[channel][cc] = initial;
        m_timelineControls.order.cc[channel][cc] = 0;
        const auto value = m_liveControllers[channel][cc] >= 0 ? m_liveControllers[channel][cc] : initial;
        appendResetEvent({ sample, static_cast<uint8_t>(0xb0 | channel), static_cast<uint8_t>(cc), static_cast<uint8_t>(value) });
    }
    if (m_liveControllers[channel][64] < 64) {
        m_liveSustained[channel].fill(0);
        m_timelineSustained[channel].fill(0);
    }
    m_timelineControls.pitch[channel] = 8192;
    m_timelineControls.order.pitch[channel] = 0;
    const auto pitch = m_livePitch[channel] >= 0 ? m_livePitch[channel] : 8192;
    appendResetEvent({ sample, static_cast<uint8_t>(0xe0 | channel), static_cast<uint8_t>(pitch & 127), static_cast<uint8_t>(pitch >> 7) });
    m_timelineControls.pressure[channel] = 0;
    m_timelineControls.order.pressure[channel] = 0;
    appendResetEvent({ sample, static_cast<uint8_t>(0xd0 | channel), static_cast<uint8_t>(std::max<int>(0, m_livePressure[channel])), 0 });
    for (int note = 0; note < 128; ++note) {
        if (m_timelineControls.poly[channel][note] >= 0) {
            m_timelineControls.poly[channel][note] = 0;
            m_timelineControls.order.poly[channel][note] = 0;
            appendResetEvent({ sample, static_cast<uint8_t>(0xa0 | channel), static_cast<uint8_t>(note),
                               static_cast<uint8_t>(std::max<int>(0, m_livePolyPressure[channel][note])) });
        }
    }
}

void LiveInstrumentSession::resetInputControllers(InputSource source, int channel)
{
    auto& own = m_inputs[static_cast<size_t>(source)];
    const auto& other = m_inputs[1 - static_cast<size_t>(source)];
    own.pedal[channel] = 0;
    for (int cc = 0; cc < 120; ++cc) {
        const auto initial = MidiControllerResetValue(cc);
        if (initial < 0 || (own.controls.cc[channel][cc] < 0 && !MidiControllerResetRequired(cc))) {
            continue;
        }
        own.controls.cc[channel][cc] = initial;
        own.controls.order.cc[channel][cc] = 0;
        const auto otherValue = other.controls.cc[channel][cc];
        const auto input = otherValue >= 0 ? otherValue : initial;
        const auto timeline = m_timelineControls.cc[channel][cc];
        const auto effective = cc == 64 ? std::max<int>(input, m_timelinePedal[channel])
                               : m_timelineControls.order.cc[channel][cc] > other.controls.order.cc[channel][cc] ? timeline
                               : otherValue >= 0 ? otherValue : initial;
        // Remember only live input state. A document controller restored by
        // this reset must not turn into a sticky live-keyboard override.
        rememberInput({ 0, static_cast<uint8_t>(0xb0 | channel), static_cast<uint8_t>(cc), static_cast<uint8_t>(input) });
        appendResetEvent({ 0, static_cast<uint8_t>(0xb0 | channel), static_cast<uint8_t>(cc), static_cast<uint8_t>(effective) });
    }
    own.controls.pitch[channel] = 8192;
    own.controls.order.pitch[channel] = 0;
    const auto inputPitch = other.controls.pitch[channel] >= 0 ? other.controls.pitch[channel] : 8192;
    const auto pitch = m_timelineControls.order.pitch[channel] > other.controls.order.pitch[channel]
                       ? m_timelineControls.pitch[channel] : inputPitch;
    rememberInput({ 0, static_cast<uint8_t>(0xe0 | channel), static_cast<uint8_t>(inputPitch & 127),
                    static_cast<uint8_t>(inputPitch >> 7) });
    appendResetEvent({ 0, static_cast<uint8_t>(0xe0 | channel), static_cast<uint8_t>(pitch & 127), static_cast<uint8_t>(pitch >> 7) });
    own.controls.pressure[channel] = 0;
    own.controls.order.pressure[channel] = 0;
    const auto inputPressure = std::max<int>(0, other.controls.pressure[channel]);
    const auto pressure = m_timelineControls.order.pressure[channel] > other.controls.order.pressure[channel]
                          ? m_timelineControls.pressure[channel] : inputPressure;
    rememberInput({ 0, static_cast<uint8_t>(0xd0 | channel), static_cast<uint8_t>(inputPressure), 0 });
    appendResetEvent({ 0, static_cast<uint8_t>(0xd0 | channel), static_cast<uint8_t>(pressure), 0 });
    for (int note = 0; note < 128; ++note) {
        if (own.controls.poly[channel][note] >= 0) {
            own.controls.poly[channel][note] = 0;
            own.controls.order.poly[channel][note] = 0;
            const auto inputValue = std::max<int>(0, other.controls.poly[channel][note]);
            const auto value = m_timelineControls.order.poly[channel][note] > other.controls.order.poly[channel][note]
                               ? m_timelineControls.poly[channel][note] : inputValue;
            rememberInput({ 0, static_cast<uint8_t>(0xa0 | channel), static_cast<uint8_t>(note), static_cast<uint8_t>(inputValue) });
            appendResetEvent({ 0, static_cast<uint8_t>(0xa0 | channel), static_cast<uint8_t>(note), static_cast<uint8_t>(value) });
        }
    }
}

void LiveInstrumentSession::appendTimeline(const MidiEvent& event)
{
    const auto channel = event.status & 0xf;
    const auto type = event.status & 0xf0;
    if (type == 0xc0) {
        m_timelinePrograms[channel] = true;
    }
    if (type == 0xb0 && event.data1 == 121) {
        resetTimelineControllers(channel, event.sample);
        return;
    }
    m_timelineControls.observe(event, ++m_controllerRevision);
    if (type == 0xb0 && (event.data1 == 120 || event.data1 >= 123)) {
        const auto sounding = [](const auto& notes) {
            return std::any_of(notes.begin(), notes.end(), [](auto note) { return note != 0; });
        };
        const bool shared = sounding(m_liveNotes[channel]) || sounding(m_liveSustained[channel]);
        // A channel-mode command belongs to this source. Send explicit offs so
        // VST3 instruments without an All Notes Off mapping also receive it.
        // Shared sustain keeps its normal channel-wide semantics; never hard
        // kill or retrigger an unrelated live voice on that channel.
        for (int note = 0; note < 128; ++note) {
            if (m_timelineNotes[channel][note]) {
                appendTimeline({ event.sample, static_cast<uint8_t>(0x80 | channel), static_cast<uint8_t>(note), 0 });
            }
        }
        if (!shared && event.data1 != 123) {
            appendEvent(event);
            if (event.data1 == 120) {
                m_timelineNotes[channel].fill(0);
                m_timelineSustained[channel].fill(0);
            }
        }
        return;
    }
    if (type == 0x90 && event.data2) {
        m_timelineNotes[channel][event.data1] = event.data2;
        m_timelineSustained[channel][event.data1] = 0;
        if (m_liveNotes[channel][event.data1]) {
            return;
        }
    } else if (type == 0x80 || type == 0x90) {
        if (m_timelinePedal[channel] >= 64 || m_liveControllers[channel][64] >= 64) {
            if (m_timelineNotes[channel][event.data1]) {
                m_timelineSustained[channel][event.data1] = m_timelineNotes[channel][event.data1];
            }
        }
        m_timelineNotes[channel][event.data1] = 0;
        if (m_liveNotes[channel][event.data1]) {
            return;
        }
    } else if (type == 0xb0 && event.data1 == 64) {
        m_timelinePedal[channel] = event.data2;
        auto merged = event;
        merged.data2 = std::max<int>(event.data2, m_liveControllers[channel][64]);
        if (merged.data2 < 64) {
            m_timelineSustained[channel].fill(0);
            m_liveSustained[channel].fill(0);
        }
        appendEvent(merged);
        return;
    }
    appendEvent(event);
}

void LiveInstrumentSession::rememberInput(const MidiEvent& event)
{
    const auto channel = event.status & 0xf;
    const auto noteOff = [&] {
        if (m_liveNotes[channel][event.data1] && (m_liveControllers[channel][64] >= 64 || m_timelinePedal[channel] >= 64)) {
            m_liveSustained[channel][event.data1] = m_liveNotes[channel][event.data1];
        }
        m_liveNotes[channel][event.data1] = 0;
    };
    switch (event.status & 0xf0) {
    case 0x80: noteOff();
        break;
    case 0x90:
        if (event.data2 == 0) {
            noteOff();
            break;
        }
        m_liveNotes[channel][event.data1] = event.data2;
        m_liveSustained[channel][event.data1] = 0;
        break;
    case 0xa0: m_livePolyPressure[channel][event.data1] = event.data2;
        break;
    case 0xb0:
        if (event.data1 == 120 || event.data1 == 123) {
            m_liveNotes[channel].fill(0);
            m_liveSustained[channel].fill(0);
        }
        if (event.data1 == 64 && event.data2 < 64 && m_timelinePedal[channel] < 64) {
            m_liveSustained[channel].fill(0);
            m_timelineSustained[channel].fill(0);
        }
        m_liveControllers[channel][event.data1] = event.data2;
        break;
    case 0xc0: m_livePrograms[channel] = event.data1;
        break;
    case 0xd0: m_livePressure[channel] = event.data1;
        break;
    case 0xe0: m_livePitch[channel] = event.data1 | (event.data2 << 7);
        break;
    default: break;
    }
}

void LiveInstrumentSession::chaseInput(bool notes, const std::array<bool, 16>* channels, const std::array<bool, 16>* programs)
{
    for (int channel = 0; channel < 16; ++channel) {
        if (channels && !(*channels)[channel]) {
            continue;
        }
        // Bank select precedes program change; channel mode commands are not
        // replayed after notes, as that would immediately silence the chase.
        for (const auto controller : { 0, 32 }) {
            if (const auto value = m_liveControllers[channel][controller]; value >= 0) {
                appendEvent({ 0, static_cast<uint8_t>(0xb0 | channel), static_cast<uint8_t>(controller), static_cast<uint8_t>(value) });
            }
        }
        if (const auto program = m_livePrograms[channel]; program >= 0 && (!programs || (*programs)[channel])) {
            appendEvent({ 0, static_cast<uint8_t>(0xc0 | channel), static_cast<uint8_t>(program), 0 });
        }
        for (int controller = 1; controller < 120; ++controller) {
            if (controller == 32) {
                continue;
            }
            if (const auto value = m_liveControllers[channel][controller]; value >= 0) {
                appendEvent({ 0, static_cast<uint8_t>(0xb0 | channel), static_cast<uint8_t>(controller), static_cast<uint8_t>(value) });
            }
        }
        if (const auto pitch = m_livePitch[channel]; pitch >= 0) {
            appendEvent({ 0, static_cast<uint8_t>(0xe0 | channel), static_cast<uint8_t>(pitch & 127), static_cast<uint8_t>(pitch >> 7) });
        }
        if (const auto pressure = m_livePressure[channel]; pressure >= 0) {
            appendEvent({ 0, static_cast<uint8_t>(0xd0 | channel), static_cast<uint8_t>(pressure), 0 });
        }
        if (!notes) {
            continue;
        }
        for (int note = 0; note < 128; ++note) {
            const auto held = m_liveNotes[channel][note];
            const auto velocity = held ? held : m_liveSustained[channel][note];
            if (velocity) {
                appendEvent({ 0, static_cast<uint8_t>(0x90 | channel), static_cast<uint8_t>(note), velocity });
                if (const auto pressure = m_livePolyPressure[channel][note]; pressure >= 0) {
                    appendEvent({ 0, static_cast<uint8_t>(0xa0 | channel), static_cast<uint8_t>(note), static_cast<uint8_t>(pressure) });
                }
                if (!held) {
                    appendEvent({ 0, static_cast<uint8_t>(0x80 | channel), static_cast<uint8_t>(note), 0 });
                }
            }
        }
    }
}

void LiveInstrumentSession::process(float* const* output, size_t channels, size_t frames, std::span<const MidiEvent> events,
                                    const InstrumentProcessContext& context)
{
    if (m_processingFailed.load(std::memory_order_acquire)) {
        for (size_t channel = 0; channel < channels; ++channel) {
            std::fill_n(output[channel], frames, 0.0f);
        }
        return;
    }
    m_events.clear();
    m_outputControls.clear();
    if (m_release.exchange(false)) {
        m_liveNotes = {};
        m_liveSustained = {};
        for (auto& channel : m_liveControllers) {
            channel.fill(-1);
        }
        for (auto& channel : m_livePolyPressure) {
            channel.fill(-1);
        }
        m_processor->reset();
        m_timelineNotes = {};
        m_timelineSustained = {};
        m_timelinePedal = {};
        m_timelinePrograms = {};
        m_timelineControls.clear();
        m_resetTimeline = false;
        m_preserveVoices = false;
        InputMessage discarded;
        while (m_pending.Get(discarded)) {}
        while (m_driverInput->audio.Get(discarded)) {}
        m_inputs = {};
        m_nextMain.reset();
        m_nextDriver.reset();
        m_replayInput = true;
    }
    if (m_resetTimeline) {
        clearTimeline();
    }
    if (m_replayInput) {
        chaseInput();
        m_replayInput = false;
    }
    const auto syncDeviceGeneration = [this] {
        const auto generation = m_driverInput->generation.load();
        if (generation != m_audioInputGeneration) {
            releaseAudioInput(InputSource::MidiDevice);
            m_audioInputGeneration = generation;
        }
    };
    syncDeviceGeneration();
    InputMessage main, driver;
    bool hasMain = m_nextMain.has_value(), hasDriver = m_nextDriver.has_value();
    if (hasMain) {
        main = *std::exchange(m_nextMain, {});
    } else {
        hasMain = m_pending.Get(main);
    }
    if (hasDriver) {
        driver = *std::exchange(m_nextDriver, {});
    } else {
        hasDriver = m_driverInput->audio.Get(driver);
    }
    // Merge producer order by input time without allocating or locking. The
    // bound prevents a continuously busy producer from monopolizing a callback.
    for (size_t count = 0; count < 8192 && (hasMain || hasDriver); ++count) {
        if (hasDriver && (!hasMain || driver.time < main.time)) {
            syncDeviceGeneration();
            if (driver.generation == m_audioInputGeneration) {
                appendInputMidi(InputSource::MidiDevice, driver.event.status, driver.event.data1, driver.event.data2);
            }
            hasDriver = m_driverInput->audio.Get(driver);
        } else {
            if (main.action == InputAction::Release) {
                releaseAudioInput(main.source);
            } else if (main.action == InputAction::Panic) {
                appendMergedInput(main.event);
            } else {
                appendInputMidi(main.source, main.event.status, main.event.data1, main.event.data2);
            }
            hasMain = m_pending.Get(main);
        }
    }
    if (hasMain) {
        m_nextMain = main;
    }
    if (hasDriver) {
        m_nextDriver = driver;
    }
    syncDeviceGeneration();
    // Incoming live messages are at offset zero; timeline messages already use
    // block offsets, so this merge retains nondecreasing MIDI timestamps.
    for (const auto& timeline : events) {
        appendTimeline(timeline);
    }
    auto renderContext = context;
    renderContext.renderSample = m_renderSample;
    m_processor->process(output, channels, frames, m_events, renderContext);
    m_renderSample += static_cast<int64_t>(frames);
}

std::vector<au::audio::InstrumentMidiInput> LiveInstrumentSession::midiInputs() const
{
    std::vector<audio::InstrumentMidiInput> result;
    for (const auto& device : midiInput()->availableDevices()) {
        if (device.id != muse::midi::NONE_DEVICE_ID) {
            result.push_back({ device.id, device.name });
        }
    }
    return result;
}

bool LiveInstrumentSession::setMidiInput(const std::string& id)
{
    if (id == m_inputId && !m_driverInput->failed
        && (id.empty() || (midiInput()->isConnected() && midiInput()->deviceID() == id))) {
        return true;
    }
    if (!m_inputId.empty()) {
        const auto previous = std::exchange(m_inputId, {});
        if (m_directInput) {
            midiInput()->clearRealtimeReceiver(m_driverInput);
            m_directInput = false;
            drainDriverRecording();
        }
        midiInput()->eventReceived().disconnect(this);
        if (auto channel = midiInput()->timestampedEventReceived()) {
            channel->disconnect(this);
        }
        releaseInputSource(InputSource::MidiDevice);
        if (midiInput()->deviceID() == previous) {
            midiInput()->disconnect();
        }
    }
    if (id.empty()) {
        return true;
    }
    if (!m_active || !midiInput()->connect(id)) {
        return false;
    }
    m_inputId = id;
    const auto checkConnection = [this] {
        if (!m_inputId.empty() && (!midiInput()->isConnected() || midiInput()->deviceID() != m_inputId)) {
            setMidiInput({});
        }
    };
    midiInput()->deviceChanged().onNotify(this, checkConnection, muse::async::Asyncable::Mode::SetReplace);
    midiInput()->availableDevicesChanged().onNotify(this, checkConnection, muse::async::Asyncable::Mode::SetReplace);
    m_driverInput->restart();
    m_directInput = midiInput()->setRealtimeReceiver(m_driverInput);
    if (m_directInput) {
        return true;
    }
    if (auto channel = midiInput()->timestampedEventReceived()) {
        channel->onReceive(this, [this, id](muse::midi::IMidiInPort::InputTime time, const muse::midi::Event& event) {
            if (m_inputId == id) {
                receiveMidi(event, time);
            }
        });
    } else {
        midiInput()->eventReceived().onReceive(this, [this, id](muse::midi::tick_t, const muse::midi::Event& event) {
            if (m_inputId == id) {
                receiveMidi(event, std::chrono::steady_clock::now());
            }
        });
    }
    return true;
}

void LiveInstrumentSession::receiveMidi(const muse::midi::Event& event, muse::midi::IMidiInPort::InputTime inputTime)
{
    if (!m_active || m_inputId.empty() || midiInput()->deviceID() != m_inputId) {
        return;
    }
    const double time = std::chrono::duration<double>(inputTime.time_since_epoch()).count();
    for (const auto& message : event.toMIDI10()) {
        unsigned char data[3] {};
        const auto count = message.toMidi10Bytes(data);
        if (count > 0) {
            sendInputMidi(InputSource::MidiDevice, data[0], data[1], data[2]);
            rememberRecordedInput(data[0], data[1], data[2], time);
        }
    }
}

void LiveInstrumentSession::sendMidi(int status, int data1, int data2)
{
    sendInputMidi(InputSource::Computer, status, data1, data2);
}

void LiveInstrumentSession::sendInputMidi(InputSource source, int status, int data1, int data2)
{
    if (!m_active || status < 0x80 || status >= 0xf0 || data1 < 0 || data1 > 127 || data2 < 0 || data2 > 127) {
        return;
    }
    queueInputMidi(source, status, data1, data2);
}

void LiveInstrumentSession::appendInputMidi(InputSource source, int status, int data1, int data2)
{
    const auto type = status & 0xf0;
    const auto channel = status & 0xf;
    const auto index = static_cast<size_t>(source);
    auto& own = m_inputs[index];
    const auto& other = m_inputs[1 - index];
    if (type == 0xb0 && data1 == 121) {
        resetInputControllers(source, channel);
        return;
    }
    own.controls.observe({ 0, static_cast<uint8_t>(status), static_cast<uint8_t>(data1), static_cast<uint8_t>(data2) },
                         ++m_controllerRevision);
    if (type == 0x90 || type == 0x80) {
        own.notes[channel][data1] = type == 0x90 ? data2 : 0;
        if (other.notes[channel][data1]) {
            return;
        }
    } else if (type == 0xb0 && data1 == 64) {
        own.pedal[channel] = data2;
        data2 = std::max<int>(data2, other.pedal[channel]);
    } else if (type == 0xb0 && (data1 == 120 || data1 >= 123)) {
        // A device's channel-mode message releases its keys, not keys owned by
        // the computer keyboard or timeline on the same MIDI channel.
        for (int note = 0; note < 128; ++note) {
            if (own.notes[channel][note]) {
                appendInputMidi(source, 0x80 | channel, note, 0);
            }
        }
        // All Notes Off retains sustain. All Sound Off is safe to forward only
        // when no other source can own a sounding voice on the shared channel.
        const auto sounding = [](const auto& notes) {
            return std::any_of(notes.begin(), notes.end(), [](auto note) { return note != 0; });
        };
        if (data1 != 123 && !sounding(m_timelineNotes[channel]) && !sounding(m_timelineSustained[channel])
            && other.pedal[channel] < 64 && !sounding(other.notes[channel])) {
            appendMergedInput({ 0, static_cast<uint8_t>(status), static_cast<uint8_t>(data1), static_cast<uint8_t>(data2) });
        }
        return;
    }
    appendMergedInput({ 0, static_cast<uint8_t>(status), static_cast<uint8_t>(data1), static_cast<uint8_t>(data2) });
}

void LiveInstrumentSession::appendMergedInput(MidiEvent event)
{
    rememberInput(event);
    const auto channel = event.status & 0xf;
    const auto type = event.status & 0xf0;
    if ((type == 0x90 || type == 0x80) && m_timelineNotes[channel][event.data1]) {
        return;
    }
    if (type == 0xb0 && event.data1 == 64) {
        event.data2 = std::max<int>(event.data2, m_timelinePedal[channel]);
    }
    appendEvent(event);
}

void LiveInstrumentSession::queueInputMidi(InputSource source, int status, int data1, int data2)
{
    if (!m_pending.Put({ std::chrono::steady_clock::now(),
                         { 0, static_cast<uint8_t>(status), static_cast<uint8_t>(data1), static_cast<uint8_t>(data2) }, source })) {
        m_release = true;
    }
}

void LiveInstrumentSession::releaseNotes() { releaseInputNotes(true); }

void LiveInstrumentSession::releaseInputNotes(bool panic)
{
    if (!m_active) {
        return;
    }
    releaseInputSource(InputSource::Computer);
    releaseInputSource(InputSource::MidiDevice);
    if (!panic) {
        return;
    }
    for (int channel = 0; channel < 16; ++channel) {
        for (const int command : { 120, 123 }) {
            if (!m_pending.Put({ std::chrono::steady_clock::now(),
                                 { 0, static_cast<uint8_t>(0xb0 | channel), static_cast<uint8_t>(command), 0 },
                                 InputSource::Computer, InputAction::Panic })) {
                m_release = true;
            }
        }
    }
}

void LiveInstrumentSession::releaseInputSource(InputSource source)
{
    if (source == InputSource::MidiDevice) {
        drainDriverRecording();
        releaseRecordedInput(std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count());
    }
    if (!m_pending.Put({ std::chrono::steady_clock::now(), {}, source, InputAction::Release })) {
        m_release = true;
    }
}

void LiveInstrumentSession::releaseAudioInput(InputSource source)
{
    auto& own = m_inputs[static_cast<size_t>(source)];
    for (int channel = 0; channel < 16; ++channel) {
        for (int note = 0; note < 128; ++note) {
            if (own.notes[channel][note]) {
                appendInputMidi(source, 0x80 | channel, note, 0);
            }
        }
        if (own.pedal[channel]) {
            appendInputMidi(source, 0xb0 | channel, 64, 0);
        }
    }
    own.controls.clear();
}

void LiveInstrumentSession::rememberRecordedInput(int status, int data1, int data2, double time)
{
    const auto type = status & 0xf0, channel = status & 15;
    if (type == 0x90 || type == 0x80) {
        m_recordedInput.notes[channel][data1] = type == 0x90 ? data2 : 0;
    } else if (type == 0xb0 && data1 == 64) {
        m_recordedInput.pedal[channel] = data2;
    } else if (type == 0xb0 && data1 == 121) {
        m_recordedInput.pedal[channel] = 0;
    } else if (type == 0xb0 && (data1 == 120 || data1 >= 123)) {
        m_recordedInput.notes[channel].fill(0);
    }
    m_received.push_back({ status, data1, data2, time, InputSource::MidiDevice });
}

void LiveInstrumentSession::releaseRecordedInput(double time)
{
    for (int channel = 0; channel < 16; ++channel) {
        for (int note = 0; note < 128; ++note) {
            if (m_recordedInput.notes[channel][note]) {
                rememberRecordedInput(0x80 | channel, note, 0, time);
            }
        }
        if (m_recordedInput.pedal[channel]) {
            rememberRecordedInput(0xb0 | channel, 64, 0, time);
        }
    }
}

void LiveInstrumentSession::drainDriverRecording()
{
    InputMessage input;
    for (size_t count = 0; count < 16384 && m_driverInput->recording.Get(input); ++count) {
        const auto time = std::chrono::duration<double>(input.time.time_since_epoch()).count();
        if (input.action == InputAction::Release) {
            releaseRecordedInput(time);
        } else {
            rememberRecordedInput(input.event.status, input.event.data1, input.event.data2, time);
        }
    }
    if (m_driverInput->failed && !m_driverInput->reported) {
        m_driverInput->reported = true;
        releaseRecordedInput(m_driverInput->failureNanos.load() * 1e-9);
        m_inputError = "MIDI input overflowed. Some events were lost; select the MIDI input again to reconnect.";
    }
}

std::vector<au::audio::InstrumentMidiMessage> LiveInstrumentSession::takeMidiMessages()
{
    drainDriverRecording();
    std::vector<audio::InstrumentMidiMessage> result;
    result.swap(m_received);
    // This queue is consumed on main, independently of the sample-timed audio
    // queue. A VST3 editor must receive live CC input on its control thread.
    m_processor->syncLiveMidiControllers(result);
    return result;
}

std::string LiveInstrumentSession::takeMidiInputError() { return std::exchange(m_inputError, {}); }

void LiveInstrumentSession::Render(float* output, size_t channels, size_t frames) noexcept
{
    std::fill_n(output, channels * frames, 0.0f);
    if (!m_active || m_timeline) {
        return;
    }
    try {
        size_t position = 0;
        while (position < frames) {
            const auto count = std::min<size_t>(512, frames - position);
            float* pointers[] { m_buffers[0].data(), m_buffers[1].data() };
            process(pointers, 2, count, {}, { m_sample, m_track->GetTempo(), 4, 4, false });
            const double gain = m_track->GetMute() ? 0.0 : m_track->GetGain();
            const double pan = m_track->GetPan();
            for (size_t frame = 0; frame < count; ++frame) {
                for (size_t channel = 0; channel < channels; ++channel) {
                    const double balance = channel == 0 ? std::min(1.0, 1.0 - pan) : std::min(1.0, 1.0 + pan);
                    output[(position + frame) * channels + channel]
                        = static_cast<float>(m_buffers[std::min<size_t>(channel, 1)][frame] * gain * balance);
                }
            }
            m_sample += count;
            position += count;
        }
    } catch (...) {
        m_active = false;
        std::fill_n(output, channels * frames, 0.0f);
    }
}
