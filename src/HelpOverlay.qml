import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Every shortcut, over the window, until Esc or a click outside.
Rectangle {
    id: overlay
    signal closed()
    color: Qt.rgba(0, 0, 0, 0.6)

    MouseArea {
        anchors.fill: parent
        onClicked: overlay.closed()
    }

    Rectangle {
        anchors.centerIn: parent
        width: Math.min(560, overlay.width - 48)
        height: column.implicitHeight + 48
        color: theme.panel
        radius: theme.radius
        border.color: theme.panelBorder
        border.width: 1

        MouseArea { anchors.fill: parent }

        ColumnLayout {
            id: column
            anchors.fill: parent
            anchors.margins: 24
            spacing: 14

            Label {
                text: qsTr("Keyboard")
                color: theme.text
                font.pixelSize: Math.round(18 * theme.fontScale)
                font.weight: Font.DemiBold
            }

            Repeater {
                model: [
                    ["1 / 2, Tab", qsTr("Show the graph or the Watch page")],
                    ["M / I", qsTr("Watch: mark everything seen / show the info lines")],
                    ["Space", qsTr("Pause or resume sampling")],
                    ["R", qsTr("Take a sample now")],
                    ["/", qsTr("Filter the apps; Esc clears the filter")],
                    ["J / K, ↓ / ↑", qsTr("Select the next or previous node in a column")],
                    ["H / L, ← / →", qsTr("Move to the column on the left or right")],
                    ["Esc", qsTr("Clear the selection")],
                    ["E", qsTr("Show or hide the changes")],
                    ["C", qsTr("Clear the changes")],
                    ["G", qsTr("Review my phone: a few questions, one at a time")],
                    ["P", qsTr("Permissions of the selected app: tighten or loosen")],
                    ["Shift + T", qsTr("Tighten presets, and undo recent changes")],
                    ["T", qsTr("Read the phone over the cable or over Wi-Fi")],
                    ["W", qsTr("Pair the phone for Wi-Fi (wireless debugging)")],
                    ["+ / -", qsTr("Sample more or less often")],
                    ["Ctrl + / -", qsTr("Make the text larger or smaller")],
                    ["?", qsTr("Show this help")],
                    ["Q", qsTr("Quit")]
                ]
                delegate: RowLayout {
                    Layout.fillWidth: true
                    spacing: 16
                    Label {
                        Layout.preferredWidth: Math.round(150 * theme.fontScale)
                        text: modelData[0]
                        color: theme.accent
                        font.family: "monospace"
                        font.pixelSize: Math.round(13 * theme.fontScale)
                    }
                    Label {
                        Layout.fillWidth: true
                        text: modelData[1]
                        color: theme.text
                        font.pixelSize: Math.round(13 * theme.fontScale)
                        wrapMode: Text.Wrap
                    }
                }
            }

            Label {
                Layout.topMargin: 6
                text: qsTr("Press Esc or click outside to close")
                color: theme.muted
                font.pixelSize: Math.round(12 * theme.fontScale)
            }
        }
    }
}
