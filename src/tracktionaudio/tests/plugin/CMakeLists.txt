#
# Audacity: A Digital Audio Editor
#

function(add_tracktion_test_plugin target product code mode)
    string(REPLACE "_" "." bundle_id ${target})
    juce_add_plugin(${target}
        COMPANY_NAME Audacity
        BUNDLE_ID org.audacity.${bundle_id}
        PLUGIN_MANUFACTURER_CODE AuTe
        PLUGIN_CODE ${code}
        PRODUCT_NAME ${product}
        VERSION 1.0.0
        FORMATS VST3
        VST3_CATEGORIES Fx
        VST3_AUTO_MANIFEST FALSE
        COPY_PLUGIN_AFTER_BUILD FALSE
    )
    target_sources(${target} PRIVATE processor.cpp)
    target_link_libraries(${target} PRIVATE juce::juce_audio_processors)
    target_compile_definitions(${target} PUBLIC
        JUCE_USE_CURL=0
        JUCE_WEB_BROWSER=0
        JUCE_VST3_CAN_REPLACE_VST2=0
        AU_TEST_PLUGIN_CHANNEL_MODE=${mode}
    )
    set_target_properties(${target} PROPERTIES UNITY_BUILD OFF)
endfunction()

add_tracktion_test_plugin(tracktionaudio_testplugin TracktionHostTest TeAu 0)
add_tracktion_test_plugin(tracktionaudio_testplugin_mono TracktionMonoHostTest TeMo 1)
add_tracktion_test_plugin(tracktionaudio_testplugin_stereo TracktionStereoHostTest TeSt 2)
add_tracktion_test_plugin(tracktionaudio_testplugin_mono_stereo TracktionMonoStereoHostTest TeMs 3)

juce_add_plugin(tracktionaudio_testinstrument
    COMPANY_NAME Audacity
    BUNDLE_ID org.audacity.tracktion.instrumenttest
    PLUGIN_MANUFACTURER_CODE AuTe
    PLUGIN_CODE TeMi
    PRODUCT_NAME TracktionInstrumentHostTest
    VERSION 1.0.0
    FORMATS VST3
    IS_SYNTH TRUE
    NEEDS_MIDI_INPUT TRUE
    VST3_CATEGORIES Instrument Synth
    VST3_AUTO_MANIFEST FALSE
    COPY_PLUGIN_AFTER_BUILD FALSE
)
target_sources(tracktionaudio_testinstrument PRIVATE processor.cpp)
target_link_libraries(tracktionaudio_testinstrument PRIVATE juce::juce_audio_processors)
target_compile_definitions(tracktionaudio_testinstrument PUBLIC
    JUCE_USE_CURL=0
    JUCE_WEB_BROWSER=0
    JUCE_VST3_CAN_REPLACE_VST2=0
    AU_TEST_PLUGIN_CHANNEL_MODE=4
)
set_target_properties(tracktionaudio_testinstrument PROPERTIES UNITY_BUILD OFF)

set(TRACKTION_TEST_LV2_BUNDLE "${CMAKE_CURRENT_BINARY_DIR}/Tracktion Host Test.lv2")
file(MAKE_DIRECTORY "${TRACKTION_TEST_LV2_BUNDLE}")
configure_file(lv2manifest.ttl.in "${TRACKTION_TEST_LV2_BUNDLE}/manifest.ttl" @ONLY)
configure_file(lv2plugin.ttl "${TRACKTION_TEST_LV2_BUNDLE}/plugin.ttl" @ONLY)
configure_file(lv2classes.ttl "${TRACKTION_TEST_LV2_BUNDLE}/classes.ttl" COPYONLY)
add_library(tracktionaudio_testplugin_lv2 MODULE lv2processor.cpp)
target_include_directories(tracktionaudio_testplugin_lv2 PRIVATE
    "${PROJECT_SOURCE_DIR}/au3/libraries"
    "${TRACKTION_SOURCE_DIR}/juce/modules"
    "${TRACKTION_SOURCE_DIR}/juce/modules/juce_audio_processors_headless/format_types/LV2_SDK/lv2"
)
set_target_properties(tracktionaudio_testplugin_lv2 PROPERTIES
    PREFIX ""
    OUTPUT_NAME hosttest
    LIBRARY_OUTPUT_DIRECTORY "${TRACKTION_TEST_LV2_BUNDLE}"
    RUNTIME_OUTPUT_DIRECTORY "${TRACKTION_TEST_LV2_BUNDLE}"
    AUTOMOC OFF
    AUTOUIC OFF
    AUTORCC OFF
    UNITY_BUILD OFF
)
set(TRACKTION_TEST_LV2_BUNDLE "${TRACKTION_TEST_LV2_BUNDLE}" PARENT_SCOPE)
