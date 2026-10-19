import QtQuick

import Muse.Ui
import Muse.UiComponents
import Audacity.ProjectScene
import Audacity.TrackEdit

TrackItemsContainer {
    id: root

    PianoRollModel {
        id: midi
        trackId: root.trackId
    }

    onInitRequired: midi.init()

    contentComponent: Component {
        Item {
            clip: true

            MouseArea {
                anchors.fill: parent
                enabled: midi.editable
                onDoubleClicked: function (mouse) {
                    const id = midi.addClip(Math.max(0, root.context.positionToTime(mouse.x)))
                    if (id > 0) {
                        midi.openEditor(id)
                    }
                }
            }

            StyledTextLabel {
                anchors.centerIn: parent
                visible: midi.clips.length === 0
                text: qsTrc("pianoroll", "Double-click to add a MIDI clip")
            }

            Repeater {
                model: midi.clips

                Rectangle {
                    id: midiClip
                    required property var modelData
                    readonly property real startPosition: (modelData.start - root.context.frameStartTime) * root.context.zoom
                    x: startPosition
                    width: Math.max(4, modelData.length * root.context.zoom)
                    y: 3
                    height: parent.height - root.bottomSeparatorHeight - 6
                    color: ui.theme.backgroundSecondaryColor
                    border.color: ui.theme.accentColor
                    radius: 3
                    clip: true

                    StyledTextLabel {
                        anchors.left: parent.left
                        anchors.right: parent.right
                        anchors.margins: 6
                        height: 22
                        horizontalAlignment: Text.AlignLeft
                        text: midiClip.modelData.title
                    }

                    Canvas {
                        id: preview
                        anchors.fill: parent
                        anchors.topMargin: 24
                        anchors.margins: 5
                        onPaint: {
                            const ctx = getContext("2d")
                            ctx.clearRect(0, 0, width, height)
                            ctx.fillStyle = ui.theme.accentColor
                            const notes = midiClip.modelData.notes
                            let low = 48
                            let high = 72
                            for (const note of notes) {
                                low = Math.min(low, note.pitch)
                                high = Math.max(high, note.pitch)
                            }
                            for (const note of notes) {
                                ctx.fillRect(note.start / midiClip.modelData.beats * width, (high - note.pitch) / (high - low + 1) * height, Math.max(2, note.length / midiClip.modelData.beats * width), Math.max(2, height / (high - low + 1) - 1))
                            }
                        }
                        onWidthChanged: requestPaint()
                        onHeightChanged: requestPaint()
                    }

                    MouseArea {
                        anchors.fill: parent
                        enabled: midi.editable
                        property real pressX: 0
                        property real moveX: 0
                        onPressed: function (mouse) {
                            pressX = mouse.x
                            moveX = 0
                        }
                        onPositionChanged: function (mouse) {
                            if (pressed) {
                                moveX = mouse.x - pressX
                            }
                        }
                        onReleased: {
                            if (Math.abs(moveX) > 5) {
                                midi.moveClip(midiClip.modelData.id, Math.max(0, root.context.positionToTime(midiClip.startPosition + moveX)))
                            }
                        }
                        onDoubleClicked: midi.openEditor(midiClip.modelData.id)
                    }
                }
            }
        }
    }
}
