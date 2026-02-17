#
# Audacity: A Digital Audio Editor
#

set(PS_ROOT ${CMAKE_CURRENT_LIST_DIR}/../../..)

muse_create_qml_module(projectscene_qml NO_UNITY FOR projectscene)

qt_add_qml_module(projectscene_qml
    URI Audacity.ProjectScene
    VERSION 1.0
    SOURCES
        # types (header-only, from main module dir via include path)
        ${PS_ROOT}/types/projectscenetypes.h

        # common/
        common/tracksviewstatemodel.cpp common/tracksviewstatemodel.h
        common/trackviewstatemodel.cpp common/trackviewstatemodel.h
        common/customcursor.cpp common/customcursor.h

        # toolbars/
        toolbars/projecttoolbarmodel.cpp toolbars/projecttoolbarmodel.h
        toolbars/undoredotoolbarmodel.cpp toolbars/undoredotoolbarmodel.h
        toolbars/playbacktoolbarmodel.cpp toolbars/playbacktoolbarmodel.h
        toolbars/playbacktoolbarcustomisemodel.cpp toolbars/playbacktoolbarcustomisemodel.h
        toolbars/playbacktoolbarcustomiseitem.cpp toolbars/playbacktoolbarcustomiseitem.h
        toolbars/snaptoolbaritem.cpp toolbars/snaptoolbaritem.h
        toolbars/workspacestoolbarmodel.cpp toolbars/workspacestoolbarmodel.h
        toolbars/audiosetupcontextmenumodel.cpp toolbars/audiosetupcontextmenumodel.h

        # trackspanel/
        trackspanel/trackitem.cpp trackspanel/trackitem.h
        trackspanel/wavetrackitem.cpp trackspanel/wavetrackitem.h
        trackspanel/labeltrackitem.cpp trackspanel/labeltrackitem.h
        trackspanel/paneltrackslistmodel.cpp trackspanel/paneltrackslistmodel.h
        trackspanel/addeffectmenumodel.cpp trackspanel/addeffectmenumodel.h
        trackspanel/addnewtrackpopupmodel.cpp trackspanel/addnewtrackpopupmodel.h
        trackspanel/trackcontextmenumodel.cpp trackspanel/trackcontextmenumodel.h
        trackspanel/realtimeeffectlistitemmenumodel.cpp trackspanel/realtimeeffectlistitemmenumodel.h
        trackspanel/realtimeeffectlistmodel.cpp trackspanel/realtimeeffectlistmodel.h
        trackspanel/realtimeeffectlistitemmodel.cpp trackspanel/realtimeeffectlistitemmodel.h
        trackspanel/realtimeeffectmenumodelbase.cpp trackspanel/realtimeeffectmenumodelbase.h
        trackspanel/realtimeeffectsectionmodel.cpp trackspanel/realtimeeffectsectionmodel.h

        # tracksitemsview/
        tracksitemsview/viewtrackitem.cpp tracksitemsview/viewtrackitem.h
        tracksitemsview/trackitemslistmodel.cpp tracksitemsview/trackitemslistmodel.h
        tracksitemsview/trackclipslistmodel.cpp tracksitemsview/trackclipslistmodel.h
        tracksitemsview/trackclipitem.cpp tracksitemsview/trackclipitem.h
        tracksitemsview/tracklabelslistmodel.cpp tracksitemsview/tracklabelslistmodel.h
        tracksitemsview/tracklabelslayoutmanager.cpp tracksitemsview/tracklabelslayoutmanager.h
        tracksitemsview/tracklabelitem.cpp tracksitemsview/tracklabelitem.h
        tracksitemsview/clipcontextmenumodel.cpp tracksitemsview/clipcontextmenumodel.h
        tracksitemsview/multiclipcontextmenumodel.cpp tracksitemsview/multiclipcontextmenumodel.h
        tracksitemsview/labelcontextmenumodel.cpp tracksitemsview/labelcontextmenumodel.h
        tracksitemsview/viewtrackslistmodel.cpp tracksitemsview/viewtrackslistmodel.h
        tracksitemsview/waveview.cpp tracksitemsview/waveview.h
        tracksitemsview/selectionviewcontroller.cpp tracksitemsview/selectionviewcontroller.h
        tracksitemsview/pitchandspeedchangemodel.cpp tracksitemsview/pitchandspeedchangemodel.h
        tracksitemsview/selectioncontextmenumodel.cpp tracksitemsview/selectioncontextmenumodel.h
        tracksitemsview/canvascontextmenumodel.cpp tracksitemsview/canvascontextmenumodel.h
        tracksitemsview/splittoolcontroller.cpp tracksitemsview/splittoolcontroller.h
        tracksitemsview/mousehelper.cpp tracksitemsview/mousehelper.h
        tracksitemsview/dropcontroller.cpp tracksitemsview/dropcontroller.h

        # tracksitemsview/labeleditor/
        tracksitemsview/labeleditor/labelstableviewmodel.cpp tracksitemsview/labeleditor/labelstableviewmodel.h
        tracksitemsview/labeleditor/labelstableviewverticalheader.cpp tracksitemsview/labeleditor/labelstableviewverticalheader.h
        tracksitemsview/labeleditor/labelstableviewtrackcell.cpp tracksitemsview/labeleditor/labelstableviewtrackcell.h
        tracksitemsview/labeleditor/labelstableviewtimecodecell.cpp tracksitemsview/labeleditor/labelstableviewtimecodecell.h
        tracksitemsview/labeleditor/addnewlabeltrackmodel.cpp tracksitemsview/labeleditor/addnewlabeltrackmodel.h

        # timeline/
        timeline/timelinecontext.cpp timeline/timelinecontext.h
        timeline/timelineruler.cpp timeline/timelineruler.h
        timeline/irulerformat.h
        timeline/timeformat.cpp timeline/timeformat.h
        timeline/beatsmeasuresformat.cpp timeline/beatsmeasuresformat.h
        timeline/timelinecontextmenumodel.cpp timeline/timelinecontextmenumodel.h
        timeline/gridlines.cpp timeline/gridlines.h
        timeline/playregioncontroller.cpp timeline/playregioncontroller.h
        timeline/playregionmodel.cpp timeline/playregionmodel.h
        timeline/snaptimeformatter.cpp timeline/snaptimeformatter.h

        # playcursor/
        playcursor/playcursorcontroller.cpp playcursor/playcursorcontroller.h
        playcursor/playpositionactioncontroller.cpp playcursor/playpositionactioncontroller.h

        # statusbar/
        statusbar/selectionstatusmodel.cpp statusbar/selectionstatusmodel.h

        # historypanel/
        historypanel/historypanelmodel.cpp historypanel/historypanelmodel.h

        # trackruler/
        trackruler/itrackruler.h
        trackruler/trackrulermodel.cpp trackruler/trackrulermodel.h
        trackruler/linearbaseruler.cpp trackruler/linearbaseruler.h
        trackruler/linearmonoruler.cpp trackruler/linearmonoruler.h
        trackruler/linearstereoruler.cpp trackruler/linearstereoruler.h
        trackruler/dblinearbaseruler.cpp trackruler/dblinearbaseruler.h
        trackruler/dblinearmonoruler.cpp trackruler/dblinearmonoruler.h
        trackruler/dblinearstereoruler.cpp trackruler/dblinearstereoruler.h
        trackruler/dblogbaseruler.cpp trackruler/dblogbaseruler.h
        trackruler/dblogmonoruler.cpp trackruler/dblogmonoruler.h
        trackruler/dblogstereoruler.cpp trackruler/dblogstereoruler.h

    QML_FILES
        # historypanel/
        historypanel/HistoryPanel.qml
        # statusbar/
        statusbar/SelectionStatus.qml
        # toolbars/
        toolbars/PlaybackToolBar.qml
        toolbars/ProjectToolBar.qml
        toolbars/UndoRedoToolBar.qml
        toolbars/WorkspacesToolBar.qml
        toolbars/internal/CustomiseView.qml
        toolbars/internal/PlaybackToolBarActionDelegate.qml
        toolbars/internal/PlaybackToolBarCustomisePopup.qml
        # trackruler/
        trackruler/SpectrogramRuler.qml
        trackruler/TrackRulerCustomizePopup.qml
        trackruler/WaveformRuler.qml
        # tracksitemsview/
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
        # tracksitemsview/labeleditor/
        tracksitemsview/labeleditor/AddNewLabelTrackDialog.qml
        tracksitemsview/labeleditor/LabelEditorDialog.qml
        tracksitemsview/labeleditor/LabelEditorLabelsTableView.qml
        tracksitemsview/labeleditor/LabelEditorTopPanel.qml
        # tracksitemsview/pitchandspeed/
        tracksitemsview/pitchandspeed/GeneralSection.qml
        tracksitemsview/pitchandspeed/PitchAndSpeedChangeDialog.qml
        tracksitemsview/pitchandspeed/PitchSection.qml
        tracksitemsview/pitchandspeed/PropertyView.qml
        tracksitemsview/pitchandspeed/SpeedSection.qml
        # trackspanel/
        trackspanel/AddNewTrackPopup.qml
        trackspanel/EditableLabel.qml
        trackspanel/LabelTrackItem.qml
        trackspanel/RealtimeEffectListItem.qml
        trackspanel/RealtimeEffectListItemButtonBackground.qml
        trackspanel/TrackEffectList.qml
        trackspanel/TrackEffectsSection.qml
        trackspanel/TrackItem.qml
        trackspanel/TrackSelectionBar.qml
        trackspanel/TracksPanel.qml
        trackspanel/TracksTitleBar.qml
        trackspanel/TracksTitleBarBackground.qml
        trackspanel/WaveTrackItem.qml
        # trackspanel/audio/
        trackspanel/audio/KnobControl.qml
        trackspanel/audio/PanKnob.qml
        trackspanel/audio/PanTooltip.qml
        trackspanel/audio/VolumePressureMeter.qml
        trackspanel/audio/VolumePressureRuler.qml
        trackspanel/audio/VolumeSlider.qml
        trackspanel/audio/VolumeTooltip.qml
    IMPORTS
        TARGET muse_ui_qml
        TARGET muse_uicomponents_qml
        TARGET muse_graphicaleffects_qml
)

fixup_qml_module_dependencies(projectscene_qml)

target_link_libraries(projectscene_qml PRIVATE
    au3wrap
    trackedit
    uicomponents
    muse_workspace_qml
)

target_include_directories(projectscene_qml PRIVATE
    common toolbars trackspanel tracksitemsview tracksitemsview/labeleditor
    timeline playcursor statusbar historypanel trackruler
    # For non-moved files (iwavepainter.h, au3/ headers)
    ${CMAKE_CURRENT_LIST_DIR}/../../../view/tracksitemsview
)
