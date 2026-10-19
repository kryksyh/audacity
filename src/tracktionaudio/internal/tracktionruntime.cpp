/*
* Audacity: A Digital Audio Editor
*/
#include <tracktion_engine/tracktion_engine.h>

#include "tracktionruntime.h"

#include <limits>
#include <optional>

#include <QMetaObject>
#include <QObject>
#include <QSocketNotifier>

#include "audiosourceregistry.h"
#include "audacityplugins.h"
#include "audacityeffectregistry.h"
#include "portaudiodevice.h"
#include "au3-audio-devices/AudioIOBase.h"

#if JUCE_LINUX
#include <juce_events/native/juce_EventLoopInternal_linux.h>
#endif

using namespace au::tracktionaudio;

namespace {
class AudacityEngineBehaviour final : public tracktion::EngineBehaviour
{
public:
    bool autoInitialiseDeviceManager() override { return false; }
    bool addSystemAudioIODeviceTypes() override { return false; }
    bool shouldProcessMutedTracks() override { return true; }
    bool shouldKeepPlaybackPositionOnLoopStart() override { return true; }
    tracktion::TimeDuration getMinimumPlaybackLoopLength() override { return {}; }
    std::optional<int> getAudioRecordingFlushIntervalSamples(double sampleRate) override { return juce::roundToInt(sampleRate * 0.05); }

    tracktion::EditLimits getEditLimits() override
    {
        constexpr int maximum = std::numeric_limits<int>::max();
        return { maximum, maximum, maximum, maximum, maximum };
    }
};

class AudacityPropertyStorage final : public tracktion::PropertyStorage
{
public:
    AudacityPropertyStorage(const std::string& cacheDirectory, const std::string& settingsDirectory)
        : tracktion::PropertyStorage("Audacity"), m_cacheDirectory(cacheDirectory), m_settingsDirectory(settingsDirectory)
    {
    }

    juce::File getAppCacheFolder() override { return m_cacheDirectory; }
    juce::File getAppPrefsFolder() override { return m_settingsDirectory; }

private:
    juce::File m_cacheDirectory;
    juce::File m_settingsDirectory;
};

#if JUCE_LINUX
class JuceEventLoop final : public QObject, private juce::LinuxEventLoopInternal::Listener
{
public:
    JuceEventLoop()
    {
        juce::LinuxEventLoopInternal::registerLinuxEventLoopListener(*this);
        updateNotifiers();
    }

    ~JuceEventLoop() override
    {
        juce::LinuxEventLoopInternal::deregisterLinuxEventLoopListener(*this);
    }

private:
    void fdCallbacksChanged() override
    {
        QMetaObject::invokeMethod(this, [this] { updateNotifiers(); }, Qt::QueuedConnection);
    }

    void updateNotifiers()
    {
        m_notifiers.clear();
        for (const int fd : juce::LinuxEventLoopInternal::getRegisteredFds()) {
            auto notifier = std::make_unique<QSocketNotifier>(fd, QSocketNotifier::Read, this);
            QObject::connect(notifier.get(), &QSocketNotifier::activated, this, [fd] {
                juce::LinuxEventLoopInternal::invokeEventLoopCallbackForFd(fd);
            });
            m_notifiers.push_back(std::move(notifier));
        }
    }

    std::vector<std::unique_ptr<QSocketNotifier> > m_notifiers;
};
#endif
}

struct TracktionRuntime::Data
{
    struct DeviceConfiguration {
        std::string host;
        std::string input;
        std::string output;
        int inputChannels;
        int outputChannels;
        double sampleRate;
        double latencyMs;
        std::shared_ptr<AudioDeviceCallback> callback;
        bool canReuseFor(const DeviceConfiguration& requested) const
        {
            return (requested.inputChannels > 0 || requested.outputChannels > 0) && host == requested.host
                   && (requested.inputChannels == 0 || (input == requested.input && inputChannels == requested.inputChannels))
                   && (requested.outputChannels == 0 || (output == requested.output && outputChannels == requested.outputChannels))
                   && sampleRate == requested.sampleRate && latencyMs == requested.latencyMs && callback == requested.callback;
        }
    };

    Data(const std::string& cacheDirectory, const std::string& settingsDirectory)
        : sources(std::make_shared<AudioSourceRegistry>()), effects(std::make_shared<AudacityEffectRegistry>()),
        engine(std::make_unique<tracktion::Engine>(std::make_unique<AudacityPropertyStorage>(cacheDirectory, settingsDirectory),
                                                   nullptr, std::make_unique<AudacityEngineBehaviour>()))
    {
        engine->getAudioFileFormatManager().addFormat([registry = sources] {
            return registry->createFormat().release();
        }, false, false);
        registerAudacityPlugins(*engine, sources);
        registerAudacityEffectPlugins(*engine, effects);
    }

    juce::ScopedJuceInitialiser_GUI juceInitialiser;
    std::shared_ptr<AudioSourceRegistry> sources;
    std::shared_ptr<AudacityEffectRegistry> effects;
    std::unique_ptr<tracktion::Engine> engine;
    bool devicesInitialised = false;
    std::optional<DeviceConfiguration> deviceConfiguration;
#if JUCE_LINUX
    JuceEventLoop eventLoop;
#endif
};

