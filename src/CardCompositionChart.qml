import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// The second Forecast module. It keeps its own four slices and legend, and
// does not reach into the heatmap or the nine Stats rows.
Rectangle {
    id: root
    objectName: "cardCompositionChart"

    // { new, learning, young, mature } from the Forecast read.
    property var slices: ({})
    property color panelColor: theme.panel
    property color raisedColor: theme.raised
    property color borderColor: theme.border
    property color softColor: theme.soft
    property color redColor: theme.red
    property color yellowColor: theme.yellow
    property color greenColor: theme.green
    readonly property var keys: ["new", "learning", "young", "mature"]
    readonly property var names: [qsTr("New"), qsTr("Learning"), qsTr("Young"), qsTr("Mature")]
    readonly property int total: {
        let sum = 0
        for (let i = 0; i < root.keys.length; ++i)
            sum += root.countAt(i)
        return sum
    }

    function countAt(index) {
        return Number((root.slices || {})[root.keys[index]] || 0)
    }

    function colorAt(index) {
        switch (index) {
        case 0: return root.softColor
        case 1: return root.redColor
        case 2: return root.yellowColor
        default: return root.greenColor
        }
    }

    function alpha(color, amount) {
        return Qt.rgba(color.r, color.g, color.b, amount)
    }

    onSlicesChanged: pie.requestPaint()
    Component.onCompleted: pie.requestPaint()
    Connections {
        target: theme
        function onChanged() {
            pie.requestPaint()
        }
    }

    implicitWidth: 340
    implicitHeight: column.implicitHeight
    radius: Math.min(10, theme.radius)
    color: alpha(root.panelColor, 0.55)
    border.width: 1
    border.color: alpha(root.borderColor, 0.8)

    ColumnLayout {
        id: column
        anchors.fill: parent
        anchors.margins: 12
        spacing: 8

        Label {
            objectName: "cardCompositionChartTitle"
            Layout.fillWidth: true
            text: qsTr("Card composition")
            color: theme.foreground
            font.pixelSize: 14
            font.weight: Font.DemiBold
        }

        RowLayout {
            Layout.alignment: Qt.AlignHCenter
            spacing: 16

            Canvas {
                id: pie
                objectName: "cardCompositionPie"
                width: 96
                height: 96
                Layout.preferredWidth: 96
                Layout.preferredHeight: 96

                onPaint: {
                    const ctx = getContext("2d")
                    ctx.reset()
                    const centerX = width / 2
                    const centerY = height / 2
                    const radius = Math.min(width, height) / 2 - 2
                    if (root.total === 0) {
                        ctx.beginPath()
                        ctx.arc(centerX, centerY, radius, 0, Math.PI * 2)
                        ctx.fillStyle = alpha(root.raisedColor, 0.7)
                        ctx.fill()
                        return
                    }
                    let start = -Math.PI / 2
                    for (let i = 0; i < root.keys.length; ++i) {
                        const share = root.countAt(i) / root.total
                        if (share <= 0)
                            continue
                        const end = start + share * Math.PI * 2
                        ctx.beginPath()
                        ctx.moveTo(centerX, centerY)
                        ctx.arc(centerX, centerY, radius, start, end)
                        ctx.closePath()
                        ctx.fillStyle = root.colorAt(i)
                        ctx.fill()
                        start = end
                    }
                    ctx.beginPath()
                    ctx.arc(centerX, centerY, radius * 0.48, 0, Math.PI * 2)
                    ctx.fillStyle = root.panelColor
                    ctx.fill()
                }
            }

            ColumnLayout {
                objectName: "cardCompositionLegend"
                spacing: 6

                Repeater {
                    model: root.names

                    RowLayout {
                        objectName: "compositionSlice"
                        readonly property string sliceName: modelData
                        readonly property int sliceCount: root.countAt(index)
                        readonly property color sliceColor: root.colorAt(index)
                        spacing: 8

                        Rectangle {
                            width: 10
                            height: 10
                            radius: Math.min(2, theme.radius)
                            color: parent.sliceColor
                        }
                        Label {
                            Layout.minimumWidth: 62
                            text: parent.sliceName
                            color: theme.muted
                            font.pixelSize: 11
                        }
                        Label {
                            objectName: "compositionSliceCount"
                            text: parent.sliceCount
                            color: theme.foreground
                            font.pixelSize: 11
                            font.weight: Font.DemiBold
                        }
                    }
                }
            }
        }
    }
}
