muse_create_qml_module(projectscene_qml FOR projectscene)

target_compile_definitions(projectscene_qml PRIVATE AU_PROJECTSCENE_QML_MODULE)

target_include_directories(projectscene_qml PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../../../../..)
target_include_directories(projectscene_qml PRIVATE ${CMAKE_SOURCE_DIR}/au3/libraries)

# Include AU3 definitions for API macros and wxWidgets
include(${AUDACITY_ROOT}/au3defs.cmake)

# Add AU3 API definitions for headers included from AU3 libraries
target_compile_definitions(projectscene_qml PRIVATE ${AU3_API_DEFS})

# Add wxWidgets include path for AU3 headers that depend on wxWidgets
target_include_directories(projectscene_qml PRIVATE /usr/local/include/wx-3.3)

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
        view/toolbars/snaptoolbaritem.cpp
        view/toolbars/snaptoolbaritem.h
        view/toolbars/workspacestoolbarmodel.cpp
        view/toolbars/workspacestoolbarmodel.h
        view/toolbars/audiosetupcontextmenumodel.cpp
        view/toolbars/audiosetupcontextmenumodel.h

        view/trackspanel/addeffectmenumodel.cpp
        view/trackspanel/addeffectmenumodel.h
        view/trackspanel/trackitem.cpp
        view/trackspanel/trackitem.h
        view/trackspanel/wavetrackitem.cpp
        view/trackspanel/wavetrackitem.h
        view/trackspanel/labeltrackitem.cpp
        view/trackspanel/labeltrackitem.h
        view/trackspanel/trackcontextmenumodel.cpp
        view/trackspanel/trackcontextmenumodel.h
        view/trackspanel/realtimeeffectlistitemmenumodel.cpp
        view/trackspanel/realtimeeffectlistitemmenumodel.h
        view/trackspanel/realtimeeffectlistmodel.cpp
        view/trackspanel/realtimeeffectlistmodel.h
        view/trackspanel/realtimeeffectlistitemmodel.cpp
        view/trackspanel/realtimeeffectlistitemmodel.h
        view/trackspanel/realtimeeffectmenumodelbase.cpp
        view/trackspanel/realtimeeffectmenumodelbase.h
        view/trackspanel/realtimeeffectsectionmodel.cpp
        view/trackspanel/realtimeeffectsectionmodel.h
        view/trackspanel/paneltrackslistmodel.cpp
        view/trackspanel/paneltrackslistmodel.h
        view/trackspanel/addnewtrackpopupmodel.cpp
        view/trackspanel/addnewtrackpopupmodel.h

        view/tracksitemsview/viewtrackitem.cpp
        view/tracksitemsview/viewtrackitem.h
        view/tracksitemsview/trackitemslistmodel.cpp
        view/tracksitemsview/trackitemslistmodel.h
        view/tracksitemsview/trackclipslistmodel.cpp
        view/tracksitemsview/trackclipslistmodel.h
        view/tracksitemsview/trackclipitem.cpp
        view/tracksitemsview/trackclipitem.h
        view/tracksitemsview/tracklabelslistmodel.cpp
        view/tracksitemsview/tracklabelslistmodel.h
        view/tracksitemsview/tracklabelslayoutmanager.cpp
        view/tracksitemsview/tracklabelslayoutmanager.h
        view/tracksitemsview/tracklabelitem.cpp
        view/tracksitemsview/tracklabelitem.h
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
        view/tracksitemsview/iwavepainter.h
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
        view/tracksitemsview/labeleditor/labelstableviewverticalheader.cpp
        view/tracksitemsview/labeleditor/labelstableviewverticalheader.h
        view/tracksitemsview/labeleditor/labelstableviewtrackcell.cpp
        view/tracksitemsview/labeleditor/labelstableviewtrackcell.h
        view/tracksitemsview/labeleditor/labelstableviewtimecodecell.cpp
        view/tracksitemsview/labeleditor/labelstableviewtimecodecell.h
        view/tracksitemsview/labeleditor/addnewlabeltrackmodel.cpp
        view/tracksitemsview/labeleditor/addnewlabeltrackmodel.h

        view/timeline/timelinecontext.cpp
        view/timeline/timelinecontext.h
        view/timeline/timelineruler.cpp
        view/timeline/timelineruler.h
        view/timeline/irulerformat.h
        view/timeline/timeformat.cpp
        view/timeline/timeformat.h
        view/timeline/beatsmeasuresformat.cpp
        view/timeline/beatsmeasuresformat.h
        view/timeline/timelinecontextmenumodel.cpp
        view/timeline/timelinecontextmenumodel.h
        view/timeline/gridlines.cpp
        view/timeline/gridlines.h
        view/timeline/playregioncontroller.cpp
        view/timeline/playregioncontroller.h
        view/timeline/playregionmodel.cpp
        view/timeline/playregionmodel.h
        view/timeline/snaptimeformatter.cpp
        view/timeline/snaptimeformatter.h

        view/playcursor/playcursorcontroller.cpp
        view/playcursor/playcursorcontroller.h
        view/playcursor/playpositionactioncontroller.cpp
        view/playcursor/playpositionactioncontroller.h

        view/statusbar/selectionstatusmodel.cpp
        view/statusbar/selectionstatusmodel.h

        view/historypanel/historypanelmodel.cpp
        view/historypanel/historypanelmodel.h

        view/trackruler/dblogmonoruler.cpp
        view/trackruler/dblogmonoruler.h
        view/trackruler/dblogstereoruler.cpp
        view/trackruler/dblogstereoruler.h
        view/trackruler/dblogbaseruler.cpp
        view/trackruler/dblogbaseruler.h
        view/trackruler/dblinearmonoruler.cpp
        view/trackruler/dblinearmonoruler.h
        view/trackruler/dblinearstereoruler.cpp
        view/trackruler/dblinearstereoruler.h
        view/trackruler/dblinearbaseruler.cpp
        view/trackruler/dblinearbaseruler.h
        view/trackruler/itrackruler.h
        view/trackruler/linearstereoruler.cpp
        view/trackruler/linearstereoruler.h
        view/trackruler/linearmonoruler.cpp
        view/trackruler/linearmonoruler.h
        view/trackruler/linearbaseruler.cpp
        view/trackruler/linearbaseruler.h
        view/trackruler/trackrulermodel.cpp
        view/trackruler/trackrulermodel.h
    QML_FILES
        tracksitemsview/TracksItemsView.qml
        tracksitemsview/TrackClipsContainer.qml
        tracksitemsview/ClipItem.qml
        tracksitemsview/Timeline.qml
        tracksitemsview/ItemsSelection.qml
        tracksitemsview/TrackLabelsContainer.qml
        tracksitemsview/TrackItemsContainer.qml
        tracksitemsview/LabelItem.qml
        tracksitemsview/LabelHeader.qml
        tracksitemsview/LabelEar.qml
        tracksitemsview/LabelStalk.qml
        tracksitemsview/ImportDropArea.qml
        trackspanel/AddNewTrackPopup.qml
        trackspanel/RealtimeEffectListItem.qml
        trackspanel/RealtimeEffectListItemButtonBackground.qml
        trackspanel/TrackItem.qml
        trackspanel/WaveTrackItem.qml
        trackspanel/LabelTrackItem.qml
        trackspanel/TrackSelectionBar.qml
        trackspanel/TracksTitleBar.qml
        trackspanel/TracksTitleBarBackground.qml
        trackspanel/TracksPanel.qml
        trackspanel/TrackEffectList.qml
        trackspanel/TrackEffectsSection.qml
        trackspanel/EditableLabel.qml
        trackspanel/audio/KnobControl.qml
        trackspanel/audio/PanKnob.qml
        trackspanel/audio/PanTooltip.qml
        trackspanel/audio/VolumePressureMeter.qml
        trackspanel/audio/VolumePressureRuler.qml
        trackspanel/audio/VolumeSlider.qml
        trackspanel/audio/VolumeTooltip.qml
        toolbars/ProjectToolBar.qml
        toolbars/UndoRedoToolBar.qml
        tracksitemsview/PlayCursorLine.qml
        tracksitemsview/PlayCursorHead.qml
        tracksitemsview/VerticalRulersPanel.qml
        statusbar/SelectionStatus.qml
        toolbars/PlaybackToolBar.qml
        toolbars/internal/CustomiseView.qml
        toolbars/internal/PlaybackToolBarActionDelegate.qml
        toolbars/internal/PlaybackToolBarCustomisePopup.qml
        tracksitemsview/ClipItemSmall.qml
        tracksitemsview/ClipPreview.qml
        tracksitemsview/ClipHandles.qml
        tracksitemsview/ChannelSplitter.qml
        tracksitemsview/ClipItemPropertyButton.qml
        tracksitemsview/pitchandspeed/PitchAndSpeedChangeDialog.qml
        tracksitemsview/pitchandspeed/PropertyView.qml
        tracksitemsview/pitchandspeed/PitchSection.qml
        tracksitemsview/pitchandspeed/GeneralSection.qml
        tracksitemsview/pitchandspeed/SpeedSection.qml
        historypanel/HistoryPanel.qml
        toolbars/WorkspacesToolBar.qml
        tracksitemsview/PlayRegion.qml
        tracksitemsview/LoopRegionGuideline.qml
        trackruler/TrackRulerCustomizePopup.qml
        trackruler/WaveformRuler.qml
        trackruler/SpectrogramRuler.qml
        tracksitemsview/labeleditor/LabelEditorDialog.qml
        tracksitemsview/labeleditor/LabelEditorTopPanel.qml
        tracksitemsview/labeleditor/LabelEditorLabelsTableView.qml
        tracksitemsview/labeleditor/AddNewLabelTrackDialog.qml
    IMPORTS
        TARGET muse_ui_qml
        TARGET muse_uicomponents_qml
)

fixup_qml_module_dependencies(projectscene_qml)

# Add AU3 API definitions for headers included from AU3 libraries
target_compile_definitions(projectscene_qml PRIVATE ${AU3_API_DEFS})

# Add wxWidgets include path for AU3 headers that depend on wxWidgets
target_include_directories(projectscene_qml PRIVATE /usr/local/include/wx-3.3)

# Necessary for the auto-generated sources
target_include_directories(projectscene_qml PRIVATE
    view/common
    view/toolbars
    view/trackspanel
    view/tracksitemsview
    view/timeline
    view/playcursor
    view/statusbar
    view/historypanel
    view/trackruler
)