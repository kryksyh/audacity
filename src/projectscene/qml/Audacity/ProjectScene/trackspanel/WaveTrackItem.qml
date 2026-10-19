/*
* Audacity: A Digital Audio Editor
*/
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

import Muse.Ui
import Muse.UiComponents

import Audacity.ProjectScene
import Audacity.TrackEdit
import Audacity.Automation

import "audio"

TrackItem {
    id: root

    TrackRoutingModel {
        id: routing
        trackId: root.item ? root.item.trackId : -1
    }

    TrackAutomationModel {
        id: automation
        trackId: root.item ? root.item.trackId : -1
    }

    PianoRollModel {
        id: midi
        trackId: root.item ? root.item.trackId : -1
    }

    headerTrailingControlsComponent: Component {
        RowLayout {
            opacity: root.collapsed ? 1 : 0
            visible: opacity !== 0

            Behavior on opacity {
                OpacityAnimator {
                    duration: 100
                }
            }

            Loader {
                sourceComponent: trackControlButtons
            }
        }
    }

    extraControlsComponent: Component {
        ColumnLayout {
            spacing: 2

            RowLayout {
                id: topRow

                Layout.fillWidth: true

                spacing: 16

                opacity: root.collapsed ? 0 : 1
                visible: opacity !== 0
                Behavior on opacity {
                    OpacityAnimator {
                        duration: 100
                    }
                }

                PanKnob {
                    id: panKnob

                    value: Boolean(root.item) ? root.item.pan : 0

                    navigation.panel: root.headerNavigationPanel
                    navigation.order: root.extraControlsNavigationStart
                    navigation.enabled: !root.collapsed
                    //: Noun: accessibility name of the pan knob
                    navigation.accessible.name: qsTrc("projectscene", "Pan")

                    onNewPanRequested: function (newValue, completed) {
                        if (Boolean(root.item)) {
                            root.item.setPan(newValue, completed)
                        }
                    }
                }

                VolumeSlider {
                    id: volumeSlider

                    value: Boolean(root.item) ? root.item.volumeLevel : 0

                    navigation.panel: root.headerNavigationPanel
                    navigation.order: panKnob.navigation.order + 1
                    navigation.enabled: !root.collapsed
                    navigation.accessible.name: qsTrc("projectscene", "Track volume")

                    onNewVolumeRequested: function (newValue, completed) {
                        if (Boolean(root.item)) {
                            root.item.setVolumeLevel(newValue, completed)
                        }
                    }

                    Component.onCompleted: {
                        root.extraControlsNavigationEnd = Qt.binding(function () {
                            return volumeSlider.navigation.order
                        })
                    }
                }

                Loader {
                    sourceComponent: trackControlButtons
                }
            }

            RowLayout {
                Layout.fillWidth: true
                spacing: 4

                opacity: topRow.visible && root.height > root.mapFromItem(this, 0, height + bottomSeparatorHeight).y ? 1 : 0
                visible: opacity !== 0
                Behavior on opacity {
                    OpacityAnimator {
                        duration: 100
                    }
                }

                FlatButton {
                    id: effectsButton
                    Layout.fillWidth: true
                    Layout.preferredHeight: 24
                    minWidth: 50
                    text: qsTrc("projectscene", "Effects")
                    toolTipDescription: qsTrc("projectscene", "Opens effects panel")
                    navigation.panel: root.headerNavigationPanel
                    navigation.order: root.headerTrailingControlsNavigationEnd + 1
                    navigation.enabled: topRow.visible
                    onClicked: root.openEffectsRequested()
                }

                FlatButton {
                    Layout.preferredHeight: 24
                    minWidth: 48
                    text: qsTrc("routing", "Route")
                    navigation.panel: root.headerNavigationPanel
                    navigation.order: effectsButton.navigation.order + 1
                    navigation.enabled: topRow.visible
                    onClicked: routing.openEditor()
                }

                FlatButton {
                    Layout.preferredWidth: 28
                    Layout.preferredHeight: 24
                    minWidth: 24
                    icon: IconCode.AUTOMATION
                    toolTipTitle: qsTrc("automation", "Automation")
                    navigation.panel: root.headerNavigationPanel
                    navigation.order: effectsButton.navigation.order + 2
                    navigation.enabled: topRow.visible
                    onClicked: automation.openEditor()
                }

                FlatButton {
                    Layout.fillWidth: true
                    Layout.preferredHeight: 24
                    minWidth: 60
                    visible: root.item && root.item.trackType === TrackType.MIDI
                    text: qsTrc("pianoroll", "Piano roll")
                    toolTipDescription: qsTrc("pianoroll", "Edit MIDI notes and instrument settings")
                    navigation.panel: root.headerNavigationPanel
                    navigation.order: effectsButton.navigation.order + 3
                    navigation.enabled: topRow.visible && visible
                    onClicked: {
                        midi.init()
                        const clips = midi.clips
                        const id = clips.length > 0 ? clips[0].id : midi.addClip(0)
                        if (id > 0) {
                            midi.openEditor(id)
                        }
                    }
                }
            }
        }
    }

    rightSideContainerComponent: Component {
        Item {
            width: parent.width
            height: parent.height

            Row {
                id: volumePressureContainer

                property int indicatorWidth: 7

                anchors.top: parent.top
                anchors.bottom: parent.bottom
                anchors.horizontalCenter: parent.horizontalCenter

                spacing: 2

                function clearMeters() {
                    leftOrMonoVolumePressureMeter.reset()
                    leftOrMonoVolumePressureMeter.resetClipped()
                    rightVolumePressureMeter.reset()
                    rightVolumePressureMeter.resetClipped()
                }

                TapHandler {
                    onTapped: volumePressureContainer.clearMeters()
                }

                VolumePressureMeter {
                    id: leftOrMonoVolumePressureMeter
                    anchors.top: parent.top
                    anchors.bottom: parent.bottom
                    indicatorWidth: parent.indicatorWidth
                    meterModel: trackViewState.meterModel
                    currentVolumePressure: Boolean(root.item) ? root.item.leftChannelPressure : 0
                    currentRMS: Boolean(root.item) ? root.item.leftChannelRMS : 0
                }

                VolumePressureMeter {
                    id: rightVolumePressureMeter
                    anchors.top: parent.top
                    anchors.bottom: parent.bottom
                    indicatorWidth: parent.indicatorWidth
                    meterModel: trackViewState.meterModel
                    currentVolumePressure: Boolean(root.item) ? root.item.rightChannelPressure : 0
                    currentRMS: Boolean(root.item) ? root.item.rightChannelRMS : 0
                }

                Connections {
                    target: root.trackViewState

                    function onIsRecordingChanged() {
                        if (trackViewState.isRecording) {
                            leftOrMonoVolumePressureMeter.resetClipped()
                            rightVolumePressureMeter.resetClipped()
                        }

                        leftOrMonoVolumePressureMeter.reset()
                        rightVolumePressureMeter.reset()
                    }

                    function onIsPlayingChanged() {
                        if (trackViewState.isPlaying) {
                            volumePressureContainer.clearMeters()
                        }
                    }
                }

                Connections {
                    target: root

                    function onIsFocusedChanged() {
                        if (!root.isFocused) {
                            volumePressureContainer.clearMeters()
                        }
                    }
                }
            }

            states: [
                State {
                    when: Boolean(root.item) && root.item.channelCount === 1
                    name: "mono"
                    PropertyChanges {
                        target: volumePressureContainer
                        indicatorWidth: 8
                    }
                    PropertyChanges {
                        target: rightVolumePressureMeter
                        visible: false
                    }
                },
                State {
                    when: Boolean(root.item) && root.item.channelCount === 2
                    name: "stereo"
                    PropertyChanges {
                        target: volumePressureContainer
                        indicatorWidth: 7
                    }
                    PropertyChanges {
                        target: rightVolumePressureMeter
                        visible: true
                    }
                }
            ]
        }
    }

    Component {
        id: trackControlButtons

        RowLayout {
            spacing: 4

            FlatToggleButton {
                id: muteButton

                Layout.preferredWidth: 20
                Layout.preferredHeight: Layout.preferredWidth

                icon: IconCode.MUTE
                checked: Boolean(root.item) ? root.item.muted : false

                navigation.panel: root.headerNavigationPanel
                navigation.order: root.headerTrailingControlsNavigationStart
                //: Accessibility name of the mute button
                navigation.accessible.name: qsTrc("projectscene", "Mute")
                navigation.accessible.role: MUAccessible.CheckBox

                onToggled: {
                    if (Boolean(root.item)) {
                        root.item.muted = !checked
                    }
                }
            }

            FlatToggleButton {
                id: soloButton

                Layout.preferredWidth: 20
                Layout.preferredHeight: Layout.preferredWidth

                icon: IconCode.SOLO
                checked: Boolean(root.item) ? root.item.solo : false

                navigation.panel: root.headerNavigationPanel
                navigation.order: muteButton.navigation.order + 1
                //: Accessibility name of the solo button
                navigation.accessible.name: qsTrc("projectscene", "Solo")
                navigation.accessible.role: MUAccessible.CheckBox

                onToggled: {
                    if (Boolean(root.item)) {
                        root.item.solo = !checked
                    }
                }

                Component.onCompleted: {
                    root.headerTrailingControlsNavigationEnd = Qt.binding(function () {
                        return soloButton.navigation.order
                    })
                }
            }
        }
    }
}
