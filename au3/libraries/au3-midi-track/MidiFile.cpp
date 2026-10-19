/*
* Audacity: A Digital Audio Editor
*/
#include "MidiFile.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <deque>
#include <limits>
#include <map>
#include <stdexcept>

namespace {
// MIDI Association RP-001: https://midi.org/standard-midi-files-specification
class Reader
{
public:
    explicit Reader(std::span<const uint8_t> bytes)
        : mBytes(bytes) {}
    size_t remaining() const { return mBytes.size() - mPosition; }
    uint8_t byte() { return take(1).front(); }
    uint32_t integer(size_t count)
    {
        uint32_t value = 0;
        for (auto b : take(count)) {
            value = (value << 8) | b;
        }
        return value;
    }

    uint32_t variable()
    {
        uint32_t value = 0;
        for (int i = 0; i < 4; ++i) {
            const auto b = byte();
            value = (value << 7) | (b & 0x7f);
            if (!(b & 0x80)) {
                return value;
            }
        }
        throw std::runtime_error("Invalid MIDI variable-length quantity.");
    }

    std::span<const uint8_t> take(size_t count)
    {
        if (count > remaining()) {
            throw std::runtime_error("Truncated MIDI file.");
        }
        auto result = mBytes.subspan(mPosition, count);
        mPosition += count;
        return result;
    }

private:
    std::span<const uint8_t> mBytes;
    size_t mPosition = 0;
};

struct Note {
    uint64_t start = 0, end = 0;
    int pitch = 0, velocity = 0;
};
struct Controller {
    uint64_t tick = 0;
    MidiControllerData event;
};
struct Part {
    std::vector<Note> notes;
    std::vector<Controller> controllers;
    std::map<int, std::deque<size_t> > held;
};
struct Track {
    wxString name;
    uint64_t end = 0;
    std::map<int, Part> parts;
};
struct Tempo {
    uint64_t tick = 0;
    uint32_t microseconds = 500000;
    double seconds = 0.0;
};

class Parser
{
public:
    MidiFileData read(std::span<const uint8_t> bytes, double projectTempo)
    {
        if (bytes.size() > 64 * 1024 * 1024) {
            throw std::runtime_error("The MIDI file is too large.");
        }
        Reader file(bytes);
        if (file.integer(4) != 0x4d546864) {
            throw std::runtime_error("Not a Standard MIDI File.");
        }
        Reader header(file.take(file.integer(4)));
        const auto format = header.integer(2), count = header.integer(2);
        mDivision = header.integer(2);
        if (format > 1) {
            throw std::runtime_error("Only MIDI file formats 0 and 1 are supported.");
        }
        if (!count || count > 4096 || (format == 0 && count != 1) || !mDivision) {
            throw std::runtime_error("Invalid MIDI file header.");
        }
        mTempos.push_back({});
        for (unsigned i = 0; i < count;) {
            const auto id = file.integer(4);
            auto chunk = file.take(file.integer(4));
            if (id != 0x4d54726b) {
                continue;                   // SMF readers must skip unknown chunks.
            }
            mTracks.push_back(readTrack(chunk));
            ++i;
        }
        std::stable_sort(mTempos.begin(), mTempos.end(), [](const auto& a, const auto& b) { return a.tick < b.tick; });
        for (size_t i = 1; i < mTempos.size(); ++i) {
            const auto& previous = mTempos[i - 1];
            mTempos[i].seconds = previous.seconds + ticksToSeconds(mTempos[i].tick - previous.tick, previous.microseconds);
        }
        auto initial = std::upper_bound(mTempos.begin(), mTempos.end(), uint64_t(0),
                                        [](uint64_t tick, const auto& tempo) { return tick < tempo.tick; });
        mResult.tempo = 60000000.0 / std::prev(initial)->microseconds;
        const double tempo = projectTempo == 0.0 ? mResult.tempo : projectTempo;
        if (!std::isfinite(tempo) || tempo <= 0.0) {
            throw std::runtime_error("Invalid project tempo.");
        }
        for (auto& source : mTracks) {
            mResult.duration = std::max(mResult.duration, seconds(source.end));
            for (auto& [channel, part] : source.parts) {
                if (part.notes.empty() && part.controllers.empty()) {
                    continue;
                }
                auto track = MidiTrack::Create();
                wxString name = source.name;
                if (name.empty()) {
                    name = wxString::Format("MIDI %u", static_cast<unsigned>(mResult.tracks.size() + 1));
                }
                if (source.parts.size() > 1) {
                    name += wxString::Format(" (%d)", channel + 1);
                }
                track->SetName(name);
                track->SetTempo(tempo);
                MidiClipData clip;
                clip.id = 1;
                clip.name = name;
                clip.channel = channel + 1;
                clip.length = std::max(seconds(source.end) * tempo / 60.0, 1.0 / 960.0);
                int64_t id = 1;
                for (const auto& note : part.notes) {
                    const auto start = seconds(note.start) * tempo / 60.0;
                    const auto end = seconds(note.end) * tempo / 60.0;
                    clip.notes.push_back({ id++, start, end - start, note.pitch, note.velocity });
                    clip.length = std::max(clip.length, end);
                }
                for (auto& controller : part.controllers) {
                    controller.event.beat = seconds(controller.tick) * tempo / 60.0;
                    clip.controllers.push_back(controller.event);
                    // Include setup messages at the final tick in playback.
                    clip.length = std::max(clip.length, controller.event.beat + 1.0 / 960.0);
                }
                if (!track->SetClips({ std::move(clip) })) {
                    throw std::runtime_error("Invalid MIDI track data.");
                }
                mResult.duration = std::max(mResult.duration, track->GetEndTime());
                mResult.tracks.push_back(std::move(track));
            }
        }
        if (mResult.tracks.empty()) {
            throw std::runtime_error("The MIDI file contains no notes or channel events.");
        }
        mResult.success = true;
        return std::move(mResult);
    }

private:
    double ticksToSeconds(uint64_t ticks, uint32_t tempo) const
    {
        if (!(mDivision & 0x8000)) {
            return double(ticks) * tempo / (1000000.0 * mDivision);
        }
        const int frames = 256 - (mDivision >> 8);
        const int subframes = mDivision & 0xff;
        if (!subframes || (frames != 24 && frames != 25 && frames != 29 && frames != 30)) {
            throw std::runtime_error("Invalid MIDI SMPTE timing division.");
        }
        return double(ticks) / ((frames == 29 ? 30000.0 / 1001.0 : frames) * subframes);
    }

