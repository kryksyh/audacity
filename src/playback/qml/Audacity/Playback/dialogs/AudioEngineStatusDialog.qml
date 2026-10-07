/*
* Audacity: A Digital Audio Editor
*/
import QtQuick
import QtQuick.Layouts

import Muse.Ui
import Muse.UiComponents

import Audacity.Playback

StyledDialogView {
    id: root

    title: qsTrc("playback", "Audio engine")

    contentWidth: 440
    contentHeight: content.implicitHeight

    AudioEngineStatusModel {
        id: statusModel
    }

    // The dialog object is kept after closing, so polling follows visibility
    Component.onCompleted: statusModel.start()
    onOpened: statusModel.start()
    onClosed: statusModel.stop()

    ColumnLayout {
        id: content

        anchors.left: parent.left
        anchors.right: parent.right

        spacing: 0

        GridLayout {
            Layout.fillWidth: true
            Layout.margins: 16

            columns: 2
            columnSpacing: 24
            rowSpacing: 6

            Repeater {
                // An index model keeps the delegates while the values refresh
                model: statusModel.details.length * 2

                StyledTextLabel {
                    readonly property var row: statusModel.details[Math.floor(index / 2)]
                    readonly property bool isValue: index % 2 === 1

                    Layout.fillWidth: isValue
                    horizontalAlignment: isValue ? Text.AlignRight : Text.AlignLeft
                    font: isValue ? ui.theme.bodyBoldFont : ui.theme.bodyFont
                    text: row ? (isValue ? row.value : row.label) : ""
                }
            }
        }

        SeparatorLine {}

        ButtonBox {
            Layout.fillWidth: true
            Layout.margins: 8

            buttons: [ButtonBoxModel.Close]

            FlatButton {
                text: qsTrc("playback", "Reset counters")
                buttonRole: ButtonBoxModel.CustomRole
                buttonId: ButtonBoxModel.CustomButton + 1
                isLeftSide: true

                onClicked: statusModel.resetCounters()
            }

            FlatButton {
                //: Copies the audio engine details to the clipboard, e.g. for a bug report
                text: qsTrc("playback", "Copy details")
                buttonRole: ButtonBoxModel.CustomRole
                buttonId: ButtonBoxModel.CustomButton + 2
                isLeftSide: true

                onClicked: statusModel.copyDetails()
            }

            onStandardButtonClicked: function (buttonId) {
                if (buttonId === ButtonBoxModel.Close) {
                    root.hide()
                }
            }
        }
    }
}
