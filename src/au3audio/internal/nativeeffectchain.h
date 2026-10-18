/*
* Audacity: A Digital Audio Editor
*/
#pragma once

#include "nativeroutinggraph.h"
#include "au3-components/EffectInterface.h"
#include "au3-automation-data/AutomationData.h"

class RealtimeEffectList;

namespace au::au3audio {
class NativeEffectChain final
{
public:
    struct Specification {
        const EffectInstanceFactory* effect = nullptr;
        EffectSettings settings;
        AutomationData automation;
    };
    static std::vector<Specification> specifications(const RealtimeEffectList& list, bool automation = true);

    NativeEffectChain(std::vector<Specification> effects, double rate, size_t preferredBlockSize = 128, size_t channels = 2,
                      const EffectProcessingContext& context = {});
    ~NativeEffectChain();
    void process(NativeRoutingGraph::Buffer buffers, size_t frames, int64_t sample);
    // Private offline path: pull future input as needed, returning audio at
    // the requested content position even when a native stage changes delay.
    void pull(NativeRoutingGraph::Buffer buffers, size_t frames, int64_t sample, const NativeRoutingGraph::Processor& input);
    void reset(int64_t sample);
    size_t latency() const;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};
}
