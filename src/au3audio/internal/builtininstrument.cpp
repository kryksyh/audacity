/*
* Audacity: A Digital Audio Editor
*/
#include "instrumentprocessor.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <numbers>
#include <QJsonDocument>
#include <QJsonObject>

namespace au::au3audio {
namespace {
constexpr size_t parameterCount = 15;
constexpr double tau = std::numbers::pi * 2.0;

class BuiltinInstrument final : public InstrumentProcessor
{
public:
    explicit BuiltinInstrument(const std::string& state)
    {
        m_values[0] = 0.5;
        m_values[1] = 0.01;
        m_values[2] = 0.1;
        for (size_t oscillator = 0; oscillator < 4; ++oscillator) {
            m_values[3 + oscillator * 3] = oscillator == 0 ? 0.5 : 0.0;
            m_values[4 + oscillator * 3] = 0.0;
            m_values[5 + oscillator * 3] = 0.5;
        }
        const auto object = QJsonDocument::fromJson(QByteArray::fromStdString(state)).object();
        for (size_t index = 0; index < parameterCount; ++index) {
            m_ids[index] = std::to_string(index);
            const auto value = object.value(QString::number(index));
            if (value.isDouble() && value.toDouble() >= 0.0 && value.toDouble() <= 1.0) {
                m_values[index] = value.toDouble();
            }
            m_processingValues[index] = m_values[index].load();
        }
    }

    std::vector<audio::InstrumentParameter> parameters() const override
    {
        std::vector<audio::InstrumentParameter> result;
        for (size_t index = 0; index < parameterCount; ++index) {
            std::string name;
            if (index < 3) {
                name = std::array { "Volume", "Attack", "Release" }[index];
            } else {
                name = "Oscillator " + std::to_string((index - 3) / 3 + 1) + " "
                       + std::array { "level", "waveform", "detune" }[(index - 3) % 3];
            }
            const double value = m_values[index].load(std::memory_order_relaxed);
            const auto display = index >= 3 && (index - 3) % 3 == 1
                                 ? std::string(std::array { "Sine", "Triangle", "Saw", "Square" }[std::min(3, int(value * 4))])
                                 : QString::number(value, 'f', 3).toStdString();
            result.push_back({ std::to_string(index), name, display, value });
        }
        return result;
    }

    bool setParameter(const std::string& id, double value) override
    {
        if (!std::isfinite(value) || value < 0.0 || value > 1.0) {
            return false;
        }
        for (size_t index = 0; index < parameterCount; ++index) {
            if (id == m_ids[index]) {
                m_values[index].store(value, std::memory_order_relaxed);
                m_processingValues[index].store(value, std::memory_order_relaxed);
                m_changed = true;
                return true;
            }
        }
        return false;
    }

    bool setAutomatedParameter(const std::string& id, double value) override
    {
        if (!std::isfinite(value) || value < 0.0 || value > 1.0) {
            return false;
        }
        for (size_t index = 0; index < parameterCount; ++index) {
            if (id == m_ids[index]) {
                m_processingValues[index].store(value, std::memory_order_relaxed);
                return true;
            }
        }
        return false;
    }

    std::string state() const override
    {
        QJsonObject object;
        for (size_t index = 0; index < parameterCount; ++index) {
            object.insert(QString::number(index), m_values[index].load(std::memory_order_relaxed));
        }
        return QJsonDocument(object).toJson(QJsonDocument::Compact).toStdString();
    }

    bool takeChanges() override { return m_changed.exchange(false); }
    void restoreManualParameters() noexcept override
    {
        for (size_t i = 0; i < parameterCount; ++i) {
            m_processingValues[i] = m_values[i].load();
        }
    }

    bool restoreManualParameter(const std::string& id) noexcept override
    {
        for (size_t i = 0; i < parameterCount; ++i) {
            if (id == m_ids[i]) {
                m_processingValues[i] = m_values[i].load();
                return true;
            }
        }
        return false;
    }

    void prepare(double sampleRate, size_t) override { m_sampleRate = sampleRate; reset(); }
    void reset() override
    {
        m_voices = {};
        m_sustain = {};
        m_pitch = {};
        m_expression.fill(1.0);
        m_volume.fill(1.0);
    }

