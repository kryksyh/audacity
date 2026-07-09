/*
 * Audacity: A Digital Audio Editor
 */
import QtQuick
import QtQuick.Layouts

import Muse.Ui
import Muse.UiComponents

import Audacity.Preferences

StyledDialogView {
    id: root

    property alias pluginId: loader.pluginId
    property string pluginName: ""

    title: pluginName

    contentWidth: 420
    contentHeight: 320

    resizable: true

    ColumnLayout {
        anchors.fill: parent
        spacing: 12

        PluginConfigViewLoader {
            id: loader

            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.margins: 16
        }

        ButtonBox {
            id: buttonBox

            Layout.fillWidth: true
            Layout.margins: 12

            FlatButton {
                text: qsTrc("global", "Close")
                buttonRole: ButtonBoxModel.RejectRole
                isNarrow: true

                onClicked: root.hide()
            }
        }
    }
}
