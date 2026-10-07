/*
* Audacity: A Digital Audio Editor
*/
import QtQuick

import Muse.Ui
import Muse.UiComponents

import Audacity.Playback

FlatButton {
    id: root

    property NavigationPanel navigationPanel: null

    transparent: true

    navigation.panel: root.navigationPanel
    navigation.name: "AudioEngineStatus"
    accessible.name: prv.text

    toolTipTitle: qsTrc("playback", "Audio engine")
    toolTipDescription: qsTrc("playback", "Buffer, reported latency, processing load and dropouts. Click for details.")

    QtObject {
        id: prv

        readonly property string loadText: statusModel.streamActive
                                           //: Audio processing load, average and peak, in percent
                                           ? qsTrc("playback", "Load %1 % (peak %2 %)").arg(statusModel.loadPercent).arg(statusModel.peakLoadPercent)
                                           : ""
        readonly property string dropoutsText: qsTrc("playback", "Dropouts: %1").arg(statusModel.dropouts)
        readonly property string text: [statusModel.summary, loadText, dropoutsText].filter(s => s.length > 0).join(" · ")
    }

    AudioEngineStatusModel {
        id: statusModel
    }

    Component.onCompleted: statusModel.start()

    onClicked: statusModel.openDetails()

    contentItem: Component {
        Row {
            spacing: 8

            StyledTextLabel {
                anchors.verticalCenter: parent.verticalCenter
                text: statusModel.summary
            }

            StyledTextLabel {
                anchors.verticalCenter: parent.verticalCenter
                visible: prv.loadText.length > 0
                text: prv.loadText
            }

            StyledTextLabel {
                anchors.verticalCenter: parent.verticalCenter
                text: prv.dropoutsText
                color: statusModel.recentDropout ? ui.theme.extra["error_text_color"] : ui.theme.fontPrimaryColor
            }
        }
    }
}
