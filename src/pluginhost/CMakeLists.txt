#
# Audacity: A Digital Audio Editor
#
declare_module(pluginhost)

set(MODULE_SRC
    ${CMAKE_CURRENT_LIST_DIR}/pluginhostmodule.cpp
    ${CMAKE_CURRENT_LIST_DIR}/pluginhostmodule.h
    ${CMAKE_CURRENT_LIST_DIR}/ipluginhostservice.h

    ${CMAKE_CURRENT_LIST_DIR}/api/audacity_plugin.h

    ${CMAKE_CURRENT_LIST_DIR}/plugin.cpp
    ${CMAKE_CURRENT_LIST_DIR}/plugin.h
    ${CMAKE_CURRENT_LIST_DIR}/hostdsp.cpp
    ${CMAKE_CURRENT_LIST_DIR}/hostdsp.h

    ${CMAKE_CURRENT_LIST_DIR}/internal/pluginhostservice.cpp
    ${CMAKE_CURRENT_LIST_DIR}/internal/pluginhostservice.h
    )

# AU3 (wxDynamicLibrary, FileNames, Resample)
include(${CMAKE_CURRENT_LIST_DIR}/../au3wrap/au3wrapDefs.cmake)

set(MODULE_INCLUDE ${AU3_INCLUDE})
set(MODULE_DEF ${AU3_DEF})

set(MODULE_LINK au3wrap au3-math)

set(MODULE_USE_UNITY OFF)

setup_module()
