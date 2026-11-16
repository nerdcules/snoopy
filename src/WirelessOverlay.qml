import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

// Pairs the phone for wireless debugging: a QR code the phone scans, then
// what adb is doing, until it is connected over Wi-Fi.
Rectangle {
    id: overlay
    signal closed()
    readonly property real u: theme.fontScale
    color: Qt.rgba(0, 0, 0, 0.6)

    MouseArea {
        anchors.fill: parent
        onClicked: overlay.closed()
    }

    Rectangle {
        anchors.centerIn: parent
        width: Math.min(560 * overlay.u, overlay.width - 48)
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
            spacing: 14 * overlay.u

            Label {
                text: qsTr("Connect over Wi-Fi")
                color: theme.text
                font.pixelSize: Math.round(18 * overlay.u)
                font.weight: Font.DemiBold
            }

            Rectangle {
                Layout.alignment: Qt.AlignHCenter
                visible: pairing.state === "waiting" && pairing.qrText !== ""
                width: 248 * overlay.u
                height: width
                color: "white"
                radius: Math.min(4, theme.radius)
                Image {
                    anchors.fill: parent
                    anchors.margins: 4
                    smooth: false
                    fillMode: Image.PreserveAspectFit
                    sourceSize: Qt.size(width, height)
                    source: pairing.qrText !== "" ? "image://qr/" + encodeURIComponent(pairing.qrText) : ""
                }
            }

            RowLayout {
                Layout.fillWidth: true
                spacing: 10
                BusyIndicator {
                    visible: pairing.state === "waiting" || pairing.state === "pairing" || pairing.state === "connecting"
                    running: visible
                    Layout.preferredWidth: 28 * overlay.u
                    Layout.preferredHeight: 28 * overlay.u
                }
                Label {
                    Layout.fillWidth: true
                    text: pairing.message
                    color: pairing.state === "failed" ? theme.warn : pairing.state === "connected" ? theme.good : theme.text
                    font.pixelSize: Math.round(13 * overlay.u)
                    wrapMode: Text.Wrap
                    textFormat: Text.PlainText
                }
            }

            Label {
                Layout.fillWidth: true
                visible: pairing.state === "waiting"
                text: qsTr("Pairing happens once. After that Snoopy finds the phone on the network whenever Wireless debugging is on.")
                color: theme.muted
                font.pixelSize: Math.round(12 * overlay.u)
                wrapMode: Text.Wrap
            }

            RowLayout {
                Layout.alignment: Qt.AlignRight
                spacing: 8
                Button {
                    visible: pairing.state === "failed"
                    text: qsTr("Try again")
                    onClicked: pairing.start()
                }
                Button {
                    text: pairing.state === "connected" ? qsTr("Done") : qsTr("Close")
                    flat: true
                    onClicked: overlay.closed()
                }
            }
        }
    }
}
