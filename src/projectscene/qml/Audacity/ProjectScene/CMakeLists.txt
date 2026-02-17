
muse_create_qml_module(projectscene_qml FOR projectscene)

qt_add_qml_module(projectscene_qml
    URI Audacity.ProjectScene
    VERSION 1.0
    SOURCES
        common/tracksviewstatemodel.cpp
        common/tracksviewstatemodel.h
        common/trackviewstatemodel.cpp
        common/trackviewstatemodel.h
        common/customcursor.cpp
        common/customcursor.h

        historypanel/historypanelmodel.cpp
        historypanel/historypanelmodel.h

        playcursor/playcursorcontroller.cpp
        playcursor/playcursorcontroller.h
        playcursor/playpositionactioncontroller.cpp
        playcursor/playpositionactioncontroller.h

        statusbar/selectionstatusmodel.cpp
        statusbar/selectionstatusmodel.h

        timeline/beatsmeasuresformat.cpp
        timeline/beatsmeasuresformat.h
        timeline/gridlines.cpp
        timeline/gridlines.h
        timeline/irulerformat.h
        timeline/playregioncontroller.cpp
        timeline/playregioncontroller.h
        timeline/playregionmodel.cpp
        timeline/playregionmodel.h
        timeline/snaptimeformatter.cpp
        timeline/snaptimeformatter.h
        timeline/timeformat.cpp
        timeline/timeformat.h
        timeline/timelinecontext.cpp
        timeline/timelinecontext.h
        timeline/timelinecontextmenumodel.cpp
        timeline/timelinecontextmenumodel.h
        timeline/timelineruler.cpp
        timeline/timelineruler.h

        toolbars/audiosetupcontextmenumodel.cpp
        toolbars/audiosetupcontextmenumodel.h
        toolbars/playbacktoolbarcustomiseitem.cpp
        toolbars/playbacktoolbarcustomiseitem.h
        toolbars/playbacktoolbarcustomisemodel.cpp
        toolbars/playbacktoolbarcustomisemodel.h
        toolbars/playbacktoolbarmodel.cpp
        toolbars/playbacktoolbarmodel.h
        toolbars/projecttoolbarmodel.cpp
        toolbars/projecttoolbarmodel.h
        toolbars/snaptoolbaritem.cpp
        toolbars/snaptoolbaritem.h
        toolbars/undoredotoolbarmodel.cpp
        toolbars/undoredotoolbarmodel.h
        toolbars/workspacestoolbarmodel.cpp
        toolbars/workspacestoolbarmodel.h

        trackruler/dblinearbaseruler.cpp
        trackruler/dblinearbaseruler.h
        trackruler/dblinearmonoruler.cpp
        trackruler/dblinearmonoruler.h
        trackruler/dblinearstereoruler.cpp
        trackruler/dblinearstereoruler.h
        trackruler/dblogbaseruler.cpp
        trackruler/dblogbaseruler.h
        trackruler/dblogmonoruler.cpp
        trackruler/dblogmonoruler.h
        trackruler/dblogstereoruler.cpp
        trackruler/dblogstereoruler.h
        trackruler/itrackruler.h
        trackruler/linearbaseruler.cpp
        trackruler/linearbaseruler.h
        trackruler/linearmonoruler.cpp
        trackruler/linearmonoruler.h
        trackruler/linearstereoruler.cpp
        trackruler/linearstereoruler.h
        trackruler/trackrulermodel.cpp
        trackruler/trackrulermodel.h

        tracksitemsview/au3/WaveMetrics.h
        tracksitemsview/au3/WaveformPainter.cpp
        tracksitemsview/au3/WaveformPainter.h
        tracksitemsview/au3/WaveformSettings.cpp
        tracksitemsview/au3/WaveformSettings.h
        tracksitemsview/au3/connectingdotspainter.cpp
        tracksitemsview/au3/connectingdotspainter.h
        tracksitemsview/au3/iau3wavepainter.h
        tracksitemsview/au3/iconnectingdotspainter.h
        tracksitemsview/au3/iminmaxrmspainter.h
        tracksitemsview/au3/isamplespainter.h
        tracksitemsview/au3/minmaxrmspainter.cpp
        tracksitemsview/au3/minmaxrmspainter.h
        tracksitemsview/au3/sampledata.h
        tracksitemsview/au3/samplespainter.cpp
        tracksitemsview/au3/samplespainter.h
        tracksitemsview/au3/samplespainterutils.cpp
        tracksitemsview/au3/samplespainterutils.h
        tracksitemsview/au3/wavepainterutils.cpp
        tracksitemsview/au3/wavepainterutils.h

        tracksitemsview/canvascontextmenumodel.cpp
        tracksitemsview/canvascontextmenumodel.h
        tracksitemsview/clipcontextmenumodel.cpp
        tracksitemsview/clipcontextmenumodel.h
        tracksitemsview/dropcontroller.cpp
        tracksitemsview/dropcontroller.h
        tracksitemsview/iwavepainter.h
        tracksitemsview/labelcontextmenumodel.cpp
        tracksitemsview/labelcontextmenumodel.h

        tracksitemsview/labeleditor/addnewlabeltrackmodel.cpp
        tracksitemsview/labeleditor/addnewlabeltrackmodel.h
        tracksitemsview/labeleditor/labelstableviewmodel.cpp
        tracksitemsview/labeleditor/labelstableviewmodel.h
        tracksitemsview/labeleditor/labelstableviewtimecodecell.cpp
        tracksitemsview/labeleditor/labelstableviewtimecodecell.h
        tracksitemsview/labeleditor/labelstableviewtrackcell.cpp
        tracksitemsview/labeleditor/labelstableviewtrackcell.h
        tracksitemsview/labeleditor/labelstableviewverticalheader.cpp
        tracksitemsview/labeleditor/labelstableviewverticalheader.h

        tracksitemsview/mousehelper.cpp
        tracksitemsview/mousehelper.h
        tracksitemsview/multiclipcontextmenumodel.cpp
        tracksitemsview/multiclipcontextmenumodel.h
        tracksitemsview/pitchandspeedchangemodel.cpp
        tracksitemsview/pitchandspeedchangemodel.h
        tracksitemsview/selectioncontextmenumodel.cpp
        tracksitemsview/selectioncontextmenumodel.h
        tracksitemsview/selectionviewcontroller.cpp
        tracksitemsview/selectionviewcontroller.h
        tracksitemsview/splittoolcontroller.cpp
        tracksitemsview/splittoolcontroller.h
        tracksitemsview/trackclipitem.cpp
        tracksitemsview/trackclipitem.h
        tracksitemsview/trackclipslistmodel.cpp
        tracksitemsview/trackclipslistmodel.h
        tracksitemsview/trackitemslistmodel.cpp
        tracksitemsview/trackitemslistmodel.h
        tracksitemsview/tracklabelitem.cpp
        tracksitemsview/tracklabelitem.h
        tracksitemsview/tracklabelslayoutmanager.cpp
        tracksitemsview/tracklabelslayoutmanager.h
        tracksitemsview/tracklabelslistmodel.cpp
        tracksitemsview/tracklabelslistmodel.h
        tracksitemsview/viewtrackitem.cpp
        tracksitemsview/viewtrackitem.h
        tracksitemsview/viewtrackslistmodel.cpp
        tracksitemsview/viewtrackslistmodel.h
        tracksitemsview/wavepainterproxy.cpp
        tracksitemsview/wavepainterproxy.h
        tracksitemsview/waveview.cpp
        tracksitemsview/waveview.h

        trackspanel/addeffectmenumodel.cpp
        trackspanel/addeffectmenumodel.h
        trackspanel/addnewtrackpopupmodel.cpp
        trackspanel/addnewtrackpopupmodel.h
        trackspanel/labeltrackitem.cpp
        trackspanel/labeltrackitem.h
        trackspanel/paneltrackslistmodel.cpp
        trackspanel/paneltrackslistmodel.h
        trackspanel/realtimeeffectlistitemmenumodel.cpp
        trackspanel/realtimeeffectlistitemmenumodel.h
        trackspanel/realtimeeffectlistitemmodel.cpp
        trackspanel/realtimeeffectlistitemmodel.h
        trackspanel/realtimeeffectlistmodel.cpp
        trackspanel/realtimeeffectlistmodel.h
        trackspanel/realtimeeffectmenumodelbase.cpp
        trackspanel/realtimeeffectmenumodelbase.h
        trackspanel/realtimeeffectsectionmodel.cpp
        trackspanel/realtimeeffectsectionmodel.h
        trackspanel/trackcontextmenumodel.cpp
        trackspanel/trackcontextmenumodel.h
        trackspanel/trackitem.cpp
        trackspanel/trackitem.h
        trackspanel/wavetrackitem.cpp
        trackspanel/wavetrackitem.h

        ${CMAKE_SOURCE_DIR}/src/projectscene/types/projectscenetypes.h

    QML_FILES
        historypanel/HistoryPanel.qml
        statusbar/SelectionStatus.qml

        toolbars/internal/CustomiseView.qml
        toolbars/internal/PlaybackToolBarActionDelegate.qml
        toolbars/internal/PlaybackToolBarCustomisePopup.qml
        toolbars/PlaybackToolBar.qml
        toolbars/ProjectToolBar.qml
        toolbars/UndoRedoToolBar.qml
        toolbars/WorkspacesToolBar.qml

        trackruler/SpectrogramRuler.qml
        trackruler/TrackRulerCustomizePopup.qml
        trackruler/WaveformRuler.qml

        tracksitemsview/labeleditor/AddNewLabelTrackDialog.qml
        tracksitemsview/labeleditor/LabelEditorDialog.qml
        tracksitemsview/labeleditor/LabelEditorLabelsTableView.qml
        tracksitemsview/labeleditor/LabelEditorTopPanel.qml
        tracksitemsview/pitchandspeed/GeneralSection.qml
        tracksitemsview/pitchandspeed/PitchAndSpeedChangeDialog.qml
        tracksitemsview/pitchandspeed/PitchSection.qml
        tracksitemsview/pitchandspeed/PropertyView.qml
        tracksitemsview/pitchandspeed/SpeedSection.qml
        tracksitemsview/ChannelSplitter.qml
        tracksitemsview/ClipHandles.qml
        tracksitemsview/ClipItem.qml
        tracksitemsview/ClipItemPropertyButton.qml
        tracksitemsview/ClipItemSmall.qml
        tracksitemsview/ClipPreview.qml
        tracksitemsview/ImportDropArea.qml
        tracksitemsview/ItemsSelection.qml
        tracksitemsview/LabelEar.qml
        tracksitemsview/LabelHeader.qml
        tracksitemsview/LabelItem.qml
        tracksitemsview/LabelStalk.qml
        tracksitemsview/LoopRegionGuideline.qml
        tracksitemsview/PlayCursorHead.qml
        tracksitemsview/PlayCursorLine.qml
        tracksitemsview/PlayRegion.qml
        tracksitemsview/Timeline.qml
        tracksitemsview/TrackClipsContainer.qml
        tracksitemsview/TrackItemsContainer.qml
        tracksitemsview/TrackLabelsContainer.qml
        tracksitemsview/TracksItemsView.qml
        tracksitemsview/VerticalRulersPanel.qml

        trackspanel/audio/KnobControl.qml
        trackspanel/audio/PanKnob.qml
        trackspanel/audio/PanTooltip.qml
        trackspanel/audio/VolumePressureMeter.qml
        trackspanel/audio/VolumePressureRuler.qml
        trackspanel/audio/VolumeSlider.qml
        trackspanel/audio/VolumeTooltip.qml
        trackspanel/AddNewTrackPopup.qml
        trackspanel/EditableLabel.qml
        trackspanel/LabelTrackItem.qml
        trackspanel/RealtimeEffectListItem.qml
        trackspanel/RealtimeEffectListItemButtonBackground.qml
        trackspanel/TrackEffectList.qml
        trackspanel/TrackEffectsSection.qml
        trackspanel/TrackItem.qml
        trackspanel/TracksPanel.qml
        trackspanel/TracksTitleBar.qml
        trackspanel/TracksTitleBarBackground.qml
        trackspanel/TrackSelectionBar.qml
        trackspanel/WaveTrackItem.qml
)

