import QtQuick
import QtQuick.Controls

// A compact button for rows of findings: an outlined pill, accent-filled
// when it is the row's main action.
Rectangle {
    id: button
    property string text
    property bool primary: false
    signal clicked()
    readonly property real u: theme.fontScale

    implicitWidth: label.implicitWidth + 20 * u
    implicitHeight: Math.round(26 * u)
    radius: Math.min(4, theme.radius)
    color: primary ? (area.containsMouse ? Qt.lighter(theme.accent, 1.1) : theme.accent)
                   : (area.containsMouse ? Qt.alpha(theme.text, 0.08) : "transparent")
    border.color: primary ? theme.accent : theme.panelBorder
    border.width: 1

    Label {
        id: label
        anchors.centerIn: parent
        text: button.text
        color: button.primary ? theme.accentForeground : theme.text
        font.pixelSize: Math.round(12 * button.u)
    }
    MouseArea {
        id: area
        anchors.fill: parent
        hoverEnabled: true
        cursorShape: Qt.PointingHandCursor
        onClicked: button.clicked()
    }
}
