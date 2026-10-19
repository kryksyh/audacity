import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

import Muse.Ui
import Muse.UiComponents
import Audacity.Automation

FocusScope {
    id: root
    required property var automation
    readonly property var lanePoints: Array.from(automation.points)
    property alias navigationSection: navigationPanel.section
    property alias navigationOrderStart: navigationPanel.order
    property real zoom: 100
    property real snap: 0.1
    readonly property real valueScale: automation.valueScale
    readonly property real valueOffset: automation.targetIndex === 0 && automation.parameterIndex === 1 ? -100 : 0
    property int selectedPoint: -1
    property int dragIndex: -1
    property real dragTime: 0
    property real dragValue: 0
    readonly property real plotHeight: Math.max(1, chart.height - 28)

    NavigationPanel {
        id: navigationPanel
        name: "TrackAutomationControls"
        direction: NavigationPanel.Both
        enabled: root.enabled && root.visible
    }

    Connections {
        target: automation
        function onDataChanged() {
            if (root.selectedPoint >= root.lanePoints.length) {
                root.selectedPoint = -1
            }
            line.requestPaint()
        }
    }
    onDragTimeChanged: line.requestPaint()
    onDragValueChanged: line.requestPaint()
    onDragIndexChanged: line.requestPaint()

    function timeAt(x) {
        return Math.max(0, Math.round(x / root.zoom / root.snap) * root.snap)
    }
    function valueAt(y) {
        return Math.max(0, Math.min(1, 1 - (y - 28) / root.plotHeight))
    }
    function pointTime(index) {
        return dragIndex === index ? dragTime : root.lanePoints[index].time
    }
    function pointValue(index) {
        return dragIndex === index ? dragValue : root.lanePoints[index].value
    }
    function editSelected(time, value) {
        if (selectedPoint >= 0) {
            automation.setPoint(selectedPoint, time, value)
        }
    }

    Rectangle {
        anchors.fill: parent
        color: ui.theme.backgroundPrimaryColor
    }
    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 8
        spacing: 8

        RowLayout {
            Layout.fillWidth: true
            StyledTextLabel {
                Layout.preferredWidth: 140
                text: automation.title
                horizontalAlignment: Text.AlignLeft
            }
            StyledDropdown {
                Layout.preferredWidth: 180
                objectName: "automationTarget"
                navigation.panel: navigationPanel
                navigation.order: 1
                model: automation.targets
                textRole: "title"
                currentIndex: automation.targetIndex
                onActivated: function (index) {
                    root.selectedPoint = -1
                    automation.targetIndex = index
                }
            }
            StyledDropdown {
                Layout.preferredWidth: 210
                objectName: "automationParameter"
                navigation.panel: navigationPanel
                navigation.order: 2
                model: automation.parameters
                textRole: "title"
                currentIndex: automation.parameterIndex
                onActivated: function (index) {
                    root.selectedPoint = -1
                    automation.parameterIndex = index
                }
            }
            FlatButton {
                text: qsTrc("automation", "Read")
                minWidth: 60
                accentButton: automation.laneEnabled
                enabled: automation.editable
                navigation.panel: navigationPanel
                navigation.order: 3
                onClicked: automation.laneEnabled = !automation.laneEnabled
            }
            Item {
                Layout.fillWidth: true
            }
            FlatButton {
                text: qsTrc("automation", "Refresh parameters")
                navigation.panel: navigationPanel
                navigation.order: 4
                onClicked: automation.refreshParameters()
            }
        }

        StyledTextLabel {
            visible: automation.error.length > 0
            Layout.fillWidth: true
            text: automation.error
            horizontalAlignment: Text.AlignLeft
        }

        StyledTextLabel {
            visible: automation.parameters.length === 0
            Layout.fillWidth: true
            horizontalAlignment: Text.AlignLeft
            text: automation.title.length === 0 ? qsTrc("automation", "Select Automation on an audio or instrument track.") : qsTrc("automation", "No automation parameters are available for this target.")
        }

        RowLayout {
            Layout.fillHeight: true
            Layout.fillWidth: true
            spacing: 10
            Column {
                Layout.preferredWidth: 170
                Layout.alignment: Qt.AlignTop
                spacing: 6
                StyledTextLabel {
                    text: qsTrc("automation", "Time (seconds)")
                }
                IncrementalPropertyControl {
                    width: parent.width
                    minValue: 0
                    maxValue: 1000000
                    decimals: 2
                    step: root.snap
                    currentValue: root.selectedPoint >= 0 && root.selectedPoint < root.lanePoints.length ? root.lanePoints[root.selectedPoint].time : 0
                    enabled: automation.editable && root.selectedPoint >= 0
                    navigation.panel: navigationPanel
                    navigation.order: 5
                    onValueEditingFinished: function (value) {
                        if (root.selectedPoint >= 0 && root.selectedPoint < root.lanePoints.length) {
                            root.editSelected(value, root.lanePoints[root.selectedPoint].value)
                        }
                    }
                }
                StyledTextLabel {
                    text: automation.targetIndex === 0 && automation.parameterIndex === 0 ? qsTrc("automation", "Volume (%)") : automation.targetIndex === 0 ? qsTrc("automation", "Pan (% left/right)") : qsTrc("automation", "Value (%)")
                }
                IncrementalPropertyControl {
                    width: parent.width
                    minValue: root.valueOffset
                    maxValue: root.valueOffset + root.valueScale
                    decimals: 1
                    step: 1
                    currentValue: (root.selectedPoint >= 0 && root.selectedPoint < root.lanePoints.length ? root.lanePoints[root.selectedPoint].value : automation.defaultValue) * root.valueScale + root.valueOffset
                    enabled: automation.editable && root.selectedPoint >= 0
                    navigation.panel: navigationPanel
                    navigation.order: 6
                    onValueEditingFinished: function (value) {
                        if (root.selectedPoint >= 0 && root.selectedPoint < root.lanePoints.length) {
                            root.editSelected(root.lanePoints[root.selectedPoint].time, (value - root.valueOffset) / root.valueScale)
                        }
                    }
                }
                FlatButton {
                    width: parent.width
                    text: qsTrc("automation", "Delete point")
                    enabled: automation.editable && root.selectedPoint >= 0
                    navigation.panel: navigationPanel
                    navigation.order: 7
                    onClicked: {
                        automation.removePoint(root.selectedPoint)
                        root.selectedPoint = -1
                    }
                }
                FlatButton {
                    width: parent.width
                    text: qsTrc("automation", "Clear lane")
                    enabled: automation.editable && root.lanePoints.length > 0
                    navigation.panel: navigationPanel
                    navigation.order: 8
                    onClicked: {
                        automation.clearLane()
                        root.selectedPoint = -1
                    }
                }
                StyledTextLabel {
                    text: qsTrc("automation", "Zoom")
                }
                StyledSlider {
                    width: parent.width
                    from: 20
                    to: 300
                    stepSize: 10
                    value: root.zoom
                    navigation.panel: navigationPanel
                    navigation.order: 9
                    onMoved: root.zoom = value
                }
            }

            Flickable {
                id: viewport
                Layout.fillWidth: true
                Layout.fillHeight: true
                contentWidth: Math.max(width, (automation.duration + 1) * root.zoom)
                contentHeight: height
                boundsBehavior: Flickable.StopAtBounds
                clip: true
                ScrollBar.horizontal: StyledScrollBar {}

                Rectangle {
                    id: chart
                    width: viewport.contentWidth
                    height: viewport.height
                    color: ui.theme.backgroundSecondaryColor
                    onWidthChanged: line.requestPaint()
                    onHeightChanged: line.requestPaint()

                    Repeater {
                        model: Math.ceil(chart.width / root.zoom)
                        Item {
                            required property int index
                            x: index * root.zoom
                            height: chart.height
                            Rectangle {
                                y: 28
                                width: 1
                                height: parent.height - 28
                                color: ui.theme.strokeColor
                                opacity: 0.5
                            }
                            StyledTextLabel {
                                height: 24
                                text: index
                            }
                        }
                    }
                    Repeater {
                        model: 5
                        Rectangle {
                            required property int index
                            y: 28 + index / 4 * root.plotHeight
                            width: chart.width
                            height: 1
                            color: ui.theme.strokeColor
                            opacity: 0.3
                        }
                    }
                    Canvas {
                        id: line
                        anchors.fill: parent
                        onPaint: {
                            const context = getContext("2d")
                            context.clearRect(0, 0, width, height)
                            context.strokeStyle = ui.theme.accentColor
                            context.lineWidth = 2
                            context.beginPath()
                            const points = root.lanePoints
                            const initial = points.length > 0 ? root.pointValue(0) : automation.defaultValue
                            context.moveTo(0, 28 + (1 - initial) * root.plotHeight)
                            for (let i = 0; i < points.length; ++i) {
                                context.lineTo(root.pointTime(i) * root.zoom, 28 + (1 - root.pointValue(i)) * root.plotHeight)
                            }
                            const last = points.length > 0 ? root.pointValue(points.length - 1) : automation.defaultValue
                            context.lineTo(width, 28 + (1 - last) * root.plotHeight)
                            context.stroke()
                        }
                    }
                    MouseArea {
                        anchors.fill: parent
                        enabled: automation.editable
                        onDoubleClicked: function (mouse) {
                            if (mouse.y >= 28) {
                                automation.setPoint(-1, root.timeAt(mouse.x), root.valueAt(mouse.y))
                            }
                        }
                    }
                    Repeater {
                        model: root.lanePoints
                        Rectangle {
                            id: point
                            required property int index
                            required property var modelData
                            x: root.pointTime(index) * root.zoom - width / 2
                            y: 28 + (1 - root.pointValue(index)) * root.plotHeight - height / 2
                            width: 10
                            height: 10
                            radius: 5
                            color: ui.theme.accentColor
                            border.color: ui.theme.fontPrimaryColor
                            border.width: root.selectedPoint === index ? 2 : 0
                            MouseArea {
                                anchors.fill: parent
                                anchors.margins: -4
                                acceptedButtons: Qt.LeftButton | Qt.RightButton
                                enabled: automation.editable
                                onPressed: {
                                    root.selectedPoint = point.index
                                    root.dragTime = point.modelData.time
                                    root.dragValue = point.modelData.value
                                    root.dragIndex = point.index
                                }
                                onPositionChanged: function (mouse) {
                                    if (pressed && pressedButtons === Qt.LeftButton) {
                                        const position = mapToItem(chart, mouse.x, mouse.y)
                                        root.dragTime = root.timeAt(position.x)
                                        root.dragValue = root.valueAt(position.y)
                                    }
                                }
                                onReleased: function (mouse) {
                                    const index = root.dragIndex
                                    root.dragIndex = -1
                                    if (mouse.button === Qt.RightButton) {
                                        automation.removePoint(index)
                                        root.selectedPoint = -1
                                    } else if (root.dragTime !== point.modelData.time || root.dragValue !== point.modelData.value) {
                                        automation.setPoint(index, root.dragTime, root.dragValue)
                                        root.selectedPoint = root.lanePoints.findIndex(point => Math.abs(point.time - root.dragTime) < 1e-7)
                                    }
                                }
                                onCanceled: root.dragIndex = -1
                            }
                        }
                    }
                }
            }
        }

        StyledTextLabel {
            Layout.fillWidth: true
            text: qsTrc("automation", "Double-click to add a point. Drag to move. Right-click to remove. Each completed edit has one undo step.")
            horizontalAlignment: Text.AlignLeft
        }
    }
}
