import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

import Muse.Ui
import Muse.UiComponents
import Audacity.TrackEdit

FocusScope {
    id: root
    required property var midi
    property alias navigationSection: controlsNavigation.section
    property alias navigationOrderStart: controlsNavigation.order

    NavigationPanel {
        id: controlsNavigation
        name: "PianoRollControls"
        direction: NavigationPanel.Both
        enabled: root.enabled && root.visible
    }
    property real beatWidth: 96
    property real rowHeight: 18
    property real snap: 0.25
    readonly property real gridLength: Math.max(midi.clipLength, Math.ceil(midi.inputLength))
    property real noteLength: 1
    property int velocity: 100
    property int octave: 4
    property real sustainBeat: 0
    property bool computerKeyboard: false
    property bool showParameters: false
    property bool instrumentEditorVisible: false
    property bool showVelocity: false
    property var selectedNote: null
    property var pressedKeys: ({})
    readonly property var noteValues: [4, 2, 1, 0.5, 0.25, 0.125]
    readonly property var noteChoices: ["1/1", "1/2", "1/4", "1/8", "1/16", "1/32"]
    readonly property var keyMap: ({
            "90": 0,
            "83": 1,
            "88": 2,
            "68": 3,
            "67": 4,
            "86": 5,
            "71": 6,
            "66": 7,
            "72": 8,
            "78": 9,
            "74": 10,
            "77": 11,
            "44": 12,
            "76": 13,
            "46": 14,
            "59": 15,
            "47": 16,
            "81": 12,
            "50": 13,
            "87": 14,
            "51": 15,
            "69": 16,
            "82": 17,
            "53": 18,
            "84": 19,
            "54": 20,
            "89": 21,
            "55": 22,
            "85": 23
        })

    Component.onCompleted: gridView.contentY = (127 - 72) * root.rowHeight
    Component.onDestruction: midi.disableInput()
    onVisibleChanged: {
        if (!visible) {
            releaseComputerKeys()
            if (!root.instrumentEditorVisible) {
                midi.disableInput()
            }
        }
    }

    Connections {
        target: midi
        function onDataChanged() {
            if (root.selectedNote) {
                root.selectedNote = midi.notes.find(note => note.id === root.selectedNote.id) || null
            }
        }
        function onInputProgressChanged() {
            const position = midi.inputLength * root.beatWidth
            if (midi.inputRecording && position > gridView.contentX + gridView.width - 24) {
                gridView.contentX = Math.max(0, position - gridView.width + 24)
            }
        }
        function onEditorRequested() {
            gridView.contentY = (127 - 72) * root.rowHeight
        }
    }

    function snapped(value) {
        return Math.round(value / snap) * snap
    }
    function selectedEdit(start, length, pitch, velocity) {
        if (selectedNote) {
            midi.editNote(selectedNote.id, start, length, pitch, velocity)
        }
    }
    function releaseComputerKeys() {
        for (const key in pressedKeys) {
            midi.noteOff(pressedKeys[key])
        }
        pressedKeys = ({})
    }
    function focusKeyboard() {
        if (computerKeyboard) {
            keyboardInput.forceActiveFocus()
        }
    }

    Item {
        id: keyboardInput
        onActiveFocusChanged: {
            if (!activeFocus) {
                root.releaseComputerKeys()
            }
        }
        Keys.onShortcutOverride: function (event) {
            if (root.computerKeyboard && !(event.modifiers & (Qt.ControlModifier | Qt.AltModifier | Qt.MetaModifier)) && root.keyMap[event.key] !== undefined) {
                event.accepted = true
            }
        }
        Keys.onPressed: function (event) {
            const offset = root.keyMap[event.key]
            if (!root.computerKeyboard || offset === undefined || event.modifiers & (Qt.ControlModifier | Qt.AltModifier | Qt.MetaModifier)) {
                return
            }
            event.accepted = true
            if (!event.isAutoRepeat && root.pressedKeys[event.key] === undefined) {
                const pitch = Math.min(127, (root.octave + 1) * 12 + offset)
                root.pressedKeys[event.key] = pitch
                midi.noteOn(pitch, root.velocity)
            }
        }
        Keys.onReleased: function (event) {
            const pitch = root.pressedKeys[event.key]
            if (pitch !== undefined) {
                event.accepted = true
                if (!event.isAutoRepeat) {
                    midi.noteOff(pitch)
                    delete root.pressedKeys[event.key]
                }
            }
        }
    }

    Rectangle {
        anchors.fill: parent
        color: ui.theme.backgroundPrimaryColor
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 8
        spacing: 6

        RowLayout {
            Layout.fillWidth: true
            StyledTextLabel {
                Layout.preferredWidth: 100
                text: midi.title
                horizontalAlignment: Text.AlignLeft
            }
            StyledDropdown {
                objectName: "pianoRollControl1"
                navigation.panel: controlsNavigation
                navigation.order: 1

                Layout.preferredWidth: 190
                model: midi.instruments
                textRole: "title"
                currentIndex: midi.instrumentIndex
                enabled: midi.editable
                onActivated: function (index) {
                    midi.selectInstrument(index)
                }
            }
            FlatButton {
                objectName: "pianoRollControl2"
                navigation.panel: controlsNavigation
                navigation.order: 2

                text: qsTrc("pianoroll", "Instrument editor")
                onClicked: midi.showInstrumentEditor()
            }
            FlatButton {
                objectName: "pianoRollControl3"
                navigation.panel: controlsNavigation
                navigation.order: 3

                minWidth: 96
                text: qsTrc("pianoroll", "Parameters")
                accentButton: root.showParameters
                onClicked: {
                    root.showParameters = !root.showParameters
                    if (root.showParameters) {
                        midi.loadParameters()
                    }
                }
            }
            Item {
                Layout.fillWidth: true
            }
            FlatButton {
                objectName: "pianoRollControl4"
                navigation.panel: controlsNavigation
                navigation.order: 4

                text: midi.inputRecording ? qsTrc("pianoroll", "Stop notes") : qsTrc("pianoroll", "Record notes")
                accentButton: midi.inputRecording
                enabled: midi.editable || midi.inputRecording
                onClicked: {
                    if (midi.inputRecording) {
                        midi.stopInputRecording()
                    } else {
                        midi.startInputRecording()
                    }
                    root.focusKeyboard()
                }
            }
        }

        StyledTextLabel {
            Layout.fillWidth: true
            visible: midi.error.length > 0
            text: midi.error
            wrapMode: Text.Wrap
            horizontalAlignment: Text.AlignLeft
        }

        RowLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            spacing: 8

            Flickable {
                id: noteControlsViewport
                Layout.preferredWidth: 190
                Layout.fillHeight: true
                contentWidth: width
                contentHeight: noteControls.height
                clip: true
                ScrollBar.vertical: StyledScrollBar {}
                Column {
                    id: noteControls
                    width: noteControlsViewport.width - 8
                    spacing: 6
                    StyledTextLabel {
                        text: qsTrc("pianoroll", "Note length")
                    }
                    StyledDropdown {
                        objectName: "pianoRollControl5"
                        navigation.panel: controlsNavigation
                        navigation.order: 5

                        width: parent.width
                        model: root.noteChoices
                        currentIndex: root.noteValues.indexOf(root.noteLength)
                        onActivated: function (index) {
                            root.noteLength = root.noteValues[index]
                        }
                    }
                    StyledTextLabel {
                        text: qsTrc("pianoroll", "Quantize")
                    }
                    StyledDropdown {
                        objectName: "pianoRollControl6"
                        navigation.panel: controlsNavigation
                        navigation.order: 6

                        width: parent.width
                        model: root.noteChoices
                        currentIndex: root.noteValues.indexOf(root.snap)
                        onActivated: function (index) {
                            root.snap = root.noteValues[index]
                        }
                    }
                    FlatButton {
                        objectName: "pianoRollControl7"
                        navigation.panel: controlsNavigation
                        navigation.order: 7

                        width: parent.width
                        text: qsTrc("pianoroll", "Edit note velocity")
                        accentButton: root.showVelocity
                        onClicked: root.showVelocity = !root.showVelocity
                    }
                    IncrementalPropertyControl {
                        objectName: "pianoRollControl8"
                        navigation.panel: controlsNavigation
                        navigation.order: 8

                        width: parent.width
                        minValue: 1
                        maxValue: 127
                        decimals: 0
                        step: 1
                        currentValue: root.selectedNote ? root.selectedNote.velocity : root.velocity
                        enabled: midi.editable
                        onValueEditingFinished: function (value) {
                            root.velocity = value
                            if (root.selectedNote) {
                                root.selectedEdit(root.selectedNote.start, root.selectedNote.length, root.selectedNote.pitch, value)
                            }
                        }
                    }
                    FlatButton {
                        objectName: "pianoRollControl9"
                        navigation.panel: controlsNavigation
                        navigation.order: 9

                        width: parent.width
                        text: qsTrc("pianoroll", "Computer keyboard")
                        accentButton: root.computerKeyboard
                        onClicked: {
                            root.releaseComputerKeys()
                            root.computerKeyboard = !root.computerKeyboard
                            if (root.computerKeyboard && midi.enableInput()) {
                                root.focusKeyboard()
                            }
                        }
                    }
                    IncrementalPropertyControl {
                        objectName: "pianoRollControl10"
                        navigation.panel: controlsNavigation
                        navigation.order: 10

                        width: parent.width
                        minValue: 0
                        maxValue: 8
                        decimals: 0
                        step: 1
                        currentValue: root.octave
                        measureUnitsSymbol: qsTrc("pianoroll", "Octave")
                        onValueEditingFinished: function (value) {
                            root.releaseComputerKeys()
                            root.octave = value
                        }
                    }
                    StyledTextLabel {
                        text: qsTrc("pianoroll", "MIDI input")
                    }
                    StyledDropdown {
                        objectName: "pianoRollControl11"
                        navigation.panel: controlsNavigation
                        navigation.order: 11

                        width: parent.width
                        model: midi.midiInputs
                        textRole: "title"
                        currentIndex: midi.midiInputIndex
                        onActivated: function (index) {
                            midi.selectMidiInput(index)
                        }
                    }
                    FlatButton {
                        objectName: "pianoRollControl12"
                        navigation.panel: controlsNavigation
                        navigation.order: 12

                        width: parent.width
                        text: qsTrc("pianoroll", "Connect MIDI input")
                        onClicked: {
                            midi.enableInput()
                            midi.refreshMidiInputs()
                        }
                    }
                    StyledTextLabel {
                        text: qsTrc("pianoroll", "Sustain beat")
                    }
                    IncrementalPropertyControl {
                        objectName: "pianoRollSustainBeat"
                        navigation.panel: controlsNavigation
                        navigation.order: 30
                        width: parent.width
                        minValue: 0
                        maxValue: Math.max(0, midi.clipLength - 0.25)
                        decimals: 2
                        step: 0.25
                        currentValue: root.sustainBeat
                        enabled: midi.editable
                        onValueEditingFinished: function (value) {
                            root.sustainBeat = value
                        }
                    }
                    Row {
                        width: parent.width
                        spacing: 4
                        FlatButton {
                            objectName: "pianoRollPedalDown"
                            navigation.panel: controlsNavigation
                            navigation.order: 31
                            width: (parent.width - 4) / 2
                            minWidth: 40
                            text: qsTrc("pianoroll", "Pedal down")
                            enabled: midi.editable
                            onClicked: midi.setSustain(root.sustainBeat, true)
                        }
                        FlatButton {
                            objectName: "pianoRollPedalUp"
                            navigation.panel: controlsNavigation
                            navigation.order: 32
                            width: (parent.width - 4) / 2
                            minWidth: 40
                            text: qsTrc("pianoroll", "Pedal up")
                            enabled: midi.editable
                            onClicked: midi.setSustain(root.sustainBeat, false)
                        }
                    }
                    FlatButton {
                        objectName: "pianoRollControl13"
                        navigation.panel: controlsNavigation
                        navigation.order: 13

                        width: parent.width
                        text: qsTrc("pianoroll", "Delete note")
                        enabled: midi.editable && root.selectedNote !== null
                        onClicked: midi.removeNote(root.selectedNote.id)
                    }
                    FlatButton {
                        objectName: "pianoRollControl14"
                        navigation.panel: controlsNavigation
                        navigation.order: 14

                        width: parent.width
                        text: qsTrc("pianoroll", "All notes off")
                        onClicked: {
                            root.releaseComputerKeys()
                            midi.releaseNotes()
                        }
                    }
                }
            }

            ColumnLayout {
                Layout.fillWidth: true
                Layout.fillHeight: true
                spacing: 0

                Item {
                    Layout.fillWidth: true
                    Layout.preferredHeight: 28
                    clip: true
                    Repeater {
                        model: Math.ceil(root.gridLength) + 1
                        StyledTextLabel {
                            required property int index
                            x: 74 + index * root.beatWidth - gridView.contentX
                            height: 28
                            text: index % 4 === 0 ? String(Math.floor(index / 4) + 1) : String(Math.floor(index / 4) + 1) + "." + String(index % 4 + 1)
                        }
                    }
                }

                RowLayout {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    spacing: 0
                    PianoKeyboard {
                        Layout.preferredWidth: 74
                        Layout.fillHeight: true
                        rowHeight: root.rowHeight
                        contentY: gridView.contentY
                        heldNotes: midi.heldNotes
                        onNotePressed: function (pitch) {
                            midi.noteOn(pitch, root.velocity)
                        }
                        onNoteReleased: function (pitch) {
                            midi.noteOff(pitch)
                        }
                    }
                    Flickable {
                        id: gridView
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        contentWidth: Math.max(width, root.gridLength * root.beatWidth)
                        contentHeight: 128 * root.rowHeight
                        clip: true
                        boundsBehavior: Flickable.StopAtBounds
                        ScrollBar.vertical: StyledScrollBar {}
                        ScrollBar.horizontal: StyledScrollBar {}
                        Column {
                            Repeater {
                                model: 128
                                Rectangle {
                                    required property int index
                                    width: gridView.contentWidth
                                    height: root.rowHeight
                                    color: [1, 3, 6, 8, 10].includes((127 - index) % 12) ? ui.theme.backgroundSecondaryColor : ui.theme.backgroundPrimaryColor
                                    border.color: ui.theme.strokeColor
                                    border.width: 0.5
                                }
                            }
                        }
                        Repeater {
                            model: Math.ceil(root.gridLength / root.snap) + 1
                            Rectangle {
                                required property int index
                                x: index * root.snap * root.beatWidth
                                width: 1
                                height: gridView.contentHeight
                                color: ui.theme.strokeColor
                                opacity: Math.round(index * root.snap) === index * root.snap ? 0.8 : 0.25
                            }
                        }
                        MouseArea {
                            width: root.gridLength * root.beatWidth
                            height: gridView.contentHeight
                            enabled: midi.editable
                            onPressed: root.focusKeyboard()
                            onDoubleClicked: function (mouse) {
                                const beat = Math.floor(mouse.x / root.beatWidth / root.snap) * root.snap
                                midi.addNote(beat, Math.min(root.noteLength, midi.clipLength - beat), 127 - Math.floor(mouse.y / root.rowHeight), root.velocity)
                            }
                        }

                        Rectangle {
                            visible: !midi.inputRecording && midi.playbackBeat >= 0 && midi.playbackBeat <= midi.clipLength
                            x: midi.playbackBeat * root.beatWidth
                            width: 1
                            height: gridView.contentHeight
                            z: 1
                            color: ui.theme.fontPrimaryColor
                        }
                        Rectangle {
                            visible: midi.inputRecording
                            x: midi.inputLength * root.beatWidth
                            width: 1
                            height: gridView.contentHeight
                            color: ui.theme.accentColor
                        }
                        Repeater {
                            model: midi.inputNotes
                            Rectangle {
                                required property var modelData
                                x: modelData.start * root.beatWidth
                                y: (127 - modelData.pitch) * root.rowHeight + 1
                                width: Math.max(3, modelData.length * root.beatWidth - 1)
                                height: root.rowHeight - 2
                                color: ui.theme.accentColor
                                opacity: 0.6
                            }
                        }
                        Repeater {
                            model: midi.notes
                            Rectangle {
                                id: noteItem
                                required property var modelData
                                property real previewStart: modelData.start
                                property real previewLength: modelData.length
                                property int previewPitch: modelData.pitch
                                x: previewStart * root.beatWidth
                                y: (127 - previewPitch) * root.rowHeight + 1
                                width: Math.max(3, previewLength * root.beatWidth - 1)
                                height: root.rowHeight - 2
                                color: ui.theme.accentColor
                                opacity: 0.4 + modelData.velocity / 127 * 0.6
                                border.width: root.selectedNote && root.selectedNote.id === modelData.id ? 2 : 0
                                border.color: ui.theme.fontPrimaryColor

                                MouseArea {
                                    anchors.fill: parent
                                    enabled: midi.editable
                                    acceptedButtons: Qt.LeftButton | Qt.RightButton
                                    property real pressX: 0
                                    property real pressY: 0
                                    property bool resize: false
                                    property real originalStart: 0
                                    property real originalLength: 0
                                    property int originalPitch: 0
                                    onPressed: function (mouse) {
                                        root.selectedNote = noteItem.modelData
                                        pressX = mouse.x + noteItem.x
                                        pressY = mouse.y + noteItem.y
                                        resize = mouse.x > noteItem.width - 8
                                        originalStart = noteItem.modelData.start
                                        originalLength = noteItem.modelData.length
                                        originalPitch = noteItem.modelData.pitch
                                    }
                                    onPositionChanged: function (mouse) {
                                        if (!pressed || pressedButtons !== Qt.LeftButton) {
                                            return
                                        }
                                        const deltaX = (mouse.x + noteItem.x - pressX) / root.beatWidth
                                        const deltaY = Math.round((mouse.y + noteItem.y - pressY) / root.rowHeight)
                                        if (resize) {
                                            noteItem.previewLength = Math.max(root.snap, Math.min(midi.clipLength - originalStart, root.snapped(originalLength + deltaX)))
                                        } else {
                                            noteItem.previewStart = Math.max(0, Math.min(midi.clipLength - originalLength, root.snapped(originalStart + deltaX)))
                                            noteItem.previewPitch = Math.max(0, Math.min(127, originalPitch - deltaY))
                                        }
                                    }
                                    onReleased: function (mouse) {
                                        if (mouse.button === Qt.RightButton) {
                                            midi.removeNote(noteItem.modelData.id)
                                        } else if (noteItem.previewStart !== originalStart || noteItem.previewLength !== originalLength || noteItem.previewPitch !== originalPitch) {
                                            midi.editNote(noteItem.modelData.id, noteItem.previewStart, noteItem.previewLength, noteItem.previewPitch, noteItem.modelData.velocity)
                                        }
                                    }
                                    onCanceled: {
                                        noteItem.previewStart = Qt.binding(function () {
                                            return noteItem.modelData.start
                                        })
                                        noteItem.previewLength = Qt.binding(function () {
                                            return noteItem.modelData.length
                                        })
                                        noteItem.previewPitch = Qt.binding(function () {
                                            return noteItem.modelData.pitch
                                        })
                                    }
                                }
                            }
                        }
                    }
                }

                Item {
                    Layout.fillWidth: true
                    Layout.preferredHeight: 62
                    visible: root.showVelocity
                    clip: true
                    Repeater {
                        model: midi.notes
                        Rectangle {
                            required property var modelData
                            x: 74 + modelData.start * root.beatWidth - gridView.contentX
                            y: parent.height * (1 - modelData.velocity / 127)
                            width: 8
                            height: parent.height * modelData.velocity / 127
                            color: ui.theme.accentColor
                            MouseArea {
                                anchors.fill: parent
                                property real startY: 0
                                property int originalVelocity: 0
                                onPressed: function (mouse) {
                                    startY = mouse.y + parent.y
                                    originalVelocity = parent.modelData.velocity
                                    root.selectedNote = parent.modelData
                                }
                                onReleased: function (mouse) {
                                    const value = Math.max(1, Math.min(127, Math.round(originalVelocity - (mouse.y + parent.y - startY) / 62 * 127)))
                                    root.selectedEdit(parent.modelData.start, parent.modelData.length, parent.modelData.pitch, value)
                                }
                            }
                        }
                    }
                }
            }

            ListView {
                Layout.preferredWidth: 250
                Layout.fillHeight: true
                visible: root.showParameters
                clip: true
                model: midi.parameters
                spacing: 8
                delegate: Column {
                    required property var modelData
                    width: ListView.view.width
                    StyledTextLabel {
                        width: parent.width
                        text: modelData.title + ": " + modelData.display
                        horizontalAlignment: Text.AlignLeft
                    }
                    StyledSlider {
                        objectName: "pianoRollControl15"
                        navigation.panel: controlsNavigation
                        navigation.order: 15

                        width: parent.width
                        from: 0
                        to: 1
                        stepSize: 0.001
                        value: modelData.value
                        enabled: midi.editable
                        onPressedChanged: {
                            if (!pressed) {
                                midi.setParameter(modelData.id, value)
                            }
                        }
                    }
                }
                ScrollBar.vertical: StyledScrollBar {}
            }
        }
        StyledTextLabel {
            Layout.fillWidth: true
            horizontalAlignment: Text.AlignLeft
            text: root.computerKeyboard ? qsTrc("pianoroll", "Click the grid, then use Z to M and Q to U to play. Record notes adds a take to this clip.") : qsTrc("pianoroll", "Double-click to add notes. Drag to move. Drag the right edge to change length.")
        }
    }
}
