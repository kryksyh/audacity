#
# Audacity: A Digital Audio Editor
#

add_subdirectory(plugin)

set(MODULE_TEST tracktionaudio_tests)
set(MODULE_TEST_SRC
    ${PROJECT_SOURCE_DIR}/src/projectscene/types/projectscenetypes.h
    ${CMAKE_CURRENT_LIST_DIR}/audiocallbacks_tests.cpp
    ${CMAKE_CURRENT_LIST_DIR}/miditrack_tests.cpp
    ${CMAKE_CURRENT_LIST_DIR}/graphlatency_tests.cpp
    ${CMAKE_CURRENT_LIST_DIR}/timestretcher_tests.cpp
    ${CMAKE_CURRENT_LIST_DIR}/asyncplugin_tests.cpp
    ${CMAKE_CURRENT_LIST_DIR}/pluginscanner_tests.cpp
    ${CMAKE_CURRENT_LIST_DIR}/lv2scanner_tests.cpp
    ${CMAKE_CURRENT_LIST_DIR}/lv2host_tests.cpp
    ${CMAKE_CURRENT_LIST_DIR}/lv2loader_tests.cpp
    ${CMAKE_CURRENT_LIST_DIR}/hostedeffect_tests.cpp
    ${CMAKE_CURRENT_LIST_DIR}/audiounithost_tests.cpp
    ${CMAKE_CURRENT_LIST_DIR}/vstloader_tests.cpp
    ${CMAKE_CURRENT_LIST_DIR}/audiounitloader_tests.cpp
    ${CMAKE_CURRENT_LIST_DIR}/audiosource_tests.cpp
    ${CMAKE_CURRENT_LIST_DIR}/exportsource_tests.cpp
    ${CMAKE_CURRENT_LIST_DIR}/environment.cpp
    ${PROJECT_SOURCE_DIR}/src/effects/builtin_collection/basstreble/basstrebleeffect.cpp
    ${PROJECT_SOURCE_DIR}/src/effects/builtin_collection/dynamics/compressor/compressoreffect.cpp
    ${PROJECT_SOURCE_DIR}/src/effects/builtin_collection/reverb/reverbeffect.cpp
)
set(MODULE_TEST_LINK
    tracktionaudio
    au_tracktion_engine
    au3wrap
    au3-builtin-effects
    au3-realtime-effects
    au3-stretching-sequence
    au3-mod-pcm
    au3-mod-flac
    au3-effects
    au3-module-manager
    au3-numeric-formats
    $<$<BOOL:${AU_MODULE_EFFECTS_NYQUIST}>:au3-nyquist-effects>
    effects_base
    trackedit
    automation
    muse::ui
    muse::ui_qml
    au3-project-history
    au3-midi-track
    au3-transactions
    au3-tags
    SndFile::sndfile
    portaudio::portaudio
    $<$<BOOL:${AU_MODULE_EFFECTS_AUDIO_UNIT}>:au3-audio-unit>
    $<$<BOOL:${AU_MODULE_EFFECTS_AUDIO_UNIT}>:effects_audio_unit>
    $<$<BOOL:${AU_MODULE_EFFECTS_AUDIO_UNIT}>:effects_audio_unit_qml>
    $<$<BOOL:${AU_MODULE_EFFECTS_VST}>:au3-vst3>
    $<$<BOOL:${AU_MODULE_EFFECTS_VST}>:effects_vst>
    $<$<BOOL:${AU_MODULE_EFFECTS_VST}>:effects_vst_qml>
)
set(MODULE_TEST_DATA_ROOT ${CMAKE_CURRENT_LIST_DIR})
set(MODULE_TEST_INCLUDE
    ${PROJECT_SOURCE_DIR}/src/trackedit
    ${PROJECT_SOURCE_DIR}/src/effects/builtin_collection
    ${PROJECT_SOURCE_DIR}/src/effects/effects_base
)
include(SetupGTest)
if(AU_MODULE_EFFECTS_AUDIO_UNIT)
    target_sources(tracktionaudio_tests PRIVATE ${CMAKE_CURRENT_LIST_DIR}/audiounitnativeview_tests.mm)
