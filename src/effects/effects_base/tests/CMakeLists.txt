#
# Audacity: A Digital Audio Editor
#

set(MODULE_TEST effects_base_tests)

set(MODULE_TEST_SRC
    ${CMAKE_CURRENT_LIST_DIR}/environment.cpp
    ${CMAKE_CURRENT_LIST_DIR}/config_tests.cpp
    ${CMAKE_CURRENT_LIST_DIR}/effectscommands_tests.cpp
    ${CMAKE_CURRENT_LIST_DIR}/effectsutils_tests.cpp
    ${CMAKE_CURRENT_LIST_DIR}/pluginmigration_tests.cpp
    ${CMAKE_CURRENT_LIST_DIR}/realtimeeffectscopy_tests.cpp
    ${CMAKE_CURRENT_LIST_DIR}/realtimeeffectservice_tests.cpp
)

set(MODULE_TEST_LINK
    au3wrap
    au3-project
    au3-realtime-effects
    au3-track
    au3-wave-track
    effects_base
    muse_audioplugins

    Qt::Qml
)

if(MUSE_MODULE_EXTENSIONS)
    list(APPEND MODULE_TEST_SRC ${PROJECT_SOURCE_DIR}/src/effects/extensions/tests/extensioneffectsscanner_tests.cpp)
    list(APPEND MODULE_TEST_LINK effects_extensions muse::ui muse::ui_qml)
endif()

set(MODULE_TEST_INCLUDE
    ${PROJECT_SOURCE_DIR}/src/effects/effects_base # for internal headers included relatively (e.g. realtimeeffectservice.h)
)
set(MODULE_TEST_DATA_ROOT ${CMAKE_CURRENT_LIST_DIR})

include(SetupGTest)
