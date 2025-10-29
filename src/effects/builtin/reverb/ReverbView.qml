import QtQuick 2.15
import QtQuick.Layouts

import Muse.UiComponents

import Audacity.Effects
import Audacity.BuiltinEffects

BuiltinEffectBase {
    id: root

    property string title: qsTrc("effects/reverb", "Reverb")
    property bool isApplyAllowed: true

    implicitHeight: row.height
    width: 560

    model: ReverbViewModelFactory.createModel(root.instanceId)

    QtObject {
        id: prv

        readonly property int narrowRowSpacing: 16
        readonly property int rowSpacing: 24
        readonly property int columnSpacing: 32
    }

    function newParameterValueRequested(key, value) {
        model.setParam(key, value)
    }

    RowLayout {
        id: row

        width: parent.width
        spacing: prv.rowSpacing

        RoundedRectangle {

            Layout.fillWidth: true
            Layout.preferredWidth: 5
            Layout.preferredHeight: rightColumn.height

            color: ui.theme.backgroundSecondaryColor

            border.color: ui.theme.strokeColor
            border.width: 1

            radius: 4

            GridLayout {
                id: leftColumn

                x: prv.rowSpacing
                y: prv.columnSpacing

                Layout.fillWidth: true

                rows: 2
                columns: 2
                rowSpacing: prv.rowSpacing
                columnSpacing: prv.columnSpacing

                BigParameterKnob {
                    Layout.fillWidth: true

                    parameter: model.paramsList["RoomSize"]
                    radius: 24

                    onNewValueRequested: function (key, newValue) {
                        newParameterValueRequested(key, newValue)
                        value = newValue
                    }

                    onCommitRequested: model.commitSettings()
                }

                BigParameterKnob {
                    Layout.fillWidth: true

                    parameter: model.paramsList["StereoWidth"]
                    radius: 24

                    onNewValueRequested: function (key, newValue) {
                        newParameterValueRequested(key, newValue)
                        value = newValue
                    }

                    onCommitRequested: model.commitSettings()
                }

                BigParameterKnob {
                    Layout.fillWidth: true

                    parameter: model.paramsList["PreDelay"]
                    radius: 24

                    onNewValueRequested: function (key, newValue) {
                        newParameterValueRequested(key, newValue)
                        value = newValue
                    }

                    onCommitRequested: model.commitSettings()
                }
            }
        }

        GridLayout {
            id: rightColumn

            Layout.fillHeight: true
            Layout.fillWidth: true
            Layout.preferredWidth: 7

            columns: 2
            rows: 5
            rowSpacing: prv.narrowRowSpacing
            columnSpacing: prv.columnSpacing

            Item {
                Layout.columnSpan: 2
                Layout.preferredHeight: 1
            }

            ParameterKnob {

                Layout.fillWidth: true

                parameter: model.paramsList["HfDamping"]

                onNewValueRequested: function (key, newValue) {
                    newParameterValueRequested(key, newValue)
                    value = newValue
                }

                onCommitRequested: model.commitSettings()
            }

            ParameterKnob {

                Layout.fillWidth: true

                parameter: model.paramsList["modelerance"]

                onNewValueRequested: function (key, newValue) {
                    newParameterValueRequested(key, newValue)
                    value = newValue
                }

                onCommitRequested: model.commitSettings()
            }

            ParameterKnob {

                Layout.fillWidth: true

                parameter: model.paramsList["ToneLow"]

                onNewValueRequested: function (key, newValue) {
                    newParameterValueRequested(key, newValue)
                    value = newValue
                }

                onCommitRequested: model.commitSettings()
            }

            ParameterKnob {

                Layout.fillWidth: true

                parameter: model.paramsList["ToneHigh"]

                onNewValueRequested: function (key, newValue) {
                    newParameterValueRequested(key, newValue)
                    value = newValue
                }

                onCommitRequested: model.commitSettings()
            }

            SeparatorLine {
                Layout.columnSpan: 2
            }

            ParameterKnob {

                Layout.fillWidth: true

                parameter: model.paramsList["WetGain"]

                onNewValueRequested: function (key, newValue) {
                    newParameterValueRequested(key, newValue)
                    value = newValue
                }

                onCommitRequested: model.commitSettings()
            }

            ParameterKnob {

                Layout.fillWidth: true

                parameter: model.paramsList["DryGain"]

                onNewValueRequested: function (key, newValue) {
                    newParameterValueRequested(key, newValue)
                    value = newValue
                }

                onCommitRequested: model.commitSettings()
            }

            CheckBox {
                id: wetOnly

                text: qsTrc("effects/model", "Wet only")
                checked: model.wetOnly
                onClicked: function () {
                    model.wetOnly = !model.wetOnly
                    model.commitSettings()
                }
            }

            Item {
                Layout.row: 6
                Layout.preferredHeight: prv.narrowRowSpacing
            }
        }
    }
}