    double seconds(uint64_t tick) const
    {
        if (mDivision & 0x8000) {
            return ticksToSeconds(tick, 500000);
        }
        auto tempo = std::upper_bound(mTempos.begin(), mTempos.end(), tick,
                                      [](uint64_t t, const auto& entry) { return t < entry.tick; });
        --tempo;
        return tempo->seconds + ticksToSeconds(tick - tempo->tick, tempo->microseconds);
    }

    Track readTrack(std::span<const uint8_t> bytes)
    {
        Reader input(bytes);
        Track track;
        uint8_t running = 0;
        uint64_t tick = 0;
        bool ended = false;
        while (input.remaining()) {
            if (++mEvents > 1000000) {
                throw std::runtime_error("The MIDI file contains too many events.");
            }
            const auto delta = input.variable();
            if (tick > std::numeric_limits<uint64_t>::max() - delta) {
                throw std::runtime_error("MIDI time overflow.");
            }
            tick += delta;
            auto status = input.byte();
            int first = -1;
            if (status < 0x80) {
                first = status;
                status = running;
                if (!status) {
                    throw std::runtime_error("MIDI running status has no preceding channel message.");
                }
            }
            if (status == 0xff) {
                running = 0;
                const auto type = input.byte();
                auto data = input.take(input.variable());
                if (type == 0x2f) {
                    if (!data.empty()) {
                        throw std::runtime_error("Invalid MIDI end-of-track event.");
                    }
                    ended = true;
                    break;
                }
                if (type == 0x03 && track.name.empty()) {
                    track.name = wxString::FromUTF8(reinterpret_cast<const char*>(data.data()), data.size());
                }
                if (type == 0x51) {
                    if (data.size() != 3) {
                        throw std::runtime_error("Invalid MIDI tempo event.");
                    }
                    const uint32_t tempo = (uint32_t(data[0]) << 16) | (uint32_t(data[1]) << 8) | data[2];
                    if (!tempo) {
                        throw std::runtime_error("Invalid zero MIDI tempo.");
                    }
                    mTempos.push_back({ tick, tempo });
                }
                if (type == 0x58) {
                    if (data.size() != 4 || !data[0]) {
                        throw std::runtime_error("Invalid MIDI time signature.");
                    }
                    if (tick == 0 && data[1] <= 6) {
                        mResult.numerator = data[0];
                        mResult.denominator = 1 << data[1];
                    }
                }
                continue;
            }
            if (status == 0xf0 || status == 0xf7) {
                running = 0;
                input.take(input.variable());
                mResult.hasSystemExclusive = true;
                continue;
            }
            if (status >= 0xf0) {
                throw std::runtime_error("Invalid MIDI file event status.");
            }
            running = status;
            const auto type = status & 0xf0;
            const int a = first < 0 ? input.byte() : first;
            const int b = type == 0xc0 || type == 0xd0 ? 0 : input.byte();
            if (a >= 128 || b >= 128) {
                throw std::runtime_error("Invalid MIDI channel message data.");
            }
            auto& part = track.parts[status & 0x0f];
            if (type == 0x90 && b) {
                part.held[a].push_back(part.notes.size());
                part.notes.push_back({ tick, tick + 1, a, b });
            } else if (type == 0x80 || type == 0x90) {
                auto found = part.held.find(a);
                if (found != part.held.end() && !found->second.empty()) {
                    auto& note = part.notes[found->second.front()];
                    note.end = std::max(tick, note.start + 1);
                    found->second.pop_front();
                }
            } else {
                MidiControllerData event;
                event.type = static_cast<MidiControllerData::Type>(type);
                event.number = type == 0xb0 || type == 0xa0 ? a : 0;
                event.value = type == 0xe0 ? a | (b << 7) : type == 0xb0 || type == 0xa0 ? b : a;
                part.controllers.push_back({ tick, event });
            }
        }
        if (!ended) {
            throw std::runtime_error("The MIDI track has no end-of-track event.");
        }
        track.end = tick;
        for (auto& [channel, part] : track.parts) {
            for (const auto& [pitch, held] : part.held) {
                for (auto index : held) {
                    auto& note = part.notes[index];
                    note.end = std::max(tick, note.start + 1);
                }
            }
            for (const auto& note : part.notes) {
                track.end = std::max(track.end, note.end);
            }
            part.held.clear();
        }
        return track;
    }

    unsigned mDivision = 0;
    size_t mEvents = 0;
    std::vector<Track> mTracks;
    std::vector<Tempo> mTempos;
    MidiFileData mResult;
};
}

MidiFileData ReadMidiFile(std::span<const uint8_t> bytes, double projectTempo)
{
    try {
        return Parser().read(bytes, projectTempo);
    } catch (const std::exception& error) {
        MidiFileData result;
        result.error = wxString::FromUTF8(error.what());
        return result;
    }
}
