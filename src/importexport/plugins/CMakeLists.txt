#
# Audacity: A Digital Audio Editor
#
declare_module(importexport_plugins)

set(MODULE_SRC
    ${CMAKE_CURRENT_LIST_DIR}/pluginformatsmodule.cpp
    ${CMAKE_CURRENT_LIST_DIR}/pluginformatsmodule.h

    ${CMAKE_CURRENT_LIST_DIR}/internal/pluginimporter.cpp
    ${CMAKE_CURRENT_LIST_DIR}/internal/pluginimporter.h
    ${CMAKE_CURRENT_LIST_DIR}/internal/pluginexporter.cpp
    ${CMAKE_CURRENT_LIST_DIR}/internal/pluginexporter.h
    )

set(MODULE_LINK au3wrap pluginhost)

set(MODULE_USE_UNITY OFF)

setup_module()
