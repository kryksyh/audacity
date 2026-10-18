get_property(NATIVE_VST_SDK GLOBAL PROPERTY vst3sdk_SOURCE_DIR)

add_library(native_instrument_fixture MODULE
    instrumentfixture.cpp
    ${NATIVE_VST_SDK}/public.sdk/source/main/pluginfactory.cpp
    ${NATIVE_VST_SDK}/public.sdk/source/vst/vstaudioeffect.cpp
    ${NATIVE_VST_SDK}/public.sdk/source/vst/vstcomponent.cpp
    ${NATIVE_VST_SDK}/public.sdk/source/vst/vstcomponentbase.cpp
    ${NATIVE_VST_SDK}/public.sdk/source/vst/vsteditcontroller.cpp
    ${NATIVE_VST_SDK}/public.sdk/source/vst/vstparameters.cpp
    ${NATIVE_VST_SDK}/public.sdk/source/vst/vstbus.cpp
)
target_link_libraries(native_instrument_fixture PRIVATE vst_sdk_3)
set_target_properties(native_instrument_fixture PROPERTIES
    BUNDLE TRUE
    BUNDLE_EXTENSION vst3
    MACOSX_BUNDLE_INFO_PLIST ${CMAKE_CURRENT_LIST_DIR}/Info.plist.in
    UNITY_BUILD OFF
    AUTOMOC OFF
    AUTOUIC OFF
    PREFIX ""
)