    void process(float* const* output, size_t channels, size_t frames, std::span<const MidiEvent> events,
                 const InstrumentProcessContext&) override
    {
        std::array<double, parameterCount> parameters;
        for (size_t index = 0; index < parameterCount; ++index) {
            parameters[index] = m_processingValues[index].load(std::memory_order_relaxed);
        }
        size_t eventIndex = 0;
        for (size_t sample = 0; sample < frames; ++sample) {
            while (eventIndex < events.size() && events[eventIndex].sample <= static_cast<int64_t>(sample)) {
                receive(events[eventIndex++]);
            }
            double value = 0.0;
            for (auto& voice : m_voices) {
                if (!voice.active) {
                    continue;
                }
                const double attack = std::max(0.001, parameters[1] * 2.0);
                const double release = std::max(0.005, parameters[2] * 4.0);
                voice.level = voice.held || m_sustain[voice.channel]
                              ? std::min(1.0, voice.level + 1.0 / (attack * m_sampleRate))
                              : std::max(0.0, voice.level - 1.0 / (release * m_sampleRate));
                if (voice.level == 0.0) {
                    voice.active = false;
                    continue;
                }
                for (size_t oscillator = 0; oscillator < 4; ++oscillator) {
                    const auto offset = 3 + oscillator * 3;
                    const double frequency = 440.0 * std::exp2((voice.note - 69.0 + m_pitch[voice.channel] * 2.0
                                                                + (parameters[offset + 2] - 0.5) * 2.0) / 12.0);
                    const double step = std::min(0.49, frequency / m_sampleRate);
                    auto& phase = voice.phase[oscillator];
                    const int waveform = std::min(3, int(parameters[offset + 1] * 4.0));
                    double wave = std::sin(phase * tau);
                    if (waveform == 1) {
                        wave = 1.0 - 4.0 * std::abs(phase - 0.5);
                    } else if (waveform == 2) {
                        wave = phase * 2.0 - 1.0 - polyBlep(phase, step);
                    } else if (waveform == 3) {
                        wave = (phase < 0.5 ? 1.0 : -1.0) + polyBlep(phase, step)
                               - polyBlep(std::fmod(phase + 0.5, 1.0), step);
                    }
                    value += wave * parameters[offset] * voice.velocity * voice.level * m_expression[voice.channel]
                             * m_volume[voice.channel];
                    phase += step;
                    phase -= std::floor(phase);
                }
            }
            const float result = static_cast<float>(value * parameters[0] * 0.25);
            for (size_t channel = 0; channel < channels; ++channel) {
                output[channel][sample] = result;
            }
        }
    }

private:
    static double polyBlep(double phase, double step)
    {
        if (phase < step) {
            const double x = phase / step;
            return x + x - x * x - 1.0;
        }
        if (phase > 1.0 - step) {
            const double x = (phase - 1.0) / step;
            return x * x + x + x + 1.0;
        }
        return 0.0;
    }

    void receive(const MidiEvent& event)
    {
        const auto channel = event.status & 0xf;
        const auto type = event.status & 0xf0;
        if (type == 0x90 && event.data2 > 0) {
            auto* voice = &m_voices[0];
            for (auto& candidate : m_voices) {
                if (!candidate.active) {
                    voice = &candidate;
                    break;
                }
                if (candidate.age < voice->age) {
                    voice = &candidate;
                }
            }
            *voice = { true, true, channel, event.data1, event.data2 / 127.0, 0.0, ++m_age, {} };
        } else if (type == 0x80 || type == 0x90) {
            for (auto& voice : m_voices) {
                if (voice.active && voice.channel == channel && voice.note == event.data1) {
                    voice.held = false;
                }
            }
        } else if (type == 0xb0) {
            if (event.data1 == 64) {
                m_sustain[channel] = event.data2 >= 64;
            } else if (event.data1 == 11) {
                m_expression[channel] = event.data2 / 127.0;
            } else if (event.data1 == 7) {
                m_volume[channel] = event.data2 / 127.0;
            } else if (event.data1 == 120 || event.data1 >= 123) {
                for (auto& voice : m_voices) {
                    if (voice.channel == channel) {
                        voice.held = false;
                        voice.active &= event.data1 != 120;
                    }
                }
            } else if (event.data1 == 121) {
                m_sustain[channel] = false;
                m_expression[channel] = 1.0;
                m_pitch[channel] = 0.0;
            }
        } else if (type == 0xe0) {
            m_pitch[channel] = ((event.data2 * 128 + event.data1) - 8192.0) / 8192.0;
        }
    }

    struct Voice {
        bool active = false;
        bool held = false;
        int channel = 0;
        int note = 0;
        double velocity = 0.0;
        double level = 0.0;
        uint64_t age = 0;
        std::array<double, 4> phase {};
    };
    std::array<Voice, 128> m_voices {};
    std::array<bool, 16> m_sustain {};
    std::array<double, 16> m_pitch {};
    std::array<double, 16> m_expression {}, m_volume {};
    std::array<std::atomic<double>, parameterCount> m_values {};
    std::array<std::atomic<double>, parameterCount> m_processingValues {};
    std::array<std::string, parameterCount> m_ids;
    std::atomic<bool> m_changed { false };
    double m_sampleRate = 44100.0;
    uint64_t m_age = 0;
};
}

std::shared_ptr<InstrumentProcessor> createBuiltinInstrument(const std::string& state)
{
    return std::make_shared<BuiltinInstrument>(state);
}
}
