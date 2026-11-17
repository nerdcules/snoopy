import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

// Asks before a change to the phone: what it does, and the exact commands.
// Enter applies, Esc cancels.
Rectangle {
    id: confirm
    property var plan: null
    signal accepted()
    signal rejected()
    readonly property real u: theme.fontScale
    color: Qt.rgba(0, 0, 0, 0.5)

    onVisibleChanged: if (visible) apply.forceActiveFocus()

    MouseArea {
        anchors.fill: parent
        onClicked: confirm.rejected()
    }

    Rectangle {
        anchors.centerIn: parent
        width: Math.min(520 * confirm.u, confirm.width - 48)
        height: body.implicitHeight + 40
        color: theme.panel
        radius: theme.radius
        border.color: theme.accent
        border.width: 1

        MouseArea { anchors.fill: parent }

        ColumnLayout {
            id: body
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: parent.top
            anchors.margins: 20
            spacing: 10 * confirm.u

            Label {
                Layout.fillWidth: true
                text: confirm.plan ? (confirm.plan.blocked ? qsTr("Can't change this") : qsTr("Change %1?").arg(confirm.plan.label)) : ""
                color: theme.text
                font.pixelSize: Math.round(16 * confirm.u)
                font.weight: Font.DemiBold
                wrapMode: Text.Wrap
            }
            Label {
                Layout.fillWidth: true
                text: confirm.plan ? (confirm.plan.blocked ? confirm.plan.reason : confirm.plan.detail) : ""
                color: theme.text
                font.pixelSize: Math.round(13 * confirm.u)
                wrapMode: Text.Wrap
            }
            Label {
                visible: confirm.plan && !confirm.plan.blocked
                Layout.fillWidth: true
                text: confirm.plan ? (confirm.plan.commands || []).join("\n") : ""
                color: theme.muted
                font.family: "monospace"
                font.pixelSize: Math.round(11 * confirm.u)
                wrapMode: Text.WrapAnywhere
            }
            Label {
                visible: confirm.plan && !confirm.plan.blocked
                Layout.fillWidth: true
                text: qsTr("Snoopy checks the result on the phone afterwards and keeps it in its list of changes, where it can be undone.")
                color: theme.muted
                font.pixelSize: Math.round(11 * confirm.u)
                wrapMode: Text.Wrap
            }
            RowLayout {
                Layout.alignment: Qt.AlignRight
                spacing: 8
                Button {
                    flat: true
                    text: qsTr("Cancel")
                    onClicked: confirm.rejected()
                }
                Button {
                    id: apply
                    visible: confirm.plan && !confirm.plan.blocked
                    highlighted: true
                    text: qsTr("Apply")
                    onClicked: confirm.accepted()
                    Keys.onReturnPressed: confirm.accepted()
                    Keys.onEnterPressed: confirm.accepted()
                    Keys.onEscapePressed: confirm.rejected()
                }
            }
        }
    }
}
