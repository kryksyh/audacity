import QtQuick
import Muse.Ui
import Muse.UiComponents

Item {
    id: root
    property real rowHeight: 18
    property real contentY: 0
    property var heldNotes: []
    signal notePressed(int pitch)
    signal noteReleased(int pitch)
    clip: true

    Column {
        y: -root.contentY
        width: root.width
        Repeater {
            model: 128
            Rectangle {
                id: key
                required property int index
                readonly property int pitch: 127 - index
                readonly property bool black: [1, 3, 6, 8, 10].includes(pitch % 12)
                width: root.width
                height: root.rowHeight
                color: "#f4f4f4"
                border.color: ui.theme.strokeColor
                Rectangle {
                    anchors.left: parent.left
                    width: key.black ? parent.width * 0.62 : parent.width
                    height: parent.height
                    color: root.heldNotes.includes(key.pitch) ? ui.theme.accentColor : key.black ? "#24242c" : "transparent"
                    radius: key.black ? 2 : 0
                }
                StyledTextLabel {
                    anchors.right: parent.right
                    anchors.rightMargin: 5
                    height: parent.height
                    text: key.pitch % 12 === 0 ? "C" + (Math.floor(key.pitch / 12) - 1) : ""
                    color: "#555555"
                }
                MouseArea {
                    anchors.fill: parent
                    onPressed: root.notePressed(key.pitch)
                    onReleased: root.noteReleased(key.pitch)
                    onCanceled: root.noteReleased(key.pitch)
                }
            }
        }
    }
}
