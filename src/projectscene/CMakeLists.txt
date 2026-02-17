#
# Audacity: A Digital Audio Editor
#

declare_module(projectscene)

set(MODULE_QRC projectscene.qrc)

set(MODULE_SRC
    ${CMAKE_CURRENT_LIST_DIR}/projectscenemodule.cpp
    ${CMAKE_CURRENT_LIST_DIR}/projectscenemodule.h
    ${CMAKE_CURRENT_LIST_DIR}/iprojectsceneconfiguration.h
    ${CMAKE_CURRENT_LIST_DIR}/iprojectviewstate.h
    ${CMAKE_CURRENT_LIST_DIR}/iprojectviewstatecreator.h
    ${CMAKE_CURRENT_LIST_DIR}/iprojectsceneactionscontroller.h

    ${CMAKE_CURRENT_LIST_DIR}/types/projectscenetypes.h

    ${CMAKE_CURRENT_LIST_DIR}/internal/irealtimeeffectpaneltrackselection.h
    ${CMAKE_CURRENT_LIST_DIR}/internal/realtimeeffectpaneltrackselection.cpp
    ${CMAKE_CURRENT_LIST_DIR}/internal/realtimeeffectpaneltrackselection.h

    ${CMAKE_CURRENT_LIST_DIR}/internal/projectsceneconfiguration.cpp
    ${CMAKE_CURRENT_LIST_DIR}/internal/projectsceneconfiguration.h
    ${CMAKE_CURRENT_LIST_DIR}/internal/projectviewstate.cpp
    ${CMAKE_CURRENT_LIST_DIR}/internal/projectviewstate.h
    ${CMAKE_CURRENT_LIST_DIR}/internal/projectviewstatecreator.cpp
    ${CMAKE_CURRENT_LIST_DIR}/internal/projectviewstatecreator.h

    ${CMAKE_CURRENT_LIST_DIR}/internal/projectsceneuiactions.cpp
    ${CMAKE_CURRENT_LIST_DIR}/internal/projectsceneuiactions.h
    ${CMAKE_CURRENT_LIST_DIR}/internal/projectsceneactionscontroller.cpp
    ${CMAKE_CURRENT_LIST_DIR}/internal/projectsceneactionscontroller.h

    ${CMAKE_CURRENT_LIST_DIR}/internal/tapholdshortcut.cpp
    ${CMAKE_CURRENT_LIST_DIR}/internal/tapholdshortcut.h

    ${CMAKE_CURRENT_LIST_DIR}/internal/au3/trackheightattachment.cpp
    ${CMAKE_CURRENT_LIST_DIR}/internal/au3/trackheightattachment.h
    ${CMAKE_CURRENT_LIST_DIR}/internal/au3/trackrulertypeattachment.cpp
    ${CMAKE_CURRENT_LIST_DIR}/internal/au3/trackrulertypeattachment.h
    ${CMAKE_CURRENT_LIST_DIR}/internal/au3/trackviewtypeattachment.cpp
    ${CMAKE_CURRENT_LIST_DIR}/internal/au3/trackviewtypeattachment.h
    ${CMAKE_CURRENT_LIST_DIR}/internal/au3/viewinfo.cpp
    ${CMAKE_CURRENT_LIST_DIR}/internal/au3/viewinfo.h
    ${CMAKE_CURRENT_LIST_DIR}/internal/au3/waveformscale.cpp
    ${CMAKE_CURRENT_LIST_DIR}/internal/au3/waveformscale.h

    ${CMAKE_CURRENT_LIST_DIR}/view/toolbars/snaptoolbaritem.cpp
    ${CMAKE_CURRENT_LIST_DIR}/view/toolbars/snaptoolbaritem.h

    ${CMAKE_CURRENT_LIST_DIR}/view/trackruler/dblogmonoruler.cpp
    ${CMAKE_CURRENT_LIST_DIR}/view/trackruler/dblogmonoruler.h
    ${CMAKE_CURRENT_LIST_DIR}/view/trackruler/dblogstereoruler.cpp
    ${CMAKE_CURRENT_LIST_DIR}/view/trackruler/dblogstereoruler.h
    ${CMAKE_CURRENT_LIST_DIR}/view/trackruler/dblogbaseruler.cpp
    ${CMAKE_CURRENT_LIST_DIR}/view/trackruler/dblogbaseruler.h
    ${CMAKE_CURRENT_LIST_DIR}/view/trackruler/dblinearmonoruler.cpp
    ${CMAKE_CURRENT_LIST_DIR}/view/trackruler/dblinearmonoruler.h
    ${CMAKE_CURRENT_LIST_DIR}/view/trackruler/dblinearstereoruler.cpp
    ${CMAKE_CURRENT_LIST_DIR}/view/trackruler/dblinearstereoruler.h
    ${CMAKE_CURRENT_LIST_DIR}/view/trackruler/dblinearbaseruler.cpp
    ${CMAKE_CURRENT_LIST_DIR}/view/trackruler/dblinearbaseruler.h
    ${CMAKE_CURRENT_LIST_DIR}/view/trackruler/itrackruler.h
    ${CMAKE_CURRENT_LIST_DIR}/view/trackruler/linearstereoruler.cpp
    ${CMAKE_CURRENT_LIST_DIR}/view/trackruler/linearstereoruler.h
    ${CMAKE_CURRENT_LIST_DIR}/view/trackruler/linearmonoruler.cpp
    ${CMAKE_CURRENT_LIST_DIR}/view/trackruler/linearmonoruler.h
    ${CMAKE_CURRENT_LIST_DIR}/view/trackruler/linearbaseruler.cpp
    ${CMAKE_CURRENT_LIST_DIR}/view/trackruler/linearbaseruler.h
    )

set(MODULE_QML_IMPORT ${CMAKE_CURRENT_LIST_DIR}/qml )

set(MODULE_LINK au3wrap trackedit spectrogram uicomponents)
if(MUSE_MODULE_WORKSPACE)
    set(MODULE_LINK ${MODULE_LINK} muse::workspace_qml)
endif()

set(MODULE_INCLUDE ${CLIPSVIEW_V1_INC})

set(MODULE_DEF ${CLIPSVIEW_V1_DEF})

set(MODULE_USE_UNITY OFF)

setup_module()

add_subdirectory(qml/Audacity/ProjectScene)

if (AU_BUILD_PROJECTSCENE_TESTS)
    add_subdirectory(tests)
endif()
