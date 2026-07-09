/*
 * Audacity: A Digital Audio Editor
 */
import QtQuick 2.15
import QtQuick.Layouts 1.15

import Muse.Ui 1.0
import Muse.UiComponents

import Audacity.UiComponents 1.0
import Audacity.Preferences

BaseSection {
    id: root

    property var pluginPreferencesModel: null

    readonly property var plugins: pluginPreferencesModel ? pluginPreferencesModel.pluginConfigViews() : []

    visible: plugins.length > 0

    title: qsTrc("preferences", "Plugin settings")

    navigation.direction: NavigationPanel.Both

    Column {
        width: parent.width
        spacing: root.rowSpacing

        Repeater {
            model: root.plugins

            delegate: RowLayout {
                width: parent.width
                spacing: 8

                StyledTextLabel {
                    Layout.fillWidth: true
                    horizontalAlignment: Text.AlignLeft
                    text: modelData.name
                }

                FlatButton {
                    id: configureButton

                    text: qsTrc("preferences", "Configure…")

                    navigation.panel: root.navigation
                    navigation.row: index
                    navigation.column: 0

                    onClicked: {
                        if (configDialog.isOpened) {
                            configDialog.raise()
                        } else {
                            configDialog.open()
                        }
                    }

                    PluginConfigDialog {
                        id: configDialog
                        pluginId: modelData.id
                        pluginName: modelData.name
                    }
                }
            }
        }
    }
}