target_include_directories(projectscene_qml PRIVATE
        ${CMAKE_SOURCE_DIR}/src/projectscene
        ${CMAKE_SOURCE_DIR}/src/projectscene/internal
        ${CMAKE_SOURCE_DIR}/au3/libraries
        ${CMAKE_CURRENT_SOURCE_DIR}/common
        ${CMAKE_CURRENT_SOURCE_DIR}/historypanel
        ${CMAKE_CURRENT_SOURCE_DIR}/playcursor
        ${CMAKE_CURRENT_SOURCE_DIR}/statusbar
        ${CMAKE_CURRENT_SOURCE_DIR}/timeline
        ${CMAKE_CURRENT_SOURCE_DIR}/toolbars
        ${CMAKE_CURRENT_SOURCE_DIR}/trackruler
        ${CMAKE_CURRENT_SOURCE_DIR}/tracksitemsview
        ${CMAKE_CURRENT_SOURCE_DIR}/tracksitemsview/au3
        ${CMAKE_CURRENT_SOURCE_DIR}/tracksitemsview/labeleditor
        ${CMAKE_CURRENT_SOURCE_DIR}/trackspanel
        ${CMAKE_SOURCE_DIR}/src/projectscene/types
)


target_link_libraries(projectscene_qml PRIVATE
    projectscene
    au3wrap
    trackedit
    spectrogram
    uicomponents
)

if(MUSE_MODULE_WORKSPACE)
    target_link_libraries(projectscene_qml PRIVATE muse::workspace_qml)
endif()

set_target_properties(projectscene_qml PROPERTIES UNITY_BUILD OFF)
