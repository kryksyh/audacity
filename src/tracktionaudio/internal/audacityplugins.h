/*
* Audacity: A Digital Audio Editor
*/
#pragma once

#include <memory>
#include <atomic>
#include <limits>

namespace tracktion {
inline namespace engine {
class Engine;
class Plugin;
}
}

namespace au::tracktionaudio {
class AudioSourceRegistry;

inline constexpr auto AUDACITY_ENVELOPE_PLUGIN_TYPE = "audacity-envelope";
inline constexpr auto AUDACITY_SOURCE_ID_PROPERTY = "audacitySourceId";
inline constexpr auto AUDACITY_TRACK_CONTROLS_PLUGIN_TYPE = "audacity-track-controls";
inline constexpr auto AUDACITY_LEFT_GAIN_PROPERTY = "audacityLeftGain";
inline constexpr auto AUDACITY_RIGHT_GAIN_PROPERTY = "audacityRightGain";
inline constexpr auto AUDACITY_VOLUME_PROPERTY = "audacityVolume";
inline constexpr auto AUDACITY_PAN_PROPERTY = "audacityPan";
inline constexpr auto AUDACITY_PLAYBACK_CONTROLS_PLUGIN_TYPE = "audacity-playback-controls";

struct AudacityPlaybackControls {
    std::atomic<double> endTime { std::numeric_limits<double>::max() };
    std::atomic<double> silentLoopStart { std::numeric_limits<double>::max() };
    std::atomic<float> volume { 1.0f };
    std::atomic<int> endSampleOffset { -1 };
};

void registerAudacityPlugins(tracktion::Engine& engine, const std::shared_ptr<AudioSourceRegistry>& sources);
std::shared_ptr<AudacityPlaybackControls> audacityPlaybackControls(tracktion::Plugin& plugin);
}
