/*
* Audacity: A Digital Audio Editor
*/
#include "instrumentprocessor.h"
#include <CoreAudio/HostTime.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <limits>
#include <stdexcept>

#include <QJsonDocument>
#include <QJsonObject>
#include <QStringList>
#import <Cocoa/Cocoa.h>
#import <AudioUnit/AUCocoaUIView.h>
#include <AudioToolbox/AudioUnitUtilities.h>
#include <AudioToolbox/MusicDevice.h>

#include "au3-audio-unit/AudioUnitWrapper.h"

namespace au::au3audio {
namespace {
AudioComponent findComponent(const std::string& path)
{
    const auto fields = QString::fromStdString(path).split('/');
    if (fields.size() < 3) {
        throw std::runtime_error("Invalid Audio Unit instrument path.");
    }
    const auto code = [](const QString& text) {
        const auto bytes = text.toLatin1();
        if (bytes.size() != 4) {
            throw std::runtime_error("Invalid Audio Unit component identifier.");
        }
        OSType value = 0;
        for (const auto byte : bytes) {
            value = (value << 8) | static_cast<unsigned char>(byte);
        }
        return value;
    };
    AudioComponentDescription description {};
    description.componentManufacturer = code(fields[0]);
    description.componentType = code(fields[1]);
    description.componentSubType = code(fields[2]);
    const auto component = AudioComponentFindNext(nullptr, &description);
    if (!component) {
        throw std::runtime_error("The Audio Unit instrument is not available.");
    }
    return component;
}

class InstrumentUnit final : public AudioUnitWrapper
{
public:
    explicit InstrumentUnit(AudioComponent component)
        : AudioUnitWrapper(component, nullptr)
    {
        mAudioIns = 0;
        mAudioOuts = 2;
        if (!CreateAudioUnit()) {
            throw std::runtime_error("Cannot create the Audio Unit instrument.");
        }
    }

    unsigned channels() const { return mAudioOuts; }
};

class AudioUnitInstrument final : public InstrumentProcessor
{
public:
    AudioUnitInstrument(const std::string& path, const std::string& state)
        : m_host(findComponent(path)), m_unit(m_host.GetAudioUnit())
    {
        const auto saved = QJsonDocument::fromJson(QByteArray::fromStdString(state)).object();
        const auto blob = QByteArray::fromBase64(saved.value("auState").toString().toLatin1());
        if (!blob.isEmpty()) {
            auto data = CFDataCreate(nullptr, reinterpret_cast<const UInt8*>(blob.constData()), blob.size());
            auto object = CFPropertyListCreateWithData(nullptr, data, kCFPropertyListImmutable, nullptr, nullptr);
            CFRelease(data);
            if (!object || AudioUnitSetProperty(m_unit, kAudioUnitProperty_ClassInfo, kAudioUnitScope_Global, 0,
                                                &object, sizeof(object)) != noErr) {
                if (object) {
                    CFRelease(object);
                }
                throw std::runtime_error("Cannot restore the Audio Unit instrument state.");
            }
            CFRelease(object);
        }
        m_host.ForEachParameter([this](const auto& parameter, AudioUnitParameterID id) {
            if (parameter.mInfo.maxValue <= parameter.mInfo.minValue
                || (parameter.mInfo.flags & kAudioUnitParameterFlag_IsWritable) == 0) {
                return true;
            }
            AudioUnitParameterValue value = parameter.mInfo.defaultValue;
            AudioUnitGetParameter(m_unit, id, kAudioUnitScope_Global, 0, &value);
            m_parameters.push_back({ id, parameter.mName ? parameter.mName->utf8_string() : std::to_string(id),
                                     parameter.mInfo.minValue, parameter.mInfo.maxValue, value, std::to_string(id) });
            return true;
        });
        m_values = std::make_unique<std::atomic<double>[]>(m_parameters.size());
        m_processingValues = std::make_unique<std::atomic<double>[]>(m_parameters.size());
        m_eventFence = std::make_unique<std::atomic<UInt64>[]>(m_parameters.size());
        m_dirty = std::make_unique<std::atomic<bool>[]>(m_parameters.size());
        m_lastApplied.assign(m_parameters.size(), std::numeric_limits<double>::quiet_NaN());
        const auto values = saved.value("parameters").toObject();
        for (size_t index = 0; index < m_parameters.size(); ++index) {
            const auto& parameter = m_parameters[index];
            const auto value = values.value(QString::number(parameter.id));
            m_values[index] = value.isDouble() ? std::clamp(value.toDouble(), 0.0, 1.0)
                              : normalized(index, parameter.initial);
            m_processingValues[index] = m_values[index].load();
            m_eventFence[index] = AudioGetCurrentHostTime();
            m_dirty[index] = true;
        }
        AUEventListenerCreate(&parameterChanged, this, CFRunLoopGetMain(), kCFRunLoopDefaultMode, 0.02f, 0.02f, &m_listener);
        if (m_listener) {
            for (const auto& parameter : m_parameters) {
                AudioUnitEvent event {};
                event.mEventType = kAudioUnitEvent_ParameterValueChange;
                event.mArgument.mParameter = { m_unit, parameter.id, kAudioUnitScope_Global, 0 };
                AUEventListenerAddEventType(m_listener, this, &event);
            }
        }
        m_latencyListener = AudioUnitAddPropertyListener(m_unit, kAudioUnitProperty_Latency, &latencyChanged, this) == noErr;
    }

