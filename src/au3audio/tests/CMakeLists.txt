#
# Audacity: A Digital Audio Editor
#

set(MODULE_TEST au3audio_tests)

set(MODULE_TEST_SRC
    ${CMAKE_CURRENT_LIST_DIR}/nativeinstrument_tests.cpp
    ${CMAKE_CURRENT_LIST_DIR}/midiinputclock_tests.cpp
    ${CMAKE_CURRENT_LIST_DIR}/nativeroutinggraph_tests.cpp
    ${CMAKE_CURRENT_LIST_DIR}/nativeeffectchain_tests.cpp
    ${CMAKE_CURRENT_LIST_DIR}/nativeaudiorender_tests.cpp
    ${CMAKE_CURRENT_LIST_DIR}/mididocument_tests.cpp
    ${CMAKE_CURRENT_LIST_DIR}/midifile_tests.cpp
    ${CMAKE_CURRENT_LIST_DIR}/nativeeditmodels_tests.cpp
    ${PROJECT_SOURCE_DIR}/src/automation/view/trackautomationmodel.cpp
    ${PROJECT_SOURCE_DIR}/src/automation/view/trackautomationmodel.h
    ${CMAKE_CURRENT_LIST_DIR}/environment.cpp
    ${CMAKE_CURRENT_LIST_DIR}/au3audiodrivercontroller_tests.cpp
    ${CMAKE_CURRENT_LIST_DIR}/mocks/audioenginemock.h
    )

set(MODULE_TEST_LINK
    au3audio
    importer
    projectscene
    trackedit
    muse::ui_qml
    au3-builtin-effects
    au3-mod-pcm
    SndFile::sndfile
    au3-audio-io
    muse::midi
    au3wrap
    au3-midi-track
    au3-project-file-io
    au3-project-history
    au3-project-rate
    au3-numeric-formats
    au3-realtime-effects
    au3-module-manager
    effects_base
    portaudio::portaudio
    )

set(MODULE_TEST_DATA_ROOT ${CMAKE_CURRENT_LIST_DIR})

include(SetupGTest)

get_target_property(NATIVE_TEST_SOURCES au3audio_tests SOURCES)
list(FILTER NATIVE_TEST_SOURCES EXCLUDE REGEX "/testing/gmain\\.cpp$")
set_property(TARGET au3audio_tests PROPERTY SOURCES ${NATIVE_TEST_SOURCES})
target_sources(au3audio_tests PRIVATE main.cpp)
target_link_libraries(au3audio_tests Qt::Widgets)
target_include_directories(au3audio_tests PRIVATE ${PROJECT_SOURCE_DIR}/src/trackedit)
foreach(codec flac ogg opus mp3 mp2 wavpack)
    if (TARGET au3-mod-${codec})
        target_link_libraries(au3audio_tests au3-mod-${codec})
        string(TOUPPER ${codec} codec_define)
        target_compile_definitions(au3audio_tests PRIVATE AU_NATIVE_TEST_CODEC_${codec_define}=1)
        if (codec STREQUAL "mp3" OR codec STREQUAL "mp2")
            target_link_libraries(au3audio_tests mpg123::libmpg123)
        endif()
        if (codec STREQUAL "wavpack")
            target_link_libraries(au3audio_tests wavpack::wavpack)
        endif()
    endif()
endforeach()
if (OS_IS_MAC)
    target_include_directories(au3audio_tests PRIVATE ${PROJECT_SOURCE_DIR}/muse/framework/midi)
    if (AU_USE_PORTMIXER)
        target_sources(au3audio_tests PRIVATE nativeaudiodevices_macos_tests.cpp)
        target_link_libraries(au3audio_tests "-framework CoreAudio")
    endif()
endif()

if (AU_MODULE_EFFECTS_LV2)
    add_subdirectory(lv2plugin)
    target_sources(au3audio_tests PRIVATE lv2instrument_tests.cpp)
    target_link_libraries(au3audio_tests au3-lv2)
    target_compile_definitions(au3audio_tests PRIVATE
        AU_NATIVE_TEST_LV2_PATH="$<TARGET_FILE_DIR:native_lv2_fixture>/"
        AU_NATIVE_TEST_LV2_BINARY="$<TARGET_FILE:native_lv2_fixture>"
    )
    add_dependencies(au3audio_tests native_lv2_fixture)
endif()

target_compile_definitions(au3audio_tests PRIVATE AU_NATIVE_TEST_AUDIO_UNIT=$<BOOL:${AU_MODULE_EFFECTS_AUDIO_UNIT}>)
if (AU_MODULE_EFFECTS_AUDIO_UNIT)
    target_link_libraries(au3audio_tests au3-audio-unit)
endif()

if (OS_IS_MAC AND AU_MODULE_EFFECTS_VST)
    add_subdirectory(plugin)
    target_link_libraries(au3audio_tests au3-vst3)
    target_compile_definitions(au3audio_tests PRIVATE
        AU_NATIVE_TEST_INSTRUMENT_PATH="$<TARGET_BUNDLE_DIR:native_instrument_fixture>"
        AU_NATIVE_TEST_INSTRUMENT_BINARY="$<TARGET_FILE:native_instrument_fixture>"
    )
    add_dependencies(au3audio_tests native_instrument_fixture)
endif()
