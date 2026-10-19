/*
* Audacity: A Digital Audio Editor
*/
#include "portaudiodevice.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <optional>

#include <portaudio.h>

#if USE_PORTMIXER
#include <portmixer.h>
#endif

#include "au3-audio-devices/AudioIOBase.h"
#include "au3-audio-devices/DeviceManager.h"

namespace au::tracktionaudio {
namespace {
struct Device
{
    int index = paNoDevice;
    int source = 0;
    juce::String name;
};

std::vector<Device> devicesForHost(int hostIndex, bool input)
{
    const auto& maps = input ? DeviceManager::Instance()->GetInputDeviceMaps() : DeviceManager::Instance()->GetOutputDeviceMaps();
    std::vector<Device> devices;
    for (const auto& item : maps) {
        if (item.hostIndex == hostIndex) {
            devices.push_back({ item.deviceIndex, item.sourceIndex, juce::String(MakeDeviceSourceString(&item, maps)) });
        }
    }
    return devices;
}

class PortAudioDevice final : public juce::AudioIODevice
{
public:
    PortAudioDevice(const juce::String& type, std::optional<Device> input, std::optional<Device> output,
                    const juce::CriticalSection& callbackLock, std::shared_ptr<AudioDeviceCallback> callback)
        : AudioIODevice(output ? output->name : input->name, type), m_input(std::move(input)), m_output(std::move(output)),
        m_callbackLock(callbackLock), m_callback(std::move(callback))
    {
    }

    ~PortAudioDevice() override { close(); }

    juce::StringArray getOutputChannelNames() override { return channelNames(false); }
    juce::StringArray getInputChannelNames() override { return channelNames(true); }

    juce::Array<double> getAvailableSampleRates() override
    {
        juce::Array<double> rates;
        const auto input = parameters(m_input, m_input ? 1 : 0);
        const auto output = parameters(m_output, m_output ? std::min(2, channelCount(false)) : 0);
        for (int index = 0; index < AudioIOBase::NumStandardRates; ++index) {
            const double rate = AudioIOBase::StandardRates[index];
            if (Pa_IsFormatSupported(input ? &*input : nullptr, output ? &*output : nullptr, rate) == paFormatIsSupported) {
                rates.add(rate);
            }
        }
        for (const auto& device : { m_input, m_output }) {
            const auto* info = device ? Pa_GetDeviceInfo(device->index) : nullptr;
            if (info && !rates.contains(info->defaultSampleRate)
                && Pa_IsFormatSupported(input ? &*input : nullptr, output ? &*output : nullptr,
                                        info->defaultSampleRate) == paFormatIsSupported) {
                rates.add(info->defaultSampleRate);
            }
        }
        rates.sort();
        return rates;
    }

    juce::Array<int> getAvailableBufferSizes() override
    {
        return { 32, 64, 128, 256, 512, 1024, 2048, 4096, 8192 };
    }

    int getDefaultBufferSize() override { return 512; }

