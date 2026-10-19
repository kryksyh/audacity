#
# Audacity: A Digital Audio Editor
#

tracktion_add_to_build()

get_property(TRACKTION_VST3_SDK_PATH GLOBAL PROPERTY vst3sdk_SOURCE_DIR)
juce_set_vst3_sdk_path("${TRACKTION_VST3_SDK_PATH}")

# JUCE requires the same SDK object layout in all configurations.
get_target_property(TRACKTION_VST3_DEFINITIONS vst3sdk COMPILE_DEFINITIONS)
list(REMOVE_ITEM TRACKTION_VST3_DEFINITIONS DEVELOPMENT RELEASE)
set_property(TARGET vst3sdk PROPERTY COMPILE_DEFINITIONS "${TRACKTION_VST3_DEFINITIONS}")
target_compile_definitions(vst3sdk PUBLIC DEVELOPMENT=0)
target_sources(vst3sdk PRIVATE
    ${TRACKTION_VST3_SDK_PATH}/base/source/timer.cpp
    ${TRACKTION_VST3_SDK_PATH}/public.sdk/source/vst/moduleinfo/moduleinfoparser.cpp
    ${TRACKTION_VST3_SDK_PATH}/public.sdk/source/vst/utility/vst2persistence.cpp
    ${TRACKTION_VST3_SDK_PATH}/public.sdk/source/vst/vstbus.cpp
    ${TRACKTION_VST3_SDK_PATH}/public.sdk/source/vst/vstcomponent.cpp
    ${TRACKTION_VST3_SDK_PATH}/public.sdk/source/vst/vstcomponentbase.cpp
    ${TRACKTION_VST3_SDK_PATH}/public.sdk/source/vst/vsteditcontroller.cpp
    ${TRACKTION_VST3_SDK_PATH}/public.sdk/source/vst/vstparameters.cpp
    ${TRACKTION_VST3_SDK_PATH}/public.sdk/source/vst/vstpresetfile.cpp
)

add_library(au_tracktion_engine STATIC)
target_link_libraries(au_tracktion_engine PRIVATE
    tracktion::tracktion_engine
    vst3sdk
    $<$<BOOL:${OS_IS_LIN}>:atomic>
)
target_sources(au_tracktion_engine PRIVATE internal/vst3sdkextensions.cpp)
target_compile_definitions(au_tracktion_engine PRIVATE JUCE_VST3HEADERS_INCLUDE_HEADERS_ONLY=1)
get_property(TRACKTION_SOURCE_DIR GLOBAL PROPERTY tracktion_SOURCE_DIR)
target_include_directories(au_tracktion_engine PRIVATE "${TRACKTION_SOURCE_DIR}")
target_include_directories(au_tracktion_engine PUBLIC
    "${TRACKTION_SOURCE_DIR}/juce/modules"
    "${TRACKTION_SOURCE_DIR}/tracktion_engine/modules"
)
set_target_properties(au_tracktion_engine PROPERTIES
    UNITY_BUILD OFF
    AUTOMOC OFF
    AUTOUIC OFF
    AUTORCC OFF
    POSITION_INDEPENDENT_CODE ON
)
target_compile_definitions(au_tracktion_engine PUBLIC
    $<$<CONFIG:Debug>:DEBUG=1>
    JUCE_GLOBAL_MODULE_SETTINGS_INCLUDED=1
    JUCE_USE_CURL=0
    JUCE_WEB_BROWSER=0
    JUCE_PLUGINHOST_VST3=1
    JUCE_PLUGINHOST_AU=$<BOOL:${OS_IS_MAC}>
    JUCE_PLUGINHOST_LV2=1
    TRACKTION_ENABLE_TIMESTRETCH_SOUNDTOUCH=1
    TRACKTION_ENABLE_TIMESTRETCH_RUBBERBAND=1
    TRACKTION_BUILD_RUBBERBAND=1
)
# Keep the embedded SoundTouch configure probe separate from Audacity's copy.
target_compile_definitions(au_tracktion_engine PRIVATE
    RUBBERBAND_STATIC=1
    $<$<BOOL:${OS_IS_WIN}>:soundtouch_ac_test=au_tracktion_soundtouch_ac_test>
)

if(OS_IS_WIN)
    # The LV2 C libraries use the narrow Windows API.
    if(CC_IS_MSVC)
        set(TRACKTION_LV2_UNICODE_OPTION /UUNICODE)
    else()
        set(TRACKTION_LV2_UNICODE_OPTION -UUNICODE)
    endif()
    set_property(SOURCE "${TRACKTION_SOURCE_DIR}/juce/modules/juce_audio_processors_headless/juce_audio_processors_headless_lv2_libs.cpp"
        APPEND PROPERTY COMPILE_OPTIONS "${TRACKTION_LV2_UNICODE_OPTION}")
endif()

muse_create_module(tracktionaudio NO_UNITY NO_PCH)
target_include_directories(tracktionaudio PRIVATE
    "${TRACKTION_SOURCE_DIR}/juce/modules/juce_audio_processors_headless/format_types/LV2_SDK/lv2"
)

