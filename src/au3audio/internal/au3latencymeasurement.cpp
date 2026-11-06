/*
* Audacity: A Digital Audio Editor
*/
#include "au3latencymeasurement.h"

#include <exception>

#include "framework/global/async/async.h"
#include "framework/global/log.h"
#include "framework/global/runtime.h"

#include "audio/iaudiostreamsuspender.h"
#include "context/iglobalcontext.h"

#include "au3-audio-devices/AudioIOBase.h"
#include "au3-audio-devices/DeviceManager.h"
#include "au3-audio-io/PortAudioLatencyTest.h"
#include "au3-project-rate/ProjectRate.h"

using namespace au::audio;
using namespace au::au3audio;

namespace {
LatencyMeasurement toLatencyMeasurement(const PortAudioLatencyTest::Result& r)
{
    LatencyMeasurement result;
    switch (r.status) {
    case PortAudioLatencyTest::Result::Status::Measured: result.status = LatencyMeasurement::Status::Measured;
        break;
    case PortAudioLatencyTest::Result::Status::Cancelled: result.status = LatencyMeasurement::Status::Cancelled;
        break;
    case PortAudioLatencyTest::Result::Status::DeviceError: result.status = LatencyMeasurement::Status::DeviceError;
        break;
    case PortAudioLatencyTest::Result::Status::NoSignal: result.status = LatencyMeasurement::Status::NoSignal;
        break;
    }
    result.sampleRate = r.sampleRate;
    result.roundTripFrames = r.roundTripFrames;
    result.reportedInputLatencyMs = r.reportedInputLatencySecs * 1000.0;
    result.reportedOutputLatencyMs = r.reportedOutputLatencySecs * 1000.0;
    result.signalsFound = r.burstsFound;
    result.signalsSent = r.burstsTotal;
    result.spreadFrames = r.spreadFrames;
    result.inputPeak = r.inputPeak;
    result.inverted = r.inverted;
    return result;
}
}

Au3LatencyMeasurement::Au3LatencyMeasurement() = default;

Au3LatencyMeasurement::~Au3LatencyMeasurement()
{
    deinit();
}

void Au3LatencyMeasurement::deinit()
{
    cancel();
    if (m_measureThread.joinable()) {
        m_measureThread.join();
    }
    async_disconnectAll();
    m_testSession.reset();
    m_measuring = false;
}

bool Au3LatencyMeasurement::isMeasuring() const
{
    return m_measuring;
}

int Au3LatencyMeasurement::signalsPlayed() const
{
    return m_testSession ? m_testSession->SignalsPlayed() : 0;
}

void Au3LatencyMeasurement::cancel()
{
    if (m_testSession) {
        m_testSession->Cancel();
    }
}

void Au3LatencyMeasurement::measure(const muse::modularity::ContextPtr& requester,
                                    std::function<void(const LatencyMeasurement&)> done)
{
    LatencyMeasurement busy;
    busy.status = LatencyMeasurement::Status::Busy;
    if (m_measuring) {
        done(busy);
        return;
    }

    const auto stream = audioEngine()->currentStream();
    if (stream) {
        if (stream->kind == AudioStreamKind::Recording) {
            done(busy);
            return;
        }
        stopPlayback(*stream);
    }

    const auto config = audioDriverController()->configuration();
    auto devices = DeviceManager::Instance();
    const int outputDevice
        = devices->GetOutputDevicePaIndex(config.api,
                                          config.outputDevice.value_or(audioDriverController()->systemDefaultOutputDevice(config.api)));
    const int inputDevice
        = devices->GetInputDevicePaIndex(config.api,
                                         config.inputDevice.value_or(audioDriverController()->systemDefaultInputDevice(config.api)));
    const auto globalContext = muse::modularity::ioc(requester)->resolve<context::IGlobalContext>("au3audio");
    const auto currentProject = globalContext->currentProject();
    const auto project = currentProject ? reinterpret_cast<AudacityProject*>(currentProject->au3ProjectPtr()) : nullptr;
    const double projectRate = project ? ::ProjectRate::Get(*project).GetRate() : static_cast<double>(config.defaultSampleRate);
    double rate = 0.0;
    if (config.asioUseDeviceSampleRate && DeviceManager::IsAsioDevice(outputDevice)) {
        rate = DeviceManager::GetAsioDeviceCurrentSampleRate(outputDevice);
    }
    if (rate == 0.0) {
        rate = AudioIOBase::GetClosestSupportedSampleRate(outputDevice, inputDevice, projectRate);
    }
    m_testSession = std::make_unique<PortAudioLatencyTest>(outputDevice, inputDevice, rate,
                                                           config.bufferLength, config.inputChannels);
    m_measuring = true;
    m_measureThread = std::thread([this, done]() {
        LatencyMeasurement result;
        try {
            const auto measured = m_testSession->Run();
            result = toLatencyMeasurement(measured);
            LOGI() << "Latency measurement: frames=" << measured.roundTripFrames
                   << ", rate=" << measured.sampleRate << ", signals=" << measured.burstsFound << "/" << measured.burstsTotal
                   << ", correlation=" << measured.minCorrelation << ", spread=" << measured.spreadFrames;
        } catch (const std::exception& e) {
            LOGE() << "Latency measurement failed: " << e.what();
        }
        muse::async::Async::call(this, [this, result, done]() {
            m_measureThread.join();
            m_testSession.reset();
            m_measuring = false;
            done(result);
        }, muse::runtime::mainThreadId());
    });
}

void Au3LatencyMeasurement::stopPlayback(const AudioStreamDescriptor& stream)
{
    for (const auto& context : application()->contexts()) {
        const auto globalContext = muse::modularity::ioc(context)->resolve<au::context::IGlobalContext>("au3audio");
        const auto project = globalContext ? globalContext->currentProject() : nullptr;
        if (!project || reinterpret_cast<AudacityProject*>(project->au3ProjectPtr()) != stream.ownerProject) {
            continue;
        }
        const auto suspender = muse::modularity::ioc(context)->resolve<IAudioStreamSuspender>("au3audio");
        if (suspender) {
            // Stop playback without restoring it after the test.
            suspender->suspendForAudioConfiguration(stream.kind);
        }
        break;
    }
    if (audioEngine()->currentStream()) {
        audioEngine()->stopTransport();
    }
}