    juce::String open(const juce::BigInteger& inputChannels, const juce::BigInteger& outputChannels,
                      double sampleRate, int bufferSize) override
    {
        close();
        m_error.clear();
        const int inputs = inputChannels.getHighestBit() + 1;
        const int outputs = outputChannels.getHighestBit() + 1;
        if (inputs > channelCount(true) || outputs > channelCount(false) || (inputs == 0 && outputs == 0)) {
            return m_error = "Invalid audio channel selection.";
        }
        if (sampleRate <= 0.0 || bufferSize <= 0) {
            return m_error = "Invalid audio sample rate or buffer size.";
        }

        const auto input = parameters(m_input, inputs);
        const auto output = parameters(m_output, outputs);
        if ((inputs > 0 && !input) || (outputs > 0 && !output)) {
            return m_error = "The audio device is no longer available.";
        }

        m_inputChannels = inputChannels;
        m_outputChannels = outputChannels;
        m_inputs = channelIndices(inputChannels);
        m_outputs = channelIndices(outputChannels);
        m_inputPointers.resize(m_inputs.size());
        m_outputPointers.resize(m_outputs.size());
        m_physicalOutputs = outputs;
        m_sampleRate = sampleRate;
        m_bufferSize = bufferSize;

        const auto error = Pa_OpenStream(&m_stream, input ? &*input : nullptr, output ? &*output : nullptr,
                                         sampleRate, bufferSize, paClipOff | paDitherOff, callback, this);
        if (error != paNoError) {
            m_stream = nullptr;
            return m_error = Pa_GetErrorText(error);
        }

        const auto* info = Pa_GetStreamInfo(m_stream);
        m_sampleRate = info->sampleRate;
        m_inputLatency = static_cast<int>(std::lround(info->inputLatency * m_sampleRate));
        m_outputLatency = static_cast<int>(std::lround(info->outputLatency * m_sampleRate));
#if USE_PORTMIXER
        m_mixer = Px_OpenMixer(m_stream, m_input ? m_input->index : paNoDevice, m_output ? m_output->index : paNoDevice, 0);
        if (m_mixer && m_input && m_input->source >= 0 && m_input->source < Px_GetNumInputSources(m_mixer)) {
            Px_SetCurrentInputSource(m_mixer, m_input->source);
        }
#if JUCE_MAC
        if (m_mixer && Px_SupportsPlaythrough(m_mixer)) {
            m_previousPlaythrough = Px_GetPlaythrough(m_mixer);
            Px_SetPlaythrough(m_mixer, 0.0f);
        }
#endif
#endif
        return {};
    }

    void close() override
    {
        stop();
#if USE_PORTMIXER
        if (m_mixer) {
#if JUCE_MAC
            if (m_previousPlaythrough >= 0.0f) {
                Px_SetPlaythrough(m_mixer, m_previousPlaythrough);
            }
            m_previousPlaythrough = -1.0f;
#endif
            Px_CloseMixer(m_mixer);
            m_mixer = nullptr;
        }
#endif
        if (m_stream) {
            Pa_CloseStream(m_stream);
            m_stream = nullptr;
        }
    }

    bool isOpen() override { return m_stream != nullptr; }

    void start(juce::AudioIODeviceCallback* receiver) override
    {
        stop();
        if (!m_stream || !receiver) {
            return;
        }

        if (m_callback) {
            m_callback->prepare(m_sampleRate, m_bufferSize, static_cast<int>(m_inputs.size()), static_cast<int>(m_outputs.size()));
        }
        receiver->audioDeviceAboutToStart(this);
        m_receiver.store(receiver, std::memory_order_release);
        const auto error = Pa_StartStream(m_stream);
        if (error != paNoError) {
            m_error = Pa_GetErrorText(error);
            m_receiver.store(nullptr, std::memory_order_release);
            receiver->audioDeviceStopped();
            receiver->audioDeviceError(m_error);
        }
    }

    void stop() override
    {
        if (m_stream && Pa_IsStreamActive(m_stream) > 0) {
            const auto error = Pa_StopStream(m_stream);
            if (error != paNoError) {
                Pa_AbortStream(m_stream);
            }
        }
        auto* receiver = m_receiver.exchange(nullptr, std::memory_order_acq_rel);
        if (receiver) {
            receiver->audioDeviceStopped();
        }
    }

    bool isPlaying() override { return m_stream && Pa_IsStreamActive(m_stream) > 0; }
    juce::String getLastError() override { return m_error; }
    int getCurrentBufferSizeSamples() override { return m_bufferSize; }
    double getCurrentSampleRate() override { return m_sampleRate; }
    int getCurrentBitDepth() override { return 32; }
    juce::BigInteger getActiveOutputChannels() const override { return m_outputChannels; }
    juce::BigInteger getActiveInputChannels() const override { return m_inputChannels; }
    int getOutputLatencyInSamples() override { return m_outputLatency; }
    int getInputLatencyInSamples() override { return m_inputLatency; }

    const std::optional<Device>& device(bool input) const { return input ? m_input : m_output; }
    PaStream* stream() const { return m_stream; }

private:
    int channelCount(bool input) const
    {
        const auto& device = input ? m_input : m_output;
        const auto* info = device ? Pa_GetDeviceInfo(device->index) : nullptr;
        return info ? (input ? info->maxInputChannels : info->maxOutputChannels) : 0;
    }

