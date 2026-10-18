/*
* Audacity: A Digital Audio Editor
*/
#include "MidiSequence.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <map>
#include <stdexcept>

#include "MidiTrack.h"
#include "MidiController.h"

namespace {
int order(const MidiEvent& event)
{
    const auto type = event.status & 0xf0;
    return type == 0x80 ? 0 : type == 0x90 ? 2 : 1;
}
}

MidiSequence::MidiSequence(const MidiTrack& track, double sampleRate, bool withEvents)
{
    if (!std::isfinite(sampleRate) || sampleRate <= 0.0) {
        throw std::invalid_argument("Invalid MIDI sample rate.");
    }
    if (!withEvents) return; // Live-only sources do not need to compile the document clips.
    const double samplesPerBeat = sampleRate * 60.0 / track.GetTempo();
    const auto sample = [samplesPerBeat](double beat) {
        const long double value = static_cast<long double>(beat) * samplesPerBeat;
        if (value > std::numeric_limits<int64_t>::max() - 1.0L) {
            throw std::overflow_error("The MIDI sequence is too long.");
        }
        return static_cast<int64_t>(std::llround(value));
    };
    for (const auto& clip : track.GetClips()) {
        const auto channel = static_cast<uint8_t>(clip.channel - 1);
        for (const auto& note : clip.notes) {
            const auto left = std::max(note.start, clip.trimLeft);
            const auto right = std::min(note.start + note.length, clip.length - clip.trimRight);
            if (left >= right) continue;
            const auto start = sample(clip.start + left);
            const auto end = std::max(start + 1, sample(clip.start + right));
            mEvents.push_back({ start, static_cast<uint8_t>(0x90 | channel), static_cast<uint8_t>(note.pitch),
                                static_cast<uint8_t>(note.velocity) });
            mEvents.push_back({ end, static_cast<uint8_t>(0x80 | channel), static_cast<uint8_t>(note.pitch), 0 });
        }
        std::map<int, MidiControllerData> initial;
        auto controllers = clip.controllers;
        std::stable_sort(controllers.begin(), controllers.end(), [](const auto& a, const auto& b) { return a.beat < b.beat; });
        for (const auto& controller : controllers) {
            if (controller.beat >= clip.trimLeft) continue;
            if (controller.number == 121) {
                for (auto& [number, event] : initial) {
                    const int reset = MidiControllerResetValue(number);
                    if (reset >= 0) event = { controller.beat, number, reset };
                }
            } else {
                const auto found = initial.find(controller.number);
                if (found == initial.end() || found->second.beat <= controller.beat) initial[controller.number] = controller;
            }
        }
        for (const auto& [number, controller] : initial) {
            mEvents.push_back({ sample(clip.visibleStart()), static_cast<uint8_t>(0xb0 | channel),
                                static_cast<uint8_t>(number), static_cast<uint8_t>(controller.value) });
        }
        for (const auto& controller : controllers) {
            if (controller.beat < clip.trimLeft || controller.beat >= clip.length - clip.trimRight) continue;
            mEvents.push_back({ sample(clip.start + controller.beat), static_cast<uint8_t>(0xb0 | channel),
                                static_cast<uint8_t>(controller.number), static_cast<uint8_t>(controller.value) });
        }
        mEvents.push_back({ sample(clip.visibleEnd()), static_cast<uint8_t>(0xb0 | channel), 64, 0 });
    }
    std::stable_sort(mEvents.begin(), mEvents.end(), [](const auto& a, const auto& b) {
        return a.sample != b.sample ? a.sample < b.sample : order(a) < order(b);
    });
}

void MidiSequence::AppendEvents(int64_t start, size_t frames, std::vector<MidiEvent>& output) const
{
    auto event = std::lower_bound(mEvents.begin(), mEvents.end(), start,
                                 [](const MidiEvent& event, int64_t time) { return event.sample < time; });
    for (; event != mEvents.end() && static_cast<uint64_t>(event->sample - start) < frames; ++event) {
        auto relative = *event;
        relative.sample -= start;
        output.push_back(relative);
    }
}

void MidiSequence::Chase(int64_t start, std::vector<MidiEvent>& output) const
{
    struct Channel {
        std::array<int, 128> controllers;
        std::array<uint8_t, 128> velocity {};
        std::array<bool, 128> pressed {};
        Channel() { controllers.fill(-1); }
    };
    std::array<Channel, 16> channels;
    for (const auto& event : mEvents) {
        if (event.sample >= start) {
            break;
        }
        auto& channel = channels[event.status & 0xf];
        const auto type = event.status & 0xf0;
        if (type == 0x90 && event.data2 != 0) {
            channel.velocity[event.data1] = event.data2;
            channel.pressed[event.data1] = true;
        } else if (type == 0x80 || type == 0x90) {
            channel.pressed[event.data1] = false;
            if (channel.controllers[64] < 64) {
                channel.velocity[event.data1] = 0;
            }
        } else if (type == 0xb0) {
            channel.controllers[event.data1] = event.data2;
            if (event.data1 == 121) {
                for (int cc = 0; cc < 120; ++cc) {
                    const auto value = MidiControllerResetValue(cc);
                    if (value >= 0 && (channel.controllers[cc] >= 0 || MidiControllerResetRequired(cc))) {
                        channel.controllers[cc] = value;
                    }
                }
                for (size_t note = 0; note < 128; ++note) {
                    if (!channel.pressed[note]) channel.velocity[note] = 0;
                }
            } else if (event.data1 == 64 && event.data2 < 64) {
                for (size_t note = 0; note < 128; ++note) {
                    if (!channel.pressed[note]) {
                        channel.velocity[note] = 0;
                    }
                }
            } else if (event.data1 == 120 || event.data1 >= 123) {
                channel.pressed.fill(false);
                if (event.data1 == 120 || channel.controllers[64] < 64) {
                    channel.velocity.fill(0);
                }
            }
        }
    }
    for (size_t index = 0; index < channels.size(); ++index) {
        const auto& channel = channels[index];
        for (size_t controller = 0; controller < 120; ++controller) {
            if (channel.controllers[controller] >= 0) {
                output.push_back({ 0, static_cast<uint8_t>(0xb0 | index), static_cast<uint8_t>(controller),
                                    static_cast<uint8_t>(channel.controllers[controller]) });
            }
        }
        for (size_t note = 0; note < 128; ++note) {
            if (channel.velocity[note]) {
                output.push_back({ 0, static_cast<uint8_t>(0x90 | index), static_cast<uint8_t>(note), channel.velocity[note] });
                if (!channel.pressed[note]) {
                    output.push_back({ 0, static_cast<uint8_t>(0x80 | index), static_cast<uint8_t>(note), 0 });
                }
            }
        }
    }
}
