import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Flows that appeared or stopped between samples, newest first. The first
// sample is the baseline, so the log starts empty.
Rectangle {
    id: log
    readonly property real u: theme.fontScale

    color: theme.panel
    radius: theme.radius
    border.color: theme.panelBorder
    border.width: 1

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 12 * log.u
        spacing: 6 * log.u

        RowLayout {
            Layout.fillWidth: true
            Label {
                text: qsTr("Changes")
                color: theme.text
                font.pixelSize: Math.round(13 * log.u)
                font.weight: Font.DemiBold
            }
            Label {
                Layout.fillWidth: true
                visible: monitor.events.length > 0
                text: qsTr("%n changes", "", monitor.events.length)
                color: theme.muted
                font.pixelSize: Math.round(12 * log.u)
            }
        }

        ListView {
            id: list
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            model: monitor.events
            spacing: 2
            ScrollBar.vertical: ScrollBar { }
            delegate: RowLayout {
                required property var modelData
                width: list.width
                spacing: 10 * log.u
                readonly property bool appeared: modelData.change === "appeared"
                Label {
                    text: { translator.language; return Qt.formatTime(modelData.at, "HH:mm:ss") }
                    color: theme.muted
                    font.family: "monospace"
                    font.pixelSize: Math.round(12 * log.u)
                }
                Label {
                    Layout.preferredWidth: 80 * log.u
                    text: parent.appeared ? qsTr("appeared") : qsTr("stopped")
                    color: parent.appeared ? theme.good : theme.muted
                    font.pixelSize: Math.round(12 * log.u)
                }
                Label {
                    Layout.fillWidth: true
                    text: qsTr("%1 → %2").arg(modelData.source).arg(modelData.destination)
                          + (modelData.information ? "  ·  " + modelData.information : "")
                    color: theme.text
                    font.pixelSize: Math.round(12 * log.u)
                    elide: Text.ElideRight
                    textFormat: Text.PlainText
                }
            }
        }

        Label {
            visible: monitor.events.length === 0
            Layout.fillWidth: true
            text: monitor.samples < 2
                  ? qsTr("The first sample is the baseline. Changes show up from the next one.")
                  : qsTr("Nothing has appeared or stopped since the baseline.")
            color: theme.muted
            font.pixelSize: Math.round(12 * log.u)
            wrapMode: Text.Wrap
        }
    }
}