    juce::StringArray channelNames(bool input) const
    {
        juce::StringArray names;
        if (m_stream) {
            // Tracktion indexes the channels supplied to the audio callback.
            for (const int channel : input ? m_inputs : m_outputs) {
                names.add(juce::String(channel + 1));
            }
            return names;
        }
        for (int channel = 0; channel < channelCount(input); ++channel) {
            names.add(juce::String(channel + 1));
        }
        return names;
    }

    static std::vector<int> channelIndices(const juce::BigInteger& channels)
    {
        std::vector<int> indices;
        for (int bit = channels.findNextSetBit(0); bit >= 0; bit = channels.findNextSetBit(bit + 1)) {
            indices.push_back(bit);
        }
        return indices;
    }

    static std::optional<PaStreamParameters> parameters(const std::optional<Device>& device, int channels)
    {
        const auto* info = device ? Pa_GetDeviceInfo(device->index) : nullptr;
        if (!info || channels == 0) {
            return {};
        }
        return PaStreamParameters { device->index, channels, paFloat32 | paNonInterleaved,
                                    AudioIOLatencyDuration.Read() / 1000.0, nullptr };
    }

    static int callback(const void* input, void* output, unsigned long frames, const PaStreamCallbackTimeInfo* times,
                        PaStreamCallbackFlags, void* context)
    {
        return static_cast<PortAudioDevice*>(context)->process(input, output, static_cast<int>(frames), *times);
    }

    int process(const void* input, void* output, int frames, const PaStreamCallbackTimeInfo& times)
    {
        const auto now = std::chrono::steady_clock::now();
        const double deviceTime = m_outputs.empty() ? times.inputBufferAdcTime : times.outputBufferDacTime;
        const auto nanos = static_cast<uint64_t>((juce::Time::highResolutionTicksToSeconds(juce::Time::getHighResolutionTicks())
                                                  + deviceTime - times.currentTime) * 1e9);
        const auto* inputs = static_cast<const float* const*>(input);
        auto* const* outputs = static_cast<float* const*>(output);
        for (int channel = 0; channel < m_physicalOutputs; ++channel) {
            std::fill_n(outputs[channel], frames, 0.0f);
        }
        for (size_t channel = 0; channel < m_inputs.size(); ++channel) {
            m_inputPointers[channel] = inputs[m_inputs[channel]];
        }
        for (size_t channel = 0; channel < m_outputs.size(); ++channel) {
            m_outputPointers[channel] = outputs[m_outputs[channel]];
        }

        const juce::ScopedLock lock(m_callbackLock);
        auto* receiver = m_receiver.load(std::memory_order_acquire);
        if (receiver) {
            const auto deviceTimePoint = [&times, now](double time) {
                return now + std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                    std::chrono::duration<double>(time - times.currentTime));
            };
            if (m_callback) {
                m_callback->processInput(m_inputPointers.data(), static_cast<int>(m_inputs.size()), frames,
                                         deviceTimePoint(times.inputBufferAdcTime));
            }
            receiver->audioDeviceIOCallbackWithContext(m_inputPointers.data(), static_cast<int>(m_inputs.size()),
                                                       m_outputPointers.data(), static_cast<int>(m_outputs.size()), frames, { &nanos });
            if (m_callback) {
                m_callback->processOutput(m_outputPointers.data(), static_cast<int>(m_outputs.size()), frames,
                                          deviceTimePoint(deviceTime));
            }
        }
        return paContinue;
    }

