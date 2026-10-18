#
# Audacity: A Digital Audio Editor
#

declare_module(au3audio)

set(MODULE_QML_IMPORT ${CMAKE_CURRENT_LIST_DIR}/qml)

set(MODULE_SRC
    #public
    ${CMAKE_CURRENT_LIST_DIR}/au3audiomodule.cpp
    ${CMAKE_CURRENT_LIST_DIR}/au3audiomodule.h
    ${CMAKE_CURRENT_LIST_DIR}/isystemaudiodeviceslistener.h

    ${CMAKE_CURRENT_LIST_DIR}/internal/instrumentprocessor.h
    ${CMAKE_CURRENT_LIST_DIR}/internal/instrumentprocessor.cpp
    ${CMAKE_CURRENT_LIST_DIR}/internal/nativeinstrumentfactory.cpp
    ${CMAKE_CURRENT_LIST_DIR}/internal/nativeinstrumentfactory.h
    ${CMAKE_CURRENT_LIST_DIR}/internal/nativeeffectparameters.cpp
    ${CMAKE_CURRENT_LIST_DIR}/internal/nativeeffectparameters.h
    ${CMAKE_CURRENT_LIST_DIR}/internal/nativetimesignature.cpp
    ${CMAKE_CURRENT_LIST_DIR}/internal/nativetimesignature.h
    ${CMAKE_CURRENT_LIST_DIR}/internal/nativeplaybackprocessor.cpp
    ${CMAKE_CURRENT_LIST_DIR}/internal/nativeplaybackprocessor.h
    ${CMAKE_CURRENT_LIST_DIR}/internal/nativeroutinggraph.cpp
    ${CMAKE_CURRENT_LIST_DIR}/internal/nativeroutinggraph.h
    ${CMAKE_CURRENT_LIST_DIR}/internal/nativeofflinegraph.cpp
    ${CMAKE_CURRENT_LIST_DIR}/internal/nativeofflinegraph.h
    ${CMAKE_CURRENT_LIST_DIR}/internal/nativeeffectchain.cpp
    ${CMAKE_CURRENT_LIST_DIR}/internal/nativeeffectchain.h
    ${CMAKE_CURRENT_LIST_DIR}/internal/nativeaudiorendersource.cpp
    ${CMAKE_CURRENT_LIST_DIR}/internal/nativeaudiorendersource.h
    ${CMAKE_CURRENT_LIST_DIR}/internal/nativemonitorsource.cpp
    ${CMAKE_CURRENT_LIST_DIR}/internal/nativemonitorsource.h
    ${CMAKE_CURRENT_LIST_DIR}/internal/nativeinstrumentsessions.cpp
    ${CMAKE_CURRENT_LIST_DIR}/internal/nativeinstrumentsessions.h
    ${CMAKE_CURRENT_LIST_DIR}/internal/liveinstrumentsession.cpp
    ${CMAKE_CURRENT_LIST_DIR}/internal/liveinstrumentsession.h
    ${CMAKE_CURRENT_LIST_DIR}/internal/builtininstrument.cpp
    ${CMAKE_CURRENT_LIST_DIR}/internal/midiplayablesequence.cpp
    ${CMAKE_CURRENT_LIST_DIR}/internal/midiplayablesequence.h

    # AU3
    ${CMAKE_CURRENT_LIST_DIR}/internal/au3audioengine.cpp
    ${CMAKE_CURRENT_LIST_DIR}/internal/au3audioengine.h
    ${CMAKE_CURRENT_LIST_DIR}/internal/au3audiodrivercontroller.cpp
    ${CMAKE_CURRENT_LIST_DIR}/internal/au3audiodrivercontroller.h
    ${CMAKE_CURRENT_LIST_DIR}/internal/au3audioiolistener.h
    ${CMAKE_CURRENT_LIST_DIR}/internal/defaultplaybackpolicy.cpp
    ${CMAKE_CURRENT_LIST_DIR}/internal/defaultplaybackpolicy.h
    )

if (OS_IS_MAC)
    set(MODULE_SRC ${MODULE_SRC}
        ${CMAKE_CURRENT_LIST_DIR}/internal/platform/macos/macossystemaudiodeviceslistener.cpp
        ${CMAKE_CURRENT_LIST_DIR}/internal/platform/macos/macossystemaudiodeviceslistener.h
    )
    # the CoreAudio system headers clash with the `using namespace muse`
    # of other unity-combined sources
    set_source_files_properties(${CMAKE_CURRENT_LIST_DIR}/internal/platform/macos/macossystemaudiodeviceslistener.cpp
        PROPERTIES SKIP_UNITY_BUILD_INCLUSION ON
    )
