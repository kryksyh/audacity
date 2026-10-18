/*
* Audacity: A Digital Audio Editor
*/
#include "nativetimesignature.h"
#include "au3-project/Project.h"
#include "au3-numeric-formats/ProjectTimeSignature.h"

namespace au::au3audio {
namespace {
const AttachedProjectObjects::RegisteredFactory registration {
    [](AudacityProject& project) { return std::make_shared<NativeTimeSignature>(project); }
};
}
NativeTimeSignature& NativeTimeSignature::Get(AudacityProject& project)
{
    return project.AttachedObjects::Get<NativeTimeSignature&>(registration);
}

NativeTimeSignature::NativeTimeSignature(AudacityProject& project)
{
    auto& signature = ProjectTimeSignature::Get(project);
    publish(signature.GetUpperTimeSignature(), signature.GetLowerTimeSignature());
    m_subscription = signature.Subscribe([this](const TimeSignatureChangedMessage& message) {
        publish(message.newUpperTimeSignature, message.newLowerTimeSignature);
    });
}

void NativeTimeSignature::publish(int numerator, int denominator) noexcept
{
    m_meter.store((uint64_t(uint32_t(numerator)) << 32) | uint32_t(denominator), std::memory_order_relaxed);
}

NativeTimeSignature::Meter NativeTimeSignature::meter() const noexcept
{
    const auto packed = m_meter.load(std::memory_order_relaxed);
    return { static_cast<int32_t>(packed >> 32), static_cast<int32_t>(packed & 0xffffffffu) };
}
}