    std::optional<Device> m_input;
    std::optional<Device> m_output;
    const juce::CriticalSection& m_callbackLock;
    std::shared_ptr<AudioDeviceCallback> m_callback;
    PaStream* m_stream = nullptr;
#if USE_PORTMIXER
    PxMixer* m_mixer = nullptr;
#if JUCE_MAC
    float m_previousPlaythrough = -1.0f;
#endif
#endif
    std::atomic<juce::AudioIODeviceCallback*> m_receiver { nullptr };
    juce::String m_error;
    juce::BigInteger m_inputChannels;
    juce::BigInteger m_outputChannels;
    std::vector<int> m_inputs;
    std::vector<int> m_outputs;
    std::vector<const float*> m_inputPointers;
    std::vector<float*> m_outputPointers;
    int m_physicalOutputs = 0;
    int m_bufferSize = 512;
    double m_sampleRate = 44100.0;
    int m_inputLatency = 0;
    int m_outputLatency = 0;
};

class PortAudioDeviceType final : public juce::AudioIODeviceType
{
public:
    PortAudioDeviceType(int index, const juce::String& name, const juce::CriticalSection& callbackLock,
                        std::shared_ptr<AudioDeviceCallback> callback)
        : AudioIODeviceType(name), m_hostIndex(index), m_callbackLock(callbackLock), m_callback(std::move(callback))
    {
        scanForDevices();
    }

    void scanForDevices() override
    {
        m_inputs = devicesForHost(m_hostIndex, true);
        m_outputs = devicesForHost(m_hostIndex, false);
    }

    juce::StringArray getDeviceNames(bool input) const override
    {
        juce::StringArray names;
        for (const auto& device : input ? m_inputs : m_outputs) {
            names.add(device.name);
        }
        return names;
    }

    int getDefaultDeviceIndex(bool input) const override
    {
        auto* manager = DeviceManager::Instance();
        const auto* device = input ? manager->GetDefaultInputDevice(m_hostIndex) : manager->GetDefaultOutputDevice(m_hostIndex);
        if (device) {
            const auto& devices = input ? m_inputs : m_outputs;
            for (size_t index = 0; index < devices.size(); ++index) {
                if (devices[index].index == device->deviceIndex && devices[index].source == device->sourceIndex) {
                    return static_cast<int>(index);
                }
            }
        }
        return -1;
    }

    int getIndexOfDevice(juce::AudioIODevice* device, bool input) const override
    {
        const auto* portAudio = dynamic_cast<PortAudioDevice*>(device);
        if (!portAudio || !portAudio->device(input)) {
            return -1;
        }
        return getDeviceNames(input).indexOf(portAudio->device(input)->name);
    }

    bool hasSeparateInputsAndOutputs() const override { return true; }

    juce::AudioIODevice* createDevice(const juce::String& outputName, const juce::String& inputName) override
    {
        const auto input = find(m_inputs, inputName);
        const auto output = find(m_outputs, outputName);
        if ((!inputName.isEmpty() && !input) || (!outputName.isEmpty() && !output) || (!input && !output)) {
            return nullptr;
        }
        return new PortAudioDevice(getTypeName(), input, output, m_callbackLock, m_callback);
    }

private:
    static std::optional<Device> find(const std::vector<Device>& devices, const juce::String& name)
    {
        const auto device = std::find_if(devices.begin(), devices.end(), [&name](const Device& item) { return item.name == name; });
        return device == devices.end() ? std::nullopt : std::optional<Device>(*device);
    }

    int m_hostIndex;
    const juce::CriticalSection& m_callbackLock;
    std::shared_ptr<AudioDeviceCallback> m_callback;
    std::vector<Device> m_inputs;
    std::vector<Device> m_outputs;
};
}

std::vector<std::unique_ptr<juce::AudioIODeviceType> > createPortAudioDeviceTypes(const juce::CriticalSection& callbackLock,
                                                                                  std::shared_ptr<AudioDeviceCallback> callback)
{
    std::vector<std::unique_ptr<juce::AudioIODeviceType> > types;
    for (int index = 0; index < Pa_GetHostApiCount(); ++index) {
        const auto* info = Pa_GetHostApiInfo(index);
        types.push_back(std::make_unique<PortAudioDeviceType>(index, juce::String::fromUTF8(info->name), callbackLock, callback));
    }
    return types;
}

PaStream* portAudioStream(juce::AudioIODevice* device)
{
    const auto* portAudio = dynamic_cast<PortAudioDevice*>(device);
    return portAudio ? portAudio->stream() : nullptr;
}
}
