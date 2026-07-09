#
# Audacity: A Digital Audio Editor
#
declare_module(effects_plugins)

set(MODULE_SRC
    ${CMAKE_CURRENT_LIST_DIR}/plugineffectsmodule.cpp
    ${CMAKE_CURRENT_LIST_DIR}/plugineffectsmodule.h

    ${CMAKE_CURRENT_LIST_DIR}/internal/pluginstypes.h
    ${CMAKE_CURRENT_LIST_DIR}/internal/plugineffectsmetareader.cpp
    ${CMAKE_CURRENT_LIST_DIR}/internal/plugineffectsmetareader.h
    ${CMAKE_CURRENT_LIST_DIR}/internal/plugineffectsloader.cpp
    ${CMAKE_CURRENT_LIST_DIR}/internal/plugineffectsloader.h
    ${CMAKE_CURRENT_LIST_DIR}/internal/pluginviewlauncher.cpp
    ${CMAKE_CURRENT_LIST_DIR}/internal/pluginviewlauncher.h
    ${CMAKE_CURRENT_LIST_DIR}/internal/pluginparameterextractorservice.cpp
    ${CMAKE_CURRENT_LIST_DIR}/internal/pluginparameterextractorservice.h
    ${CMAKE_CURRENT_LIST_DIR}/internal/plugineffectadapter.cpp
    ${CMAKE_CURRENT_LIST_DIR}/internal/plugineffectadapter.h
    ${CMAKE_CURRENT_LIST_DIR}/internal/plugineffectsprovider.cpp
    ${CMAKE_CURRENT_LIST_DIR}/internal/plugineffectsprovider.h
    ${CMAKE_CURRENT_LIST_DIR}/internal/plugineffectviewsservice.cpp
    ${CMAKE_CURRENT_LIST_DIR}/internal/plugineffectviewsservice.h
    )

# AU3
include(${CMAKE_CURRENT_LIST_DIR}/../../au3wrap/au3wrapDefs.cmake)

set(MODULE_INCLUDE ${AU3_INCLUDE})
set(MODULE_DEF ${AU3_DEF})

set(MODULE_LINK au3wrap effects_base pluginhost au3-label-track au3-math)

set(MODULE_USE_UNITY OFF)

setup_module()