TracktionRuntime::TracktionRuntime() = default;

TracktionRuntime::~TracktionRuntime() = default;

void TracktionRuntime::init(const std::string& cacheDirectory, const std::string& settingsDirectory)
{
    m_data = std::make_shared<Data>(cacheDirectory, settingsDirectory);
}

void TracktionRuntime::deinit()
{
    m_data.reset();
}

std::string TracktionRuntime::openAudioDevice(const std::string& host, const std::string& inputName, const std::string& outputName,
                                              int inputChannels, int outputChannels, double sampleRate,
                                              std::shared_ptr<AudioDeviceCallback> callback)
{
    auto& manager = engine().getDeviceManager();
    auto& devices = manager.deviceManager;
    const Data::DeviceConfiguration configuration {
        host, inputName, outputName, inputChannels, outputChannels, sampleRate, AudioIOLatencyDuration.Read(), callback
    };
    if (m_data->deviceConfiguration && m_data->deviceConfiguration->canReuseFor(configuration) && isAudioDeviceRunning()) {
        const juce::ScopedLock lock(devices.getAudioCallbackLock());
        auto* device = devices.getCurrentAudioDevice();
        if (callback) {
            callback->prepare(device->getCurrentSampleRate(), device->getCurrentBufferSizeSamples(),
                              device->getActiveInputChannels().countNumberOfSetBits(),
                              device->getActiveOutputChannels().countNumberOfSetBits());
        }
        return {};
    }
    closeAudioDevice();

    juce::Array<juce::AudioIODeviceType*> oldTypes;
    for (auto* type : devices.getAvailableDeviceTypes()) {
        oldTypes.add(type);
    }
    for (auto* type : oldTypes) {
        devices.removeAudioDeviceType(type);
    }
    for (auto& type : createPortAudioDeviceTypes(devices.getAudioCallbackLock(), std::move(callback))) {
        devices.addAudioDeviceType(std::move(type));
    }
    if (!m_data->devicesInitialised) {
        manager.initialise(0, 0);
        m_data->devicesInitialised = true;
    }

    juce::AudioIODeviceType* selectedType = nullptr;
    for (auto* type : devices.getAvailableDeviceTypes()) {
        if (type->getTypeName() == juce::String(host)) {
            selectedType = type;
            break;
        }
    }
    if (!selectedType) {
        return "The audio host is no longer available.";
    }

    const auto deviceName = [selectedType](const std::string& name, bool input, int channels) {
        if (channels == 0) {
            return juce::String();
        }
        if (!name.empty()) {
            return juce::String(name);
        }
        const auto names = selectedType->getDeviceNames(input);
        return names[selectedType->getDefaultDeviceIndex(input)];
    };

    const auto input = deviceName(inputName, true, inputChannels);
    const auto output = deviceName(outputName, false, outputChannels);
    if ((inputChannels > 0 && input.isEmpty()) || (outputChannels > 0 && output.isEmpty())) {
        return "The audio device is no longer available.";
    }

    juce::BigInteger inputs, outputs;
    inputs.setRange(0, inputChannels, true);
    outputs.setRange(0, outputChannels, true);
    juce::XmlElement settings("DEVICESETUP");
    settings.setAttribute("deviceType", selectedType->getTypeName());
    settings.setAttribute("audioInputDeviceName", input);
    settings.setAttribute("audioOutputDeviceName", output);
    settings.setAttribute("audioDeviceInChans", inputs.toString(2));
    settings.setAttribute("audioDeviceOutChans", outputs.toString(2));
    settings.setAttribute("audioDeviceRate", sampleRate);
    settings.setAttribute("audioDeviceBufferSize", 512);
    const auto error = devices.initialise(inputChannels, outputChannels, &settings, false);
    if (error.isEmpty()) {
        manager.rescanWaveDeviceList();
        m_data->deviceConfiguration = configuration;
    }
    manager.dispatchPendingUpdates();
    return error.toStdString();
}

void TracktionRuntime::closeAudioDevice()
{
    m_data->deviceConfiguration.reset();
    auto& manager = engine().getDeviceManager();
    manager.deviceManager.closeAudioDevice();
    manager.dispatchPendingUpdates();
}

bool TracktionRuntime::isAudioDeviceRunning() const
{
    auto* device = engine().getDeviceManager().deviceManager.getCurrentAudioDevice();
    return device && device->isPlaying();
}

void TracktionRuntime::setInputGain(tracktion::WaveInputDevice& input, float gain)
{
    // Use a finite floor that converts zero gain to exact silence.
    input.setInputGainDb(juce::Decibels::gainToDecibels(gain, std::numeric_limits<float>::lowest()));
}

tracktion::Engine& TracktionRuntime::engine() const
{
    return *m_data->engine;
}

std::shared_ptr<AudioSourceRegistry> TracktionRuntime::sources() const
{
    return m_data->sources;
}

std::shared_ptr<AudacityEffectRegistry> TracktionRuntime::effects() const
{
    return m_data->effects;
}

std::shared_ptr<void> TracktionRuntime::lifetime() const
{
    return m_data;
}
