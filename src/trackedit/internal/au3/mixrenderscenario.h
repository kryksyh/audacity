/*
* Audacity: A Digital Audio Editor
*/
#pragma once

#include "modularity/ioc.h"
#include "types/ret.h"

namespace muse {
class IInteractive;
}
namespace au::audio {
class IAudioEngine;
}
namespace au::context {
class IGlobalContext;
}

namespace au::trackedit {
class IProjectHistory;
class ITracksViewRequestsService;
class MixRenderScenario : public muse::Contextable
{
    muse::GlobalInject<audio::IAudioEngine> audioEngine;
    muse::ContextInject<context::IGlobalContext> globalContext { this };
    muse::ContextInject<muse::IInteractive> interactive { this };
    muse::ContextInject<IProjectHistory> history { this };
    muse::ContextInject<ITracksViewRequestsService> viewRequests { this };

public:
    explicit MixRenderScenario(const muse::modularity::ContextPtr& ctx);
    muse::Ret run();

private:
    bool m_running = false;
};
}