endif()
target_include_directories(tracktionaudio_tests PRIVATE
    "${TRACKTION_SOURCE_DIR}/juce/modules/juce_audio_processors_headless/format_types/LV2_SDK/lv2"
    "${TRACKTION_SOURCE_DIR}/rubberband"
)
target_compile_definitions(tracktionaudio_tests PRIVATE RUBBERBAND_STATIC=1)
target_compile_definitions(tracktionaudio_tests PRIVATE AU_TRACKTION_TEST_AUDIO_UNIT=$<BOOL:${AU_MODULE_EFFECTS_AUDIO_UNIT}>)
get_target_property(TRACKTION_TEST_PLUGIN_PATH tracktionaudio_testplugin_VST3 JUCE_PLUGIN_ARTEFACT_FILE)
target_compile_definitions(tracktionaudio_tests PRIVATE
    AU_TRACKTION_TEST_VST3=$<BOOL:${AU_MODULE_EFFECTS_VST}>
    AU_TRACKTION_TEST_VST3_PATH="${TRACKTION_TEST_PLUGIN_PATH}"
)
add_dependencies(tracktionaudio_tests tracktionaudio_testplugin_VST3)
get_target_property(TRACKTION_TEST_INSTRUMENT_PATH tracktionaudio_testinstrument_VST3 JUCE_PLUGIN_ARTEFACT_FILE)
target_compile_definitions(tracktionaudio_tests PRIVATE AU_TRACKTION_TEST_INSTRUMENT_PATH="${TRACKTION_TEST_INSTRUMENT_PATH}")
add_dependencies(tracktionaudio_tests tracktionaudio_testinstrument_VST3)
foreach(variant mono stereo mono_stereo)
    get_target_property(TRACKTION_TEST_PLUGIN_PATH tracktionaudio_testplugin_${variant}_VST3 JUCE_PLUGIN_ARTEFACT_FILE)
    string(TOUPPER ${variant} variant_upper)
    target_compile_definitions(tracktionaudio_tests PRIVATE AU_TRACKTION_TEST_VST3_${variant_upper}_PATH="${TRACKTION_TEST_PLUGIN_PATH}")
    add_dependencies(tracktionaudio_tests tracktionaudio_testplugin_${variant}_VST3)
endforeach()

target_compile_definitions(tracktionaudio_tests PRIVATE
    AU_TRACKTION_TEST_LV2_BUNDLE="${TRACKTION_TEST_LV2_BUNDLE}"
    AU_TRACKTION_TEST_LV2_BINARY="$<TARGET_FILE:tracktionaudio_testplugin_lv2>"
)
add_dependencies(tracktionaudio_tests tracktionaudio_testplugin_lv2)

target_compile_definitions(tracktionaudio_tests PRIVATE AU_TRACKTION_TEST_NYQUIST=$<BOOL:${AU_MODULE_EFFECTS_NYQUIST}>)
if(AU_MODULE_EFFECTS_NYQUIST)
    target_compile_definitions(tracktionaudio_tests PRIVATE
        AU_TRACKTION_TEST_NYQUIST_RUNTIME="$<TARGET_PROPERTY:nyquist-runtime,NYQUIST_RUNTIME_DIR>"
    )
    add_dependencies(tracktionaudio_tests nyquist-runtime)
endif()

if(AU_MODULE_EFFECTS_LV2)
    target_link_libraries(tracktionaudio_tests effects_lv2)
else()
    target_sources(tracktionaudio_tests PRIVATE
        ${PROJECT_SOURCE_DIR}/src/effects/lv2/internal/lv2effectloader.cpp
        ${PROJECT_SOURCE_DIR}/src/effects/lv2/internal/lv2viewlauncher.cpp
        ${PROJECT_SOURCE_DIR}/src/effects/lv2/view/lv2viewmodel.cpp
        ${PROJECT_SOURCE_DIR}/src/effects/lv2/view/lv2viewmodel.h
    )
endif()
