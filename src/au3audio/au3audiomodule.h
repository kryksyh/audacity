/*
* Audacity: A Digital Audio Editor
*/
#pragma once

#include <memory>

#include "framework/global/modularity/imodulesetup.h"

namespace au::tracktionaudio {
class TracktionAudioEngine;
}

namespace au::au3audio {
class Au3AudioDriverController;
class ISystemAudioDevicesListener;

class Au3AudioModule : public muse::modularity::IModuleSetup
{
public:

    std::string moduleName() const override;
    void registerExports() override;
    void onInit(const muse::IApplication::RunMode& mode) override;
    void onDeinit() override;

private:
    std::shared_ptr<tracktionaudio::TracktionAudioEngine> m_audioEngine;
    std::shared_ptr<Au3AudioDriverController> m_audioDriverController;
    std::shared_ptr<ISystemAudioDevicesListener> m_systemAudioDevicesListener;
};
}
