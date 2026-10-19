import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Muse.Ui
import Muse.UiComponents
import Audacity.TrackEdit

FocusScope {
    id: root
    required property var routing
    property alias navigationSection: navigationPanel.section
    property alias navigationOrderStart: navigationPanel.order

    TrackFreezeModel {
        id: freezeModel
        trackId: root.routing.trackId
        Component.onCompleted: init()
    }

    NavigationPanel {
        id: navigationPanel
        name: "TrackRoutingControls"
        direction: NavigationPanel.Both
        enabled: root.enabled && root.visible
    }
    Rectangle {
        anchors.fill: parent
        color: ui.theme.backgroundPrimaryColor
    }
    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 12
        spacing: 12
        RowLayout {
            StyledTextLabel {
                text: root.routing.title
            }
            StyledTextLabel {
                text: qsTrc("routing", "Output")
            }
            StyledDropdown {
                Layout.preferredWidth: 220
                model: root.routing.buses
                textRole: "title"
                currentIndex: root.routing.outputIndex
                enabled: root.routing.editable
                navigation.panel: navigationPanel
                navigation.order: 1
                onActivated: function (index) {
                    root.routing.setOutput(index)
                }
            }
            FlatButton {
                text: qsTrc("routing", "Add bus")
                enabled: root.routing.editable
                navigation.panel: navigationPanel
                navigation.order: 2
                onClicked: root.routing.addBus()
            }
            Item {
                Layout.fillWidth: true
            }
        }
        RowLayout {
            FlatButton {
                text: freezeModel.frozen ? qsTrc("freeze", "Unfreeze") : qsTrc("freeze", "Freeze")
                enabled: freezeModel.frozen ? !freezeModel.busy && root.routing.editable : freezeModel.canFreeze
                navigation.panel: navigationPanel
                navigation.order: 3
                onClicked: freezeModel.frozen ? freezeModel.unfreeze() : freezeModel.freeze(tail.value)
            }
            StyledTextLabel {
                text: qsTrc("freeze", "Effect tail (seconds)")
            }
            IncrementalPropertyControl {
                id: tail
                Layout.preferredWidth: 90
                currentValue: 2
                minValue: 0
                maxValue: 30
                step: 0.1
                decimals: 1
                property real value: currentValue
                navigation.panel: navigationPanel
                navigation.order: 4
                onValueEdited: function (value) {
                    currentValue = value
                }
            }
            FlatButton {
                visible: freezeModel.busy
                text: qsTrc("freeze", "Cancel")
                navigation.panel: navigationPanel
                navigation.order: 5
                onClicked: freezeModel.cancel()
            }
            StyledTextLabel {
                visible: freezeModel.busy
                text: Math.round(freezeModel.progress * 100) + "%"
            }
            StyledTextLabel {
                text: freezeModel.error
                visible: text.length > 0
            }
        }
        StyledTextLabel {
            text: qsTrc("routing", "Sends")
        }
        ScrollView {
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            ColumnLayout {
                width: parent.width
                Repeater {
                    model: root.routing.sends
                    delegate: RowLayout {
                        id: row
                        required property var modelData
                        required property int index
                        CheckBox {
                            text: row.modelData.title
                            checked: row.modelData.enabled
                            enabled: root.routing.editable
                            navigation.panel: navigationPanel
                            navigation.order: 6 + row.index * 3
                            onClicked: root.routing.setSend(row.modelData.index, !row.modelData.enabled, row.modelData.gain, row.modelData.preFader)
                        }
                        StyledSlider {
                            Layout.preferredWidth: 220
                            stepSize: 0.01
                            from: 0
                            to: 4
                            value: row.modelData.gain
                            enabled: root.routing.editable && row.modelData.enabled
                            navigation.panel: navigationPanel
                            navigation.order: 7 + row.index * 3
                            onMoved: if (!pressed)
                                root.routing.setSend(row.modelData.index, true, value, row.modelData.preFader)
                            onPressedChanged: if (!pressed)
                                root.routing.setSend(row.modelData.index, true, value, row.modelData.preFader)
                        }
                        StyledTextLabel {
                            text: Math.round(row.modelData.gain * 100) + "%"
                        }
                        CheckBox {
                            text: qsTrc("routing", "Pre-fader")
                            checked: row.modelData.preFader
                            enabled: root.routing.editable && row.modelData.enabled
                            navigation.panel: navigationPanel
                            navigation.order: 8 + row.index * 3
                            onClicked: root.routing.setSend(row.modelData.index, true, row.modelData.gain, !row.modelData.preFader)
                        }
                    }
                }
                StyledTextLabel {
                    visible: root.routing.sends.length === 0
                    text: qsTrc("routing", "Add a bus to create a send.")
                }
            }
        }
        StyledTextLabel {
            text: root.routing.error
            visible: text.length > 0
        }
    }
}
