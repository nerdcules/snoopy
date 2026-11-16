import QtQuick
import QtQuick.Controls
import "kinds.js" as Kinds

// One circle in the graph with its caption: an information kind (amber, a
// glyph), an app (its launcher icon) or a destination (teal, a globe).
Item {
    id: root

    property var node
    property bool lit: false
    property bool dim: false
    property bool selected: false
    signal hovered(bool inside)
    signal clicked()

    readonly property real u: theme.fontScale
    readonly property real r: (node.type === "app" ? 34 : node.type === "information" ? 28 : 25) * u
    readonly property color tint: node.type === "information" ? theme.information
                                : node.type === "destination"
                                  ? (node.relation === "lookup" ? theme.lookup : theme.destination)
                                  : theme.text

    width: Math.round(160 * u)
    height: r * 2 + caption.implicitHeight + (sub.visible ? sub.implicitHeight : 0) + 8 * u
    x: node.x - width / 2
    y: node.y - r
    opacity: dim ? 0.3 : 1
    Behavior on opacity { NumberAnimation { duration: 120 } }

    Rectangle {
        id: circle
        anchors.horizontalCenter: parent.horizontalCenter
        width: root.r * 2
        height: width
        radius: width / 2
        color: root.node.type === "app" ? theme.panel : Qt.alpha(root.tint, 0.14)
        border.color: root.selected ? theme.accent : root.lit ? root.tint : Qt.alpha(root.tint, 0.55)
        border.width: (root.selected ? 3 : root.lit ? 2 : 1.5) * root.u

        Image {
            visible: root.node.type === "app"
            anchors.centerIn: parent
            width: parent.width * 0.72
            height: width
            asynchronous: true
            sourceSize: Qt.size(Math.round(width * 2), Math.round(width * 2))
            source: root.node.type === "app"
                    ? "image://appicon/" + encodeURIComponent(root.node.card.package || root.node.card.name)
                    : ""
            fillMode: Image.PreserveAspectFit
            smooth: true
        }

        Glyph {
            visible: root.node.type !== "app"
            anchors.centerIn: parent
            size: parent.width * 0.52
            name: root.node.type === "information" ? root.node.label : "Destination"
            color: root.tint
        }
    }

    Label {
        id: caption
        anchors.top: circle.bottom
        anchors.topMargin: 6 * root.u
        width: parent.width
        horizontalAlignment: Text.AlignHCenter
        elide: Text.ElideMiddle
        text: root.node.type === "information" ? Kinds.information(root.node.label, translator.language) : root.node.label
        color: theme.text
        font.pixelSize: Math.round(13 * root.u)
        font.weight: root.node.type === "app" ? Font.DemiBold : Font.Normal
    }

    Label {
        id: sub
        visible: root.node.type === "destination" && !!root.node.kind
        anchors.top: caption.bottom
        width: parent.width
        horizontalAlignment: Text.AlignHCenter
        elide: Text.ElideRight
        text: Kinds.connection(root.node.kind || "", translator.language)
        color: theme.muted
        font.pixelSize: Math.round(11 * root.u)
    }

    MouseArea {
        anchors.fill: parent
        hoverEnabled: true
        cursorShape: Qt.PointingHandCursor
        onEntered: root.hovered(true)
        onExited: root.hovered(false)
        onClicked: root.clicked()
    }
}
