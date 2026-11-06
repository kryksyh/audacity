/*
 * Audacity: A Digital Audio Editor
 */
import QtQuick
import QtQuick.Layouts

import Muse.Ui
import Muse.UiComponents
import Audacity.AppShell

StyledDialogView {
    id: root

    objectName: "MeasureLatencyDialog"
    title: qsTrc("latency", "Measure latency")
    modal: true
    contentWidth: 520
    contentHeight: measuring ? 220 : 500

    readonly property bool measuring: measurementModel.state === LatencyMeasurementModel.Measuring
    readonly property bool setup: measurementModel.state === LatencyMeasurementModel.Idle

    onClosed: measurementModel.cancel()

    LatencyMeasurementModel {
        id: measurementModel
    }

    Timer {
        interval: 100
        repeat: true
        running: root.measuring
        onTriggered: measurementModel.pollProgress()
    }

    NavigationPanel {
        id: buttonsNavigation
        name: "LatencyButtons"
        section: root.navigationSection
        direction: NavigationPanel.Horizontal
        order: 1
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        ColumnLayout {
            visible: !root.measuring
            Layout.fillWidth: true
            Layout.margins: 16
            spacing: 8

            StyledTextLabel {
                text: root.setup ? qsTrc("latency", "Step 1 of 2") : qsTrc("latency", "Step 2 of 2")
            }

            RowLayout {
                Layout.fillWidth: true
                spacing: 4

                Repeater {
                    model: 2
                    Rectangle {
                        required property int index
                        Layout.fillWidth: true
                        height: 3
                        radius: 1
                        color: index === 0 || !root.setup ? ui.theme.accentColor : ui.theme.strokeColor
                    }
                }
            }
        }

        SeparatorLine {
            visible: !root.measuring
            Layout.fillWidth: true
        }

        ColumnLayout {
            visible: root.setup
            Layout.fillWidth: true
            Layout.margins: 16
            spacing: 12

            StyledTextLabel {
                Layout.fillWidth: true
                horizontalAlignment: Text.AlignLeft
                wrapMode: Text.WordWrap
                text: qsTrc("latency", "Connect an output of your audio interface to one of its inputs with a cable. Or hold the microphone close to the speaker or headphones.")
            }

            StyledTextLabel {
                Layout.fillWidth: true
                horizontalAlignment: Text.AlignLeft
                wrapMode: Text.WordWrap
                text: qsTrc("latency", "The test plays four short noise bursts. Lower the volume if you use headphones.")
            }

            StyledGroupBox {
                Layout.fillWidth: true
                title: qsTrc("latency", "Current audio settings")

                contentItem: GridLayout {
                    id: settingsGrid
                    columns: 2
                    columnSpacing: 16
                    rowSpacing: 16

                    Repeater {
                        model: measurementModel.settings

                        ColumnLayout {
                            required property var modelData
                            Layout.fillWidth: true
                            Layout.preferredWidth: (settingsGrid.width - settingsGrid.columnSpacing) / 2
                            spacing: 4

                            StyledTextLabel {
                                Layout.fillWidth: true
                                horizontalAlignment: Text.AlignLeft
                                text: parent.modelData.title
                            }
                            StyledTextLabel {
                                Layout.fillWidth: true
                                horizontalAlignment: Text.AlignLeft
                                wrapMode: Text.WordWrap
                                font: ui.theme.bodyBoldFont
                                text: parent.modelData.value
                            }
                        }
                    }
                }
            }
        }

        ColumnLayout {
            visible: root.measuring
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.margins: 24
            spacing: 16

            Item {
                Layout.fillHeight: true
            }
            StyledTextLabel {
                Layout.alignment: Qt.AlignHCenter
                text: qsTrc("latency", "Playing sounds to test latency")
            }
            StyledTextLabel {
                Layout.alignment: Qt.AlignHCenter
                font: ui.theme.headerBoldFont
                text: measurementModel.signalsPlayed > 0 ? measurementModel.signalsPlayed : qsTrc("latency", "Starting…")
            }
            StyledTextLabel {
                Layout.alignment: Qt.AlignHCenter
                text: qsTrc("latency", "Keep the microphone close to the speaker.")
            }
            Item {
                Layout.fillHeight: true
            }
        }

        ColumnLayout {
            visible: !root.setup && !root.measuring
            Layout.fillWidth: true
            Layout.margins: 16
            spacing: 12

            StyledGroupBox {
                Layout.fillWidth: true
                title: qsTrc("latency", "Latency test results")

                contentItem: ColumnLayout {
                    spacing: 12

                    ColumnLayout {
                        visible: measurementModel.state === LatencyMeasurementModel.Measured
                        Layout.fillWidth: true
                        spacing: 4

                        StyledTextLabel {
                            text: qsTrc("latency", "Round trip")
                        }
                        StyledTextLabel {
                            Layout.fillWidth: true
                            horizontalAlignment: Text.AlignLeft
                            wrapMode: Text.WordWrap
                            font: ui.theme.bodyBoldFont
                            text: measurementModel.resultText
                        }
                        StyledTextLabel {
                            Layout.topMargin: 8
                            text: qsTrc("latency", "Driver-reported latency")
                        }
                        StyledTextLabel {
                            Layout.fillWidth: true
                            horizontalAlignment: Text.AlignLeft
                            wrapMode: Text.WordWrap
                            font: ui.theme.bodyBoldFont
                            text: measurementModel.reportedText
                        }
                    }

                    RowLayout {
                        visible: measurementModel.warningText.length > 0
                        Layout.fillWidth: true
                        spacing: 8

                        StyledIconLabel {
                            Layout.alignment: Qt.AlignTop
                            iconCode: measurementModel.state === LatencyMeasurementModel.Failed ? IconCode.ERROR : IconCode.WARNING
                        }
                        StyledTextLabel {
                            Layout.fillWidth: true
                            horizontalAlignment: Text.AlignLeft
                            wrapMode: Text.WordWrap
                            text: measurementModel.warningText
                        }
                    }

                    RowLayout {
                        visible: measurementModel.detailsText.length > 0
                        Layout.fillWidth: true
                        spacing: 8

                        StyledIconLabel {
                            iconCode: measurementModel.warningText.length > 0 ? IconCode.WARNING : IconCode.TICK
                        }
                        StyledTextLabel {
                            Layout.fillWidth: true
                            horizontalAlignment: Text.AlignLeft
                            wrapMode: Text.WordWrap
                            text: measurementModel.detailsText
                        }
                    }
                }
            }

            FlatButton {
                text: qsTrc("latency", "Rerun latency test")
                navigation.name: "RerunLatencyTest"
                navigation.panel: buttonsNavigation
                navigation.column: 0
                onClicked: measurementModel.start()
            }
        }

        Item {
            visible: !root.measuring
            Layout.fillHeight: true
        }

        SeparatorLine {
            Layout.fillWidth: true
        }

        RowLayout {
            Layout.fillWidth: true
            Layout.margins: 12
            spacing: 8

            FlatButton {
                visible: !root.setup && !root.measuring
                text: qsTrc("latency", "Back")
                navigation.name: "LatencyBack"
                navigation.panel: buttonsNavigation
                navigation.column: 1
                onClicked: measurementModel.reset()
            }
            Item {
                Layout.fillWidth: true
            }
            FlatButton {
                text: qsTrc("latency", "Cancel")
                navigation.name: "LatencyCancel"
                navigation.panel: buttonsNavigation
                navigation.column: 2
                onClicked: root.reject()
            }
            FlatButton {
                visible: !root.measuring
                enabled: root.setup || measurementModel.state === LatencyMeasurementModel.Measured
                text: root.setup ? qsTrc("latency", "Start latency test") : qsTrc("latency", "Use result")
                accentButton: true
                navigation.name: "LatencyContinue"
                navigation.panel: buttonsNavigation
                navigation.column: 3
                onClicked: {
                    if (root.setup) {
                        measurementModel.start()
                    } else if (measurementModel.apply()) {
                        root.accept()
                    }
                }
            }
        }
    }
}
