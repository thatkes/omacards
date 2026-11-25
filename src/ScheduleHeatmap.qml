import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// One Forecast module. It owns its grid and its legend, so it can be moved
// without dragging the rest of the Stats panel along.
Rectangle {
    id: root
    objectName: "scheduleHeatmap"

    // Each entry is { date, count } for one upcoming Study day.
    property var days: []
    property color accentColor: theme.accent
    property color raisedColor: theme.raised
    property color borderColor: theme.border
    readonly property int bandCount: 4
    readonly property int maxCount: {
        let busiest = 0
        for (let i = 0; i < (root.days || []).length; ++i)
            busiest = Math.max(busiest, Number(root.days[i].count || 0))
        return busiest
    }

    function alpha(color, amount) {
        return Qt.rgba(color.r, color.g, color.b, amount)
    }

    // Bands are relative to the busiest day, so a quiet deck and a busy deck
    // both use the full four-step scale. All shades come from the live theme.
    function band(count) {
        if (!count)
            return 0
        return Math.max(1, Math.min(root.bandCount,
                                    Math.ceil(Number(count) / Math.max(1, root.maxCount) * root.bandCount)))
    }

    function shade(count) {
        const level = root.band(count)
        if (level === 0)
            return alpha(root.raisedColor, 0.35)
        return alpha(root.accentColor, 0.2 + level * 0.18)
    }

    implicitWidth: 340
    implicitHeight: column.implicitHeight
    radius: Math.min(10, theme.radius)
    color: alpha(theme.panel, 0.55)
    border.width: 1
    border.color: alpha(root.borderColor, 0.8)

    ColumnLayout {
        id: column
        anchors.fill: parent
        anchors.margins: 12
        spacing: 8

        Label {
            objectName: "scheduleHeatmapTitle"
            Layout.fillWidth: true
            text: qsTr("Schedule heatmap")
            color: theme.foreground
            font.pixelSize: 14
            font.weight: Font.DemiBold
        }

        Grid {
            id: grid
            objectName: "scheduleHeatmapGrid"
            Layout.alignment: Qt.AlignHCenter
            columns: 15
            spacing: 4

            Repeater {
                model: root.days

                Rectangle {
                    objectName: "scheduleHeatmapCell"
                    readonly property int dayIndex: index
                    readonly property int dayCount: Number(modelData.count || 0)
                    readonly property int band: root.band(dayCount)
                    width: 16
                    height: 16
                    radius: Math.min(3, theme.radius)
                    color: root.shade(dayCount)
                    border.width: band > 0 ? 1 : 0
                    border.color: alpha(root.borderColor, 0.9)

                    ToolTip.visible: mouse.containsMouse && dayCount > 0
                    ToolTip.text: modelData.date + ": " + dayCount

                    MouseArea {
                        id: mouse
                        anchors.fill: parent
                        hoverEnabled: true
                    }
                }
            }
        }

        RowLayout {
            objectName: "scheduleHeatmapLegend"
            Layout.alignment: Qt.AlignHCenter
            spacing: 8

            Repeater {
                model: [qsTr("None"), qsTr("Light"), qsTr("Medium"), qsTr("Heavy"), qsTr("Busiest")]
                delegate: RowLayout {
                    objectName: "scheduleHeatmapLegendEntry"
                    spacing: 4

                    Rectangle {
                        width: 10
                        height: 10
                        radius: Math.min(2, theme.radius)
                        color: root.shade(index)
                    }
                    Label {
                        text: modelData
                        color: theme.muted
                        font.pixelSize: 10
                    }
                }
            }
        }
    }
}
