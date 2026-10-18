/*
* Audacity: A Digital Audio Editor
*/
#include <gtest/gtest.h>
#include <vector>
#include <string>
#include <portaudio.h>
#include <pa_mac_core.h>
#include <portmixer.h>
#include <CoreAudio/CoreAudio.h>
#include "au3-utility/MemoryX.h"
#include "au3-audio-devices/AudioIOBase.h"
#include "au3-audio-devices/DeviceManager.h"

namespace au::au3audio {
namespace {
struct DeviceMetadataAccess : AudioIOBase {
    using AudioIOBase::getPlayDevIndex;
};

std::string utf8(CFStringRef text)
{
    if (!text) {
        return {};
    }
    std::string result(CFStringGetMaximumSizeForEncoding(CFStringGetLength(text), kCFStringEncodingUTF8) + 1, '\0');
    if (!CFStringGetCString(text, result.data(), result.size(), kCFStringEncodingUTF8)) {
        return {};
    }
    result.resize(std::char_traits<char>::length(result.c_str()));
    return result;
}
}

TEST(NativeAudioDeviceMetadataTests, NativeDeviceIdentityAndInputSourcesDoNotRequireAnAudioStream)
{
    ASSERT_EQ(Pa_Initialize(), paNoError);
    const auto terminate = finally([] { Pa_Terminate(); });
    EXPECT_EQ(PaMacCore_GetDeviceID(paNoDevice), kAudioDeviceUnknown);
    EXPECT_EQ(Px_OpenMixer(nullptr, paNoDevice, paNoDevice, 0), nullptr);
    const auto count = Pa_GetDeviceCount();
    ASSERT_GE(count, 0);
    EXPECT_EQ(PaMacCore_GetDeviceID(count), kAudioDeviceUnknown);
    for (int index = 0; index < count; ++index) {
        const auto* info = Pa_GetDeviceInfo(index);
        ASSERT_NE(info, nullptr);
        const auto* host = Pa_GetHostApiInfo(info->hostApi);
        ASSERT_NE(host, nullptr);
        if (host->type != paCoreAudio) {
            EXPECT_EQ(PaMacCore_GetDeviceID(index), kAudioDeviceUnknown);
            continue;
        }
        const auto device = PaMacCore_GetDeviceID(index);
        ASSERT_NE(device, kAudioDeviceUnknown) << index;
        AudioObjectPropertyAddress nameProperty { kAudioObjectPropertyName, kAudioObjectPropertyScopeGlobal,
                                                  kAudioObjectPropertyElementMaster };
        CFStringRef name = nullptr;
        UInt32 size = sizeof(name);
        ASSERT_EQ(AudioObjectGetPropertyData(device, &nameProperty, 0, nullptr, &size, &name), noErr);
        const auto releaseName = finally([&] {
            if (name) {
                CFRelease(name);
            }
        });
        EXPECT_EQ(utf8(name), info->name) << index;
        if (!info->maxInputChannels) {
            continue;
        }
        auto* mixer = Px_OpenMixer(nullptr, index, paNoDevice, 0);
        ASSERT_NE(mixer, nullptr) << info->name;
        const auto close = finally([&] { Px_CloseMixer(mixer); });
        AudioObjectPropertyAddress sourcesProperty { kAudioDevicePropertyDataSources, kAudioDevicePropertyScopeInput,
                                                     kAudioObjectPropertyElementMaster };
        size = 0;
        const auto status = AudioObjectGetPropertyDataSize(device, &sourcesProperty, 0, nullptr, &size);
        if (status != noErr) {
            EXPECT_EQ(Px_GetNumInputSources(mixer), 0) << info->name;
            continue;
        }
        EXPECT_EQ(Px_GetNumInputSources(mixer), size / sizeof(UInt32)) << info->name;
        std::vector<UInt32> sources(size / sizeof(UInt32));
        if (sources.empty()) {
            continue;
        }
        ASSERT_EQ(AudioObjectGetPropertyData(device, &sourcesProperty, 0, nullptr, &size, sources.data()), noErr);
        for (size_t source = 0; source < sources.size(); ++source) {
            CFStringRef sourceName = nullptr;
            const auto release = finally([&] {
                if (sourceName) {
                    CFRelease(sourceName);
                }
            });
            AudioValueTranslation value { &sources[source], sizeof(UInt32), &sourceName, sizeof(sourceName) };
            auto sourceProperty = sourcesProperty;
            sourceProperty.mSelector = kAudioDevicePropertyDataSourceNameForIDCFString;
            size = sizeof(value);
            ASSERT_EQ(AudioObjectGetPropertyData(device, &sourceProperty, 0, nullptr, &size, &value), noErr);
            const auto* actual = Px_GetInputSourceName(mixer, source);
            ASSERT_NE(actual, nullptr);
            EXPECT_EQ(utf8(sourceName), actual) << info->name << " source " << source;
        }
    }
}

TEST(NativeAudioDeviceMetadataTests, DefaultProjectRateUsesOutputMetadataWithoutProbingCaptureFormats)
{
    ASSERT_EQ(Pa_Initialize(), paNoError);
    const auto terminate = finally([] { Pa_Terminate(); });
    DeviceManager::Instance()->Rescan();
    const auto index = DeviceMetadataAccess::getPlayDevIndex();
    const auto* info = Pa_GetDeviceInfo(index);
    if (!info) {
        EXPECT_EQ(AudioIOBase::GetOptimalSupportedSampleRate(), 44100);
        return;
    }
    const auto* host = Pa_GetHostApiInfo(info->hostApi);
    ASSERT_NE(host, nullptr);
    if (host->type == paCoreAudio && info->defaultSampleRate > 0) {
        EXPECT_EQ(AudioIOBase::GetOptimalSupportedSampleRate(), static_cast<int>(info->defaultSampleRate));
    }
}
}
