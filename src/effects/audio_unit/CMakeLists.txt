#
# Audacity: A Digital Audio Editor
#
muse_create_module(effects_audio_unit NO_UNITY)

target_sources(effects_audio_unit PRIVATE
    audiouniteffectsmodule.cpp
    audiouniteffectsmodule.h

    internal/audiouniteffectloader.cpp
    internal/audiouniteffectloader.h

    internal/audiounitpluginsscanner.h

    internal/audiounitpluginsmetareader.h

    internal/audiounitviewlauncher.cpp
    internal/audiounitviewlauncher.h
    internal/audiounittypes.h
)

target_link_libraries(effects_audio_unit PRIVATE
    tracktionaudio
    au_tracktion_engine
    au3wrap
    au3-effects
    au3-module-manager
    au3-realtime-effects
    au3-audio-unit
)

add_subdirectory(qml/Audacity/AudioUnit)