target_sources(tracktionaudio PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/internal/tracktionautomation.cpp
    ${CMAKE_CURRENT_LIST_DIR}/internal/tracktionautomation.h
    ${CMAKE_CURRENT_LIST_DIR}/internal/tracktioninstrumenteditor.cpp
    ${CMAKE_CURRENT_LIST_DIR}/internal/tracktioninstrumenteditor.h
    ${CMAKE_CURRENT_LIST_DIR}/internal/tracktioninstrument.h
    ${CMAKE_CURRENT_LIST_DIR}/internal/audacityaudiosource.cpp
    ${CMAKE_CURRENT_LIST_DIR}/internal/audacityaudiosource.h
    ${CMAKE_CURRENT_LIST_DIR}/internal/audacityeffectregistry.cpp
    ${CMAKE_CURRENT_LIST_DIR}/internal/audacityeffectregistry.h
    ${CMAKE_CURRENT_LIST_DIR}/internal/audacityexternaleffect.cpp
    ${CMAKE_CURRENT_LIST_DIR}/internal/audacityexternaleffect.h
    ${CMAKE_CURRENT_LIST_DIR}/internal/audacityplugins.cpp
    ${CMAKE_CURRENT_LIST_DIR}/internal/audacityplugins.h
    ${CMAKE_CURRENT_LIST_DIR}/internal/audiosourceregistry.cpp
    ${CMAKE_CURRENT_LIST_DIR}/internal/audiosourceregistry.h
    ${CMAKE_CURRENT_LIST_DIR}/internal/portaudiodevice.cpp
    ${CMAKE_CURRENT_LIST_DIR}/internal/portaudiodevice.h
    ${CMAKE_CURRENT_LIST_DIR}/internal/tracktionaudiocallbacks.cpp
    ${CMAKE_CURRENT_LIST_DIR}/internal/tracktionaudiocallbacks.h
    ${CMAKE_CURRENT_LIST_DIR}/internal/tracktionaudioengine.cpp
    ${CMAKE_CURRENT_LIST_DIR}/internal/tracktionaudioengine.h
    ${CMAKE_CURRENT_LIST_DIR}/internal/tracktionaudioio.cpp
    ${CMAKE_CURRENT_LIST_DIR}/internal/tracktionaudioio.h
    ${CMAKE_CURRENT_LIST_DIR}/internal/tracktionmixsource.cpp
    ${CMAKE_CURRENT_LIST_DIR}/internal/tracktionmixsource.h
    ${CMAKE_CURRENT_LIST_DIR}/internal/tracktioneffectinstance.cpp
    ${CMAKE_CURRENT_LIST_DIR}/internal/tracktioneffectinstance.h
    ${CMAKE_CURRENT_LIST_DIR}/internal/tracktionlv2settings.cpp
    ${CMAKE_CURRENT_LIST_DIR}/internal/tracktionlv2settings.h
    ${CMAKE_CURRENT_LIST_DIR}/internal/tracktionlv2ui.cpp
    ${CMAKE_CURRENT_LIST_DIR}/internal/tracktionlv2ui.h
    ${CMAKE_CURRENT_LIST_DIR}/internal/tracktionparameterextractorservice.cpp
    ${CMAKE_CURRENT_LIST_DIR}/internal/tracktionparameterextractorservice.h
    ${CMAKE_CURRENT_LIST_DIR}/internal/tracktionplugindescriptions.cpp
    ${CMAKE_CURRENT_LIST_DIR}/internal/tracktionplugindescriptions.h
    ${CMAKE_CURRENT_LIST_DIR}/internal/tracktionpluginloader.cpp
    ${CMAKE_CURRENT_LIST_DIR}/internal/tracktionpluginloader.h
    ${CMAKE_CURRENT_LIST_DIR}/internal/tracktionpluginscanner.cpp
    ${CMAKE_CURRENT_LIST_DIR}/internal/tracktionpluginscanner.h
    ${CMAKE_CURRENT_LIST_DIR}/internal/tracktionrecording.cpp
    ${CMAKE_CURRENT_LIST_DIR}/internal/tracktionrecording.h
    ${CMAKE_CURRENT_LIST_DIR}/internal/tracktionrendersource.cpp
    ${CMAKE_CURRENT_LIST_DIR}/internal/tracktionrendersource.h
    ${CMAKE_CURRENT_LIST_DIR}/internal/tracktionruntime.cpp
    ${CMAKE_CURRENT_LIST_DIR}/internal/tracktionruntime.h
    ${CMAKE_CURRENT_LIST_DIR}/internal/tracktionsession.cpp
    ${CMAKE_CURRENT_LIST_DIR}/internal/tracktionsession.h
    ${CMAKE_CURRENT_LIST_DIR}/internal/tracktiontrackmeters.cpp
    ${CMAKE_CURRENT_LIST_DIR}/internal/tracktiontrackmeters.h
)

target_link_libraries(tracktionaudio PUBLIC
    au3-effects
PRIVATE
    au_tracktion_engine
    vst3sdk
    effects_base
    au3wrap
    au3-wave-track
    au3-midi-track
    au3-mixer
    au3-module-manager
    au3-audio-devices
    au3-realtime-effects
    au3-audio-io
    au3-project-rate
    au3-numeric-formats
    au3-time-frequency-selection
    portaudio::portaudio
    $<$<BOOL:${AU_USE_PORTMIXER}>:portmixer>
    Qt::Core
    $<$<BOOL:${AU_MODULE_EFFECTS_AUDIO_UNIT}>:au3-audio-unit>
)

target_compile_definitions(tracktionaudio PRIVATE AU_TRACKTION_AUDIO_UNIT=$<BOOL:${AU_MODULE_EFFECTS_AUDIO_UNIT}>)

if(AU_BUILD_TRACKTIONAUDIO_TESTS)
    add_subdirectory(tests)
endif()
