muse_create_qml_module(projectscene_qml FOR projectscene)

file(GLOB_RECURSE PROJECTSCENE_QML_FILES
    RELATIVE ${CMAKE_CURRENT_SOURCE_DIR}
    CONFIGURE_DEPENDS
    "*.qml"
)

qt_add_qml_module(projectscene_qml
    URI Audacity.ProjectScene
    VERSION 1.0
    SOURCES
    view/common/tracksviewstatemodel.cpp
    view/common/tracksviewstatemodel.h
    view/common/trackviewstatemodel.cpp
    view/common/trackviewstatemodel.h
    view/common/customcursor.cpp
    view/common/customcursor.h

    view/toolbars/projecttoolbarmodel.cpp
    view/toolbars/projecttoolbarmodel.h
    view/toolbars/undoredotoolbarmodel.cpp
    view/toolbars/undoredotoolbarmodel.h
    view/toolbars/playbacktoolbarmodel.cpp
    view/toolbars/playbacktoolbarmodel.h
    view/toolbars/playbacktoolbarcustomisemodel.cpp
    view/toolbars/playbacktoolbarcustomisemodel.h
    view/toolbars/playbacktoolbarcustomiseitem.cpp
    view/toolbars/playbacktoolbarcustomiseitem.h
    view/toolbars/workspacestoolbarmodel.cpp
    view/toolbars/workspacestoolbarmodel.h
    view/toolbars/audiosetupcontextmenumodel.cpp
    view/toolbars/audiosetupcontextmenumodel.h

    view/trackspanel/addeffectmenumodel.cpp
    view/trackspanel/addeffectmenumodel.h
    view/trackspanel/realtimeeffectmenumodelbase.cpp
    view/trackspanel/realtimeeffectmenumodelbase.h
    view/trackspanel/trackcontextmenumodel.cpp
    view/trackspanel/trackcontextmenumodel.h
    view/trackspanel/realtimeeffectlistitemmenumodel.cpp
    view/trackspanel/realtimeeffectlistitemmenumodel.h
    view/trackspanel/realtimeeffectlistitemmodel.cpp
    view/trackspanel/realtimeeffectlistitemmodel.h
    view/trackspanel/realtimeeffectlistmodel.cpp
    view/trackspanel/realtimeeffectlistmodel.h
    view/trackspanel/realtimeeffectsectionmodel.cpp
    view/trackspanel/realtimeeffectsectionmodel.h
    view/trackspanel/paneltrackslistmodel.cpp
    view/trackspanel/paneltrackslistmodel.h
    view/trackspanel/addnewtrackpopupmodel.cpp
    view/trackspanel/addnewtrackpopupmodel.h
    view/trackspanel/trackitem.cpp
    view/trackspanel/trackitem.h
    view/trackspanel/wavetrackitem.cpp
    view/trackspanel/wavetrackitem.h
    view/trackspanel/labeltrackitem.cpp
    view/trackspanel/labeltrackitem.h

    view/tracksitemsview/trackclipslistmodel.cpp
    view/tracksitemsview/trackclipslistmodel.h
    view/tracksitemsview/viewtrackitem.cpp
    view/tracksitemsview/viewtrackitem.h
    view/tracksitemsview/trackitemslistmodel.cpp
    view/tracksitemsview/trackitemslistmodel.h
    view/tracksitemsview/trackclipitem.cpp
    view/tracksitemsview/trackclipitem.h
    view/tracksitemsview/tracklabelitem.cpp
    view/tracksitemsview/tracklabelitem.h
    view/tracksitemsview/wavepainterproxy.cpp
    view/tracksitemsview/wavepainterproxy.h
    view/tracksitemsview/iwavepainter.h
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
    view/tracksitemsview/tracklabelslistmodel.cpp
    view/tracksitemsview/tracklabelslistmodel.h
    view/tracksitemsview/tracklabelslayoutmanager.cpp
    view/tracksitemsview/tracklabelslayoutmanager.h
    view/tracksitemsview/clipcontextmenumodel.cpp
    view/tracksitemsview/clipcontextmenumodel.h
    view/tracksitemsview/multiclipcontextmenumodel.cpp
    view/tracksitemsview/multiclipcontextmenumodel.h
    view/tracksitemsview/labelcontextmenumodel.cpp
    view/tracksitemsview/labelcontextmenumodel.h
    view/tracksitemsview/viewtrackslistmodel.cpp
    view/tracksitemsview/viewtrackslistmodel.h
    view/tracksitemsview/waveview.cpp
    view/tracksitemsview/waveview.h
    view/tracksitemsview/selectionviewcontroller.cpp
    view/tracksitemsview/selectionviewcontroller.h
    view/tracksitemsview/pitchandspeedchangemodel.cpp
    view/tracksitemsview/pitchandspeedchangemodel.h
    view/tracksitemsview/selectioncontextmenumodel.cpp
    view/tracksitemsview/selectioncontextmenumodel.h
    view/tracksitemsview/canvascontextmenumodel.cpp
    view/tracksitemsview/canvascontextmenumodel.h
    view/tracksitemsview/splittoolcontroller.cpp
    view/tracksitemsview/splittoolcontroller.h
    view/tracksitemsview/mousehelper.cpp
    view/tracksitemsview/mousehelper.h
    view/tracksitemsview/dropcontroller.cpp
    view/tracksitemsview/dropcontroller.h
    view/tracksitemsview/labeleditor/labelstableviewmodel.cpp
    view/tracksitemsview/labeleditor/labelstableviewmodel.h
    view/tracksitemsview/labeleditor/addnewlabeltrackmodel.cpp
    view/tracksitemsview/labeleditor/addnewlabeltrackmodel.h
    view/tracksitemsview/labeleditor/labelstableviewverticalheader.cpp
    view/tracksitemsview/labeleditor/labelstableviewverticalheader.h
    view/tracksitemsview/labeleditor/labelstableviewtrackcell.cpp
    view/tracksitemsview/labeleditor/labelstableviewtrackcell.h
    view/tracksitemsview/labeleditor/labelstableviewtimecodecell.cpp
    view/tracksitemsview/labeleditor/labelstableviewtimecodecell.h

    view/timeline/timelinecontext.cpp
    view/timeline/timelinecontext.h
    view/timeline/irulerformat.h
    view/timeline/timeformat.cpp
    view/timeline/timeformat.h
    view/timeline/beatsmeasuresformat.cpp
    view/timeline/beatsmeasuresformat.h
    view/timeline/snaptimeformatter.cpp
    view/timeline/snaptimeformatter.h
    view/timeline/timelineruler.cpp
    view/timeline/timelineruler.h
    view/timeline/timelinecontextmenumodel.cpp
    view/timeline/timelinecontextmenumodel.h
    view/timeline/gridlines.cpp
    view/timeline/gridlines.h
    view/timeline/playregioncontroller.cpp
    view/timeline/playregioncontroller.h
    view/timeline/playregionmodel.cpp
    view/timeline/playregionmodel.h

    view/playcursor/playcursorcontroller.cpp
    view/playcursor/playcursorcontroller.h
    view/playcursor/playpositionactioncontroller.cpp
    view/playcursor/playpositionactioncontroller.h

    view/statusbar/selectionstatusmodel.cpp
    view/statusbar/selectionstatusmodel.h

    view/historypanel/historypanelmodel.cpp
    view/historypanel/historypanelmodel.h

    view/trackruler/trackrulermodel.cpp
    view/trackruler/trackrulermodel.h
    QML_FILES
        ${PROJECTSCENE_QML_FILES}
)

fixup_qml_module_dependencies(projectscene_qml)

target_include_directories(projectscene_qml PRIVATE
    view/common
    view/toolbars
    view/trackspanel
    view/tracksitemsview
    view/tracksitemsview/labeleditor
    view/timeline
    view/playcursor
    view/statusbar
    view/historypanel
    view/trackruler
    ${CMAKE_SOURCE_DIR}/au3/libraries
    ${CLIPSVIEW_V1_INC}
)

target_compile_definitions(projectscene_qml PRIVATE
    ${CLIPSVIEW_V1_DEF}
)

target_link_libraries(projectscene_qml PRIVATE
    au3wrap
    trackedit
    spectrogram
    uicomponents
)

set_target_properties(projectscene_qml PROPERTIES
    UNITY_BUILD OFF
)