    ~AudioUnitInstrument() override
    {
        detachView();
        if (m_latencyListener) {
            AudioUnitRemovePropertyListenerWithUserData(m_unit, kAudioUnitProperty_Latency, &latencyChanged, this);
        }
        if (m_listener) {
            AUListenerDispose(m_listener);
        }
        if (m_prepared) {
            AudioUnitUninitialize(m_unit);
        }
    }

    std::vector<audio::InstrumentParameter> parameters() const override
    {
        std::vector<audio::InstrumentParameter> result;
        for (size_t index = 0; index < m_parameters.size(); ++index) {
            const auto value = m_values[index].load();
            result.push_back({ std::to_string(m_parameters[index].id), m_parameters[index].name,
                               QString::number(plain(index, value), 'g', 6).toStdString(), value });
        }
        return result;
    }

    bool setParameter(const std::string& id, double value) override
    {
        if (!std::isfinite(value) || value < 0.0 || value > 1.0) {
            return false;
        }
        for (size_t index = 0; index < m_parameters.size(); ++index) {
            if (id == m_parameters[index].key) {
                m_values[index] = value;
                m_processingValues[index] = value;
                m_eventFence[index] = AudioGetCurrentHostTime();
                m_dirty[index] = true;
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
        for (size_t index = 0; index < m_parameters.size(); ++index) {
            if (id == m_parameters[index].key) {
                m_processingValues[index] = value;
                m_dirty[index] = true;
                return true;
            }
        }
        return false;
    }

    std::string state() const override
    {
        CFPropertyListRef object = nullptr;
        UInt32 size = sizeof(object);
        QByteArray blob;
        if (AudioUnitGetProperty(m_unit, kAudioUnitProperty_ClassInfo, kAudioUnitScope_Global, 0, &object, &size) == noErr && object) {
            auto data = CFPropertyListCreateData(nullptr, object, kCFPropertyListBinaryFormat_v1_0, 0, nullptr);
            CFRelease(object);
            if (data) {
                blob = QByteArray(reinterpret_cast<const char*>(CFDataGetBytePtr(data)), CFDataGetLength(data)).toBase64();
                CFRelease(data);
            }
        }
        QJsonObject values;
        for (size_t index = 0; index < m_parameters.size(); ++index) {
            values.insert(QString::number(m_parameters[index].id), m_values[index].load());
        }
        return QJsonDocument(QJsonObject { { "auState", QString::fromLatin1(blob) }, { "parameters", values } })
               .toJson(QJsonDocument::Compact).toStdString();
    }

    bool takeChanges() override { return m_changed.exchange(false); }
    void restoreManualParameters() noexcept override
    {
        for (size_t i = 0; i < m_parameters.size(); ++i) {
            m_processingValues[i] = m_values[i].load();
            m_dirty[i] = true;
        }
    }

    bool restoreManualParameter(const std::string& id) noexcept override
    {
        for (size_t i = 0; i < m_parameters.size(); ++i) {
            if (id == m_parameters[i].key) {
                m_processingValues[i] = m_values[i].load();
                m_dirty[i] = true;
                return true;
            }
        }
        return false;
    }

    size_t latency() const override { return m_latency.load(std::memory_order_acquire); }
    size_t reportedLatency() const override
    {
        Float64 latency = 0.0;
        m_host.GetFixedSizeProperty(kAudioUnitProperty_Latency, latency);
        const auto samples = std::max(0.0, latency * m_rate);
        if (!std::isfinite(latency) || samples >= static_cast<double>(std::numeric_limits<int64_t>::max())) {
            throw std::runtime_error("The Audio Unit instrument reported an invalid latency.");
        }
        return static_cast<size_t>(samples);
    }

    bool latencyUpdatePending() const noexcept override { return m_latencyPending.load(std::memory_order_acquire); }
    bool requiresMainThreadLatencyRefresh() const noexcept override { return true; }
    bool refreshLatency() override
    {
        // Offline probing also explicitly measures units which omit their
        // latency notification. A new callback during this query stays pending.
        m_latencyPending.exchange(false, std::memory_order_acq_rel);
        const auto samples = reportedLatency();
        return m_latency.exchange(samples, std::memory_order_acq_rel) != samples;
    }

    void prepare(double sampleRate, size_t maximumFrames) override
    {
        if (m_prepared) {
            AudioUnitUninitialize(m_unit);
            m_prepared = false;
        }
        const auto maximum = static_cast<UInt32>(maximumFrames);
        if (!m_host.SetRateAndChannels(sampleRate, "Audacity instrument")
            || m_host.SetProperty(kAudioUnitProperty_MaximumFramesPerSlice, maximum) != noErr) {
            throw std::runtime_error("The Audio Unit instrument does not support the audio format.");
        }
        HostCallbackInfo callbacks {};
        callbacks.hostUserData = this;
        callbacks.beatAndTempoProc = &beatAndTempo;
        callbacks.musicalTimeLocationProc = &musicalTime;
        callbacks.transportStateProc = &transportState;
        m_host.SetProperty(kAudioUnitProperty_HostCallbacks, callbacks);
        if (AudioUnitInitialize(m_unit) != noErr) {
            throw std::runtime_error("Cannot initialize the Audio Unit instrument for playback.");
        }
        m_rate = sampleRate;
        m_prepared = true;
        m_latencyPending.store(true, std::memory_order_release);
        refreshLatency();
        for (size_t channel = 0; channel < 2; ++channel) {
            m_buffers[channel].assign(maximumFrames, 0.0f);
        }
        reset();
    }

    void reset() override { m_reset = true; }

    void process(float* const* output, size_t channels, size_t frames, std::span<const MidiEvent> events,
                 const InstrumentProcessContext& context) override
    {
        m_discontinuous = m_reset || m_nextSample != context.sample || m_context.playing != context.playing
                          || m_context.cycling != context.cycling || m_context.cycleStart != context.cycleStart
                          || m_context.cycleEnd != context.cycleEnd || m_context.tempo != context.tempo
                          || m_context.numerator != context.numerator || m_context.denominator != context.denominator;
        m_context = context;
        if (m_reset) {
            std::fill(m_lastApplied.begin(), m_lastApplied.end(), std::numeric_limits<double>::quiet_NaN());
            AudioUnitReset(m_unit, kAudioUnitScope_Global, 0);
            for (int channel = 0; channel < 16; ++channel) {
                MusicDeviceMIDIEvent(m_unit, 0xb0 | channel, 64, 0, 0);
                MusicDeviceMIDIEvent(m_unit, 0xb0 | channel, 120, 0, 0);
                MusicDeviceMIDIEvent(m_unit, 0xb0 | channel, 123, 0, 0);
            }
            // Reset can enqueue notifications for old/default parameter values.
            // They must not overwrite a newer host edit when delivered later.
            const auto fence = AudioGetCurrentHostTime();
            for (size_t i = 0; i < m_parameters.size(); ++i) {
                m_eventFence[i] = fence;
            }
            m_reset = false;
        }
        for (size_t index = 0; index < m_parameters.size(); ++index) {
            if (m_dirty[index].exchange(false)) {
                const auto value = m_processingValues[index].load();
                if (value != m_lastApplied[index]) {
                    m_latencyPending.store(true, std::memory_order_release);
                }
                m_lastApplied[index] = value;
                AudioUnitSetParameter(m_unit, m_parameters[index].id, kAudioUnitScope_Global, 0,
                                      static_cast<AudioUnitParameterValue>(plain(index, value)), 0);
            }
        }
        for (const auto& event : events) {
            MusicDeviceMIDIEvent(m_unit, event.status, event.data1, event.data2, static_cast<UInt32>(event.sample));
        }
        alignas(AudioBufferList) std::byte storage[sizeof(AudioBufferList) + sizeof(AudioBuffer)] {};
        auto* buffers = reinterpret_cast<AudioBufferList*>(storage);
        buffers->mNumberBuffers = m_host.channels();
        for (UInt32 channel = 0; channel < buffers->mNumberBuffers; ++channel) {
            buffers->mBuffers[channel] = { 1, static_cast<UInt32>(frames * sizeof(float)), m_buffers[channel].data() };
        }
        AudioTimeStamp timestamp {};
        timestamp.mFlags = kAudioTimeStampSampleTimeValid;
        timestamp.mSampleTime = context.renderSample.value_or(context.continuousSample.value_or(context.sample));
        AudioUnitRenderActionFlags flags = 0;
        const auto result = AudioUnitRender(m_unit, &flags, &timestamp, 0, static_cast<UInt32>(frames), buffers);
        for (size_t channel = 0; channel < channels; ++channel) {
            if (result == noErr) {
                std::copy_n(m_buffers[std::min<size_t>(channel, m_host.channels() - 1)].data(), frames, output[channel]);
            } else {
                std::fill_n(output[channel], frames, 0.0f);
            }
        }
        m_nextSample = context.sample + (context.playing ? frames : 0);
    }

    bool attachView(void* parent, int x, int y, int, int) override
    {
        auto* parentView = (__bridge NSView*)parent;
        if (!parentView) {
            return false;
        }
        if (!m_view) {
            PackedArray::Ptr<AudioUnitCocoaViewInfo> info;
            if (m_host.GetVariableSizeProperty(kAudioUnitProperty_CocoaUI, info) != noErr || !info) {
                return false;
            }
            auto* url = (__bridge NSURL*)info->mCocoaAUViewBundleLocation;
            auto* name = (__bridge NSString*)info->mCocoaAUViewClass[0];
            auto* bundle = url ? [NSBundle bundleWithURL:url] : nil;
            Class factoryClass = name ? [bundle classNamed:name] : Nil;
            if (!factoryClass) {
                return false;
            }
            m_factory = [[factoryClass alloc] init];
            if (![m_factory conformsToProtocol:@protocol(AUCocoaUIBase)]) {
                m_factory = nil;
                return false;
            }
            m_view = [(id<AUCocoaUIBase>)m_factory uiViewForAudioUnit:m_unit withSize:NSMakeSize(800, 600)];
            if (!m_view) {
                return false;
            }
        }
        if (m_view.superview != parentView) {
            [m_view removeFromSuperview];
            [parentView addSubview:m_view];
        }
        const auto size = m_view.frame.size;
        const auto top = parentView.isFlipped ? y : parentView.bounds.size.height - y - size.height;
        [m_view setFrameOrigin:NSMakePoint(x, top)];
        return true;
    }

    void detachView() override
    {
        [m_view removeFromSuperview];
        m_view = nil;
        m_factory = nil;
    }

    audio::InstrumentEditorSize editorSize() const override
    {
        return m_view ? audio::InstrumentEditorSize { static_cast<int>(m_view.frame.size.width),
                                                      static_cast<int>(m_view.frame.size.height) }
               : audio::InstrumentEditorSize {};
    }

private:
    static void latencyChanged(void* data, AudioUnit, AudioUnitPropertyID property, AudioUnitScope, AudioUnitElement)
    {
        if (property == kAudioUnitProperty_Latency) {
            static_cast<AudioUnitInstrument*>(data)->m_latencyPending.store(true, std::memory_order_release);
        }
    }

    double normalized(size_t index, double value) const
    {
        const auto& parameter = m_parameters[index];
        return std::clamp((value - parameter.minimum) / (parameter.maximum - parameter.minimum), 0.0, 1.0);
    }

    double plain(size_t index, double value) const
    {
        const auto& parameter = m_parameters[index];
        return parameter.minimum + value * (parameter.maximum - parameter.minimum);
    }

    static void parameterChanged(void* data, void*, const AudioUnitEvent* event, UInt64 hostTime, AudioUnitParameterValue value)
    {
        auto& self = *static_cast<AudioUnitInstrument*>(data);
        if (event->mEventType != kAudioUnitEvent_ParameterValueChange) {
            return;
        }
        for (size_t index = 0; index < self.m_parameters.size(); ++index) {
            if (self.m_parameters[index].id == event->mArgument.mParameter.mParameterID) {
                if (hostTime && hostTime <= self.m_eventFence[index].load()) {
                    return;
                }
                if (!hostTime) {
                    AudioUnitParameterValue current = 0;
                    if (AudioUnitGetParameter(self.m_unit, self.m_parameters[index].id, kAudioUnitScope_Global, 0, &current) != noErr
                        || current != value) {
                        return;
                    }
                }
                const auto normalized = self.normalized(index, value);
                // A plug-in may echo processing automation through notifications.
                if (std::abs(normalized - self.m_processingValues[index].load()) < 1e-7
                    || std::abs(normalized - self.m_values[index].load()) < 1e-7) {
                    return;
                }
                self.m_values[index] = normalized;
                self.m_processingValues[index] = self.m_values[index].load();
                self.m_dirty[index] = true;
                self.m_changed = true;
                break;
            }
        }
    }

    static OSStatus beatAndTempo(void* data, Float64* beat, Float64* tempo)
    {
        const auto& self = *static_cast<AudioUnitInstrument*>(data);
        if (beat) {
            *beat = self.m_context.sample / self.m_rate * self.m_context.tempo / 60.0;
        }
        if (tempo) {
            *tempo = self.m_context.tempo;
        }
        return noErr;
    }

    static OSStatus musicalTime(void* data, UInt32* nextBeat, Float32* numerator, UInt32* denominator, Float64* downbeat)
    {
        const auto& self = *static_cast<AudioUnitInstrument*>(data);
        const double beat = self.m_context.sample / self.m_rate * self.m_context.tempo / 60.0;
        const double beatsPerMeasure = self.m_context.numerator * 4.0 / self.m_context.denominator;
        if (nextBeat) {
            *nextBeat = static_cast<UInt32>((std::ceil(beat) - beat) * 60.0 / self.m_context.tempo * self.m_rate);
        }
        if (numerator) {
            *numerator = self.m_context.numerator;
        }
        if (denominator) {
            *denominator = self.m_context.denominator;
        }
        if (downbeat) {
            *downbeat = std::floor(beat / beatsPerMeasure) * beatsPerMeasure;
        }
        return noErr;
    }

    static OSStatus transportState(void* data, Boolean* playing, Boolean* changed, Float64* sample,
                                   Boolean* cycling, Float64* cycleStart, Float64* cycleEnd)
    {
        const auto& self = *static_cast<AudioUnitInstrument*>(data);
        if (playing) {
            *playing = self.m_context.playing;
        }
        if (changed) {
            *changed = self.m_discontinuous;
        }
        if (sample) {
            *sample = self.m_context.sample;
        }
        if (cycling) {
            *cycling = self.m_context.cycling;
        }
        if (cycleStart) {
            *cycleStart = self.m_context.cycleStart * self.m_context.tempo / 60.0;
        }
        if (cycleEnd) {
            *cycleEnd = self.m_context.cycleEnd * self.m_context.tempo / 60.0;
        }
        return noErr;
    }

    struct Parameter {
        AudioUnitParameterID id;
        std::string name;
        double minimum;
        double maximum;
        double initial;
        std::string key;
    };
    InstrumentUnit m_host;
    AudioUnit m_unit;
    std::vector<Parameter> m_parameters;
    std::unique_ptr<std::atomic<double>[]> m_values;
    std::unique_ptr<std::atomic<double>[]> m_processingValues;
    std::unique_ptr<std::atomic<UInt64>[]> m_eventFence;
    std::unique_ptr<std::atomic<bool>[]> m_dirty;
    std::vector<double> m_lastApplied;
    std::vector<float> m_buffers[2];
    AUEventListenerRef m_listener = nullptr;
    std::atomic<bool> m_changed { false };
    std::atomic<size_t> m_latency { 0 };
    std::atomic<bool> m_latencyPending { false };
    bool m_latencyListener = false;
    __strong NSView* m_view = nil;
    __strong NSObject* m_factory = nil;
    InstrumentProcessContext m_context;
    double m_rate = 44100.0;
    int64_t m_nextSample = -1;
    bool m_prepared = false;
    bool m_reset = true;
    bool m_discontinuous = true;
};
}

std::shared_ptr<InstrumentProcessor> createAudioUnitInstrument(const std::string& path, const std::string& state)
{
    return std::make_shared<AudioUnitInstrument>(path, state);
}
}
