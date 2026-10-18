import QtQuick
import QtQuick.Controls
import Muse.Ui
import Muse.UiComponents
import Audacity.TrackEdit

StyledDialogView {
    id: root
    required property var midi
    property bool pianoRollVisible: false
    title: qsTrc("pianoroll", "Instrument") + " - " + midi.title
    modal: false
    alwaysAboveApp: true
    resizable: false
    contentWidth: nativeView.available && nativeView.implicitWidth > 0 ? nativeView.implicitWidth : 600
    contentHeight: nativeView.available && nativeView.implicitHeight > 0 ? nativeView.implicitHeight : 500

    onClosed: {
        midi.hideInstrumentEditor()
        if (!pianoRollVisible) {
            midi.disableInput()
        }
    }

    InstrumentView {
        id: nativeView
        objectName: "nativeInstrumentView"
        anchors.fill: parent
        model: root.midi
    }
    NavigationPanel {
        id: navigationPanel
        name: "InstrumentParameters"
        section: root.navigationSection
        direction: NavigationPanel.Vertical
        enabled: root.isOpened
    }
    ListView {
        anchors.fill: parent
        anchors.margins: 16
        visible: !nativeView.available
        model: root.midi.parameters
        clip: true
        spacing: 12
        delegate: Column {
            id: parameterRow
            required property var modelData
            required property int index
            width: ListView.view.width
            StyledTextLabel {
                width: parent.width
                horizontalAlignment: Text.AlignLeft
                text: parameterRow.modelData.title + ": " + parameterRow.modelData.display
            }
            StyledSlider {
                width: parent.width
                from: 0
                to: 1
                stepSize: 0.001
                value: parameterRow.modelData.value
                enabled: root.midi.editable
                navigation.panel: navigationPanel
                navigation.order: parameterRow.index
                onMoved: if (!pressed)
                    root.midi.setParameter(parameterRow.modelData.id, value)
                onPressedChanged: if (!pressed)
                    root.midi.setParameter(parameterRow.modelData.id, value)
            }
        }
        ScrollBar.vertical: StyledScrollBar {}
    }
}
