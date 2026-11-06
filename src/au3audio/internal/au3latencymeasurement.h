/*
* Audacity: A Digital Audio Editor
*/
#pragma once

#include <memory>
#include <thread>

#include "framework/global/async/asyncable.h"
#include "framework/global/iapplication.h"
#include "framework/global/modularity/ioc.h"

#include "audio/driver/iaudiodrivercontroller.h"
#include "audio/driver/ilatencymeasurement.h"
#include "audio/iaudioengine.h"

class PortAudioLatencyTest;

namespace au::au3audio {
class Au3LatencyMeasurement final : public audio::ILatencyMeasurement, public muse::async::Asyncable
{
    muse::GlobalInject<audio::IAudioEngine> audioEngine;
    muse::GlobalInject<audio::IAudioDriverController> audioDriverController;
    muse::GlobalInject<muse::IApplication> application;

public:
    Au3LatencyMeasurement();
    ~Au3LatencyMeasurement() override;
    void deinit();

    bool isMeasuring() const override;
    int signalsPlayed() const override;
    void cancel() override;
    void measure(const muse::modularity::ContextPtr& requester, std::function<void(const audio::LatencyMeasurement&)> done) override;

private:
    void stopPlayback(const audio::AudioStreamDescriptor& stream);

    bool m_measuring = false;
    std::thread m_measureThread;
    std::unique_ptr<PortAudioLatencyTest> m_testSession;
};
}
