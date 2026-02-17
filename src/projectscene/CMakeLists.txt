#
# Audacity: A Digital Audio Editor
#

muse_create_module(projectscene NO_UNITY)

target_sources(projectscene PRIVATE
    projectscenemodule.cpp
    projectscenemodule.h
    iprojectsceneconfiguration.h
    iprojectviewstate.h
    iprojectviewstatecreator.h
    iprojectsceneactionscontroller.h

    internal/irealtimeeffectpaneltrackselection.h
    internal/realtimeeffectpaneltrackselection.cpp
    internal/realtimeeffectpaneltrackselection.h

    internal/projectsceneconfiguration.cpp
    internal/projectsceneconfiguration.h
    internal/projectviewstate.cpp
    internal/projectviewstate.h
    internal/projectviewstatecreator.cpp
    internal/projectviewstatecreator.h

    internal/projectsceneuiactions.cpp
    internal/projectsceneuiactions.h
    internal/projectsceneactionscontroller.cpp
    internal/projectsceneactionscontroller.h

    internal/tapholdshortcut.cpp
    internal/tapholdshortcut.h

    internal/au3/trackheightattachment.cpp
    internal/au3/trackheightattachment.h
    internal/au3/trackrulertypeattachment.cpp
    internal/au3/trackrulertypeattachment.h
    internal/au3/trackviewtypeattachment.cpp
    internal/au3/trackviewtypeattachment.h
    internal/au3/viewinfo.cpp
    internal/au3/viewinfo.h
    internal/au3/waveformscale.cpp
    internal/au3/waveformscale.h

    # Remaining view files (used by module C++ IoC exports, not QML-only)
    view/tracksitemsview/au3/isamplespainter.h
    view/tracksitemsview/au3/samplespainter.h
    view/tracksitemsview/au3/samplespainter.cpp
    view/tracksitemsview/au3/samplespainterutils.h
    view/tracksitemsview/au3/samplespainterutils.cpp
    view/tracksitemsview/au3/iminmaxrmspainter.h
    view/tracksitemsview/au3/minmaxrmspainter.cpp
    view/tracksitemsview/au3/minmaxrmspainter.h
    view/tracksitemsview/au3/iconnectingdotspainter.h
    view/tracksitemsview/au3/connectingdotspainter.cpp
    view/tracksitemsview/au3/connectingdotspainter.h
    view/tracksitemsview/au3/iau3wavepainter.h
    view/tracksitemsview/au3/sampledata.h
    view/tracksitemsview/au3/WaveformSettings.cpp
    view/tracksitemsview/au3/WaveformSettings.h
    view/tracksitemsview/au3/WaveformPainter.cpp
    view/tracksitemsview/au3/WaveformPainter.h
    view/tracksitemsview/au3/WaveMetrics.h
    view/tracksitemsview/au3/wavepainterutils.cpp
    view/tracksitemsview/au3/wavepainterutils.h
    view/tracksitemsview/wavepainterproxy.cpp
    view/tracksitemsview/wavepainterproxy.h
    view/tracksitemsview/iwavepainter.h
)

qt_add_resources(projectscene projectscene_resources
    PREFIX
        /
    FILES
        images/customCursorShapes/ClipStretchBoth.png
        images/customCursorShapes/ClipStretchLeft.png
        images/customCursorShapes/ClipStretchRight.png
        images/customCursorShapes/ClipTrimBoth.png
        images/customCursorShapes/ClipTrimLeft.png
        images/customCursorShapes/ClipTrimRight.png
        images/customCursorShapes/IBeamCursor.png
        images/customCursorShapes/Pencil.png
        images/customCursorShapes/SelectionLeft.png
        images/customCursorShapes/SelectionRight.png
        images/customCursorShapes/Smooth.png
        images/customCursorShapes/Split.png
)

target_link_libraries(projectscene PRIVATE
    au3wrap
    trackedit
    spectrogram
    uicomponents
    Qt::Quick
)

add_subdirectory(qml/Audacity/ProjectScene)

if (AU_BUILD_PROJECTSCENE_TESTS)
    add_subdirectory(tests)
endif()