elseif (OS_IS_WIN)
    set(MODULE_SRC ${MODULE_SRC}
        ${CMAKE_CURRENT_LIST_DIR}/internal/platform/windows/windowssystemaudiodeviceslistener.cpp
        ${CMAKE_CURRENT_LIST_DIR}/internal/platform/windows/windowssystemaudiodeviceslistener.h
    )
    # the Windows system headers clash with the `using namespace muse`
    # of other unity-combined sources
    set_source_files_properties(${CMAKE_CURRENT_LIST_DIR}/internal/platform/windows/windowssystemaudiodeviceslistener.cpp
        PROPERTIES SKIP_UNITY_BUILD_INCLUSION ON
    )
elseif (OS_IS_LIN)
    find_path(LIBUDEV_INCLUDE_DIR libudev.h)
    find_library(LIBUDEV_LIBRARY udev)
    if (LIBUDEV_INCLUDE_DIR AND LIBUDEV_LIBRARY)
        set(MODULE_SRC ${MODULE_SRC}
            ${CMAKE_CURRENT_LIST_DIR}/internal/platform/linux/linuxsystemaudiodeviceslistener.cpp
            ${CMAKE_CURRENT_LIST_DIR}/internal/platform/linux/linuxsystemaudiodeviceslistener.h
        )
    else()
        message(STATUS "libudev not found - system audio device change detection disabled")
        set(MODULE_SRC ${MODULE_SRC}
            ${CMAKE_CURRENT_LIST_DIR}/internal/platform/stub/stubsystemaudiodeviceslistener.h
        )
    endif()
else()
    set(MODULE_SRC ${MODULE_SRC}
        ${CMAKE_CURRENT_LIST_DIR}/internal/platform/stub/stubsystemaudiodeviceslistener.h
    )
endif()

set(MODULE_LINK
        au3-audio-io
        au3-midi-track
        au3-stretching-sequence
        au3-realtime-effects
        effects_base
        muse::midi
        au3wrap
        au3-audio-devices
        au3-math
        au3-project-rate
        au3-numeric-formats
        au3-time-frequency-selection
        au3-track
        au3-utility
)

if (OS_IS_MAC)
    set(MODULE_LINK ${MODULE_LINK} "-framework CoreAudio")
elseif (OS_IS_WIN)
    set(MODULE_LINK ${MODULE_LINK} ole32)
elseif (OS_IS_LIN AND LIBUDEV_INCLUDE_DIR AND LIBUDEV_LIBRARY)
    set(MODULE_INCLUDE ${MODULE_INCLUDE} ${LIBUDEV_INCLUDE_DIR})
    set(MODULE_LINK ${MODULE_LINK} ${LIBUDEV_LIBRARY})
    set(MODULE_DEF ${MODULE_DEF} AU_AU3AUDIO_HAS_UDEV)
endif()

set(MODULE_USE_UNITY OFF)
setup_module()

target_compile_definitions(au3audio PRIVATE AU_NATIVE_VST=$<BOOL:${AU_MODULE_EFFECTS_VST}>)
target_compile_definitions(au3audio PRIVATE AU_NATIVE_AUDIO_UNIT=$<BOOL:${AU_MODULE_EFFECTS_AUDIO_UNIT}>)
target_compile_definitions(au3audio PRIVATE AU_NATIVE_LV2=$<BOOL:${AU_MODULE_EFFECTS_LV2}>)
if (AU_MODULE_EFFECTS_LV2)
    target_sources(au3audio PRIVATE internal/lv2instrument.cpp)
    target_link_libraries(au3audio PRIVATE au3-lv2 Qt::Widgets)
endif()
if (AU_MODULE_EFFECTS_AUDIO_UNIT)
    target_sources(au3audio PRIVATE internal/audiounitinstrument.mm)
    target_link_libraries(au3audio PRIVATE au3-audio-unit "-framework AudioToolbox" "-framework AudioUnit" "-framework Cocoa")
    set_source_files_properties(internal/audiounitinstrument.mm PROPERTIES COMPILE_FLAGS "-fobjc-arc" SKIP_PRECOMPILE_HEADERS ON)
endif()
if (AU_MODULE_EFFECTS_VST)
    target_sources(au3audio PRIVATE internal/vstinstrument.cpp)
    target_link_libraries(au3audio PRIVATE au3-vst3)
endif()

if (AU_BUILD_AU3AUDIO_TESTS)
    add_subdirectory(tests)
endif()
