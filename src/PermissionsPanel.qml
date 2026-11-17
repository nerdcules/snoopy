import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

// One app's permissions, special access and on/off state, read from the
// phone. Every change is shown first (what it does and the exact adb
// commands), then applied, then read back from the phone; the app's changes
// can be undone from here.
Rectangle {
    id: panel
    signal closed()
    // Opened from a Watch finding: ask about this change straight away.
    property string askControl: ""
    property string askTo: ""
    property var pending: null
    readonly property real u: theme.fontScale
    color: Qt.rgba(0, 0, 0, 0.6)

    function stateName(state) {
        switch (state) {
        case "allow": return qsTr("Allow")
        case "deny": return qsTr("Deny")
        case "always": return qsTr("All the time")
        case "while_using": return qsTr("While using")
        case "on": return qsTr("On")
        case "off": return qsTr("Off")
        case "active": return qsTr("Running")
        case "paused": return qsTr("Paused")
        case "enabled": return qsTr("Enabled")
        case "disabled": return qsTr("Disabled")
        case "installed": return qsTr("Installed")
        case "removed": return qsTr("Removed")
        }
        return state
    }
    function ask(control, to) {
        var plan = perms.planFor(control, to)
        if (plan.commands.length === 0 && !plan.blocked)
            return
        pending = plan
    }

    Connections {
        target: perms
        function onAppChanged() {
            if (panel.visible && panel.askControl !== "" && perms.controls.length > 0 && !perms.busy) {
                panel.ask(panel.askControl, panel.askTo)
                panel.askControl = ""
            }
        }
    }

    MouseArea {
        anchors.fill: parent
        onClicked: panel.closed()
    }

    Rectangle {
        id: card
        anchors.centerIn: parent
        width: Math.min(640 * panel.u, panel.width - 48)
        height: Math.min(panel.height - 48, column.implicitHeight + 48)
        color: theme.panel
        radius: theme.radius
        border.color: theme.panelBorder
        border.width: 1

        MouseArea { anchors.fill: parent }

        ScrollView {
            id: scroll
            anchors.fill: parent
            anchors.margins: 24
            contentWidth: availableWidth
            clip: true

            ColumnLayout {
                id: column
                width: scroll.availableWidth
                spacing: 10 * panel.u

                RowLayout {
                    Layout.fillWidth: true
                    spacing: 12
                    Image {
                        Layout.preferredWidth: 40 * panel.u
                        Layout.preferredHeight: 40 * panel.u
                        sourceSize: Qt.size(96, 96)
                        source: perms.packageName !== "" ? "image://appicon/" + encodeURIComponent(perms.packageName) : ""
                        asynchronous: true
                    }
                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 0
                        Label {
                            text: perms.appName
                            color: theme.text
                            font.pixelSize: Math.round(18 * panel.u)
                            font.weight: Font.DemiBold
                        }
                        Label {
                            text: perms.packageName + (perms.system ? "  ·  " + qsTr("system app") : "")
                            color: theme.muted
                            font.family: "monospace"
                            font.pixelSize: Math.round(11 * panel.u)
                        }
                    }
                    BusyIndicator {
                        running: perms.busy
                        visible: perms.busy
                        Layout.preferredWidth: 28 * panel.u
                        Layout.preferredHeight: 28 * panel.u
                    }
                }

                Label {
                    visible: perms.error !== ""
                    Layout.fillWidth: true
                    text: perms.error
                    color: theme.warn
                    wrapMode: Text.Wrap
                    font.pixelSize: Math.round(13 * panel.u)
                }

                // The last change's outcome, as the phone reports it.
                Label {
                    readonly property var r: perms.lastResult
                    visible: r && r.control !== undefined
                    Layout.fillWidth: true
                    wrapMode: Text.Wrap
                    font.pixelSize: Math.round(13 * panel.u)
                    color: r && r.verified ? theme.good : theme.warn
                    text: !r || r.control === undefined ? ""
                          : r.verified ? qsTr("Done: %1 is now %2, checked on the phone.").arg(r.label).arg(panel.stateName(r.now))
                          : r.undone !== undefined ? (r.undone ? qsTr("Put back: %1 is %2 again.").arg(r.label).arg(panel.stateName(r.undoNow))
                                                               : qsTr("Couldn't put it back: %1").arg(r.undoOutput || ""))
                          : qsTr("It didn't take: the phone says %1 is %2. %3").arg(r.label).arg(panel.stateName(r.now || "?")).arg(r.output || "")
                }

                Label {
                    visible: perms.busy && perms.controls.length === 0
                    text: qsTr("Reading the phone…")
                    color: theme.muted
                }

                Repeater {
                    model: [["permission", qsTr("Permissions")], ["special", qsTr("Special access")], ["app", qsTr("The app")]]
                    delegate: ColumnLayout {
                        required property var modelData
                        readonly property var rows: perms.controls.filter(function (c) { return c.group === modelData[0] })
                        visible: rows.length > 0
                        Layout.fillWidth: true
                        spacing: 6 * panel.u

                        Label {
                            Layout.topMargin: 6 * panel.u
                            text: parent.modelData[1]
                            color: theme.muted
                            font.pixelSize: Math.round(11 * panel.u)
                            font.weight: Font.DemiBold
                            font.capitalization: Font.AllUppercase
                        }

                        Repeater {
                            model: parent.rows
                            delegate: RowLayout {
                                required property var modelData
                                Layout.fillWidth: true
                                spacing: 10
                                ColumnLayout {
                                    Layout.fillWidth: true
                                    spacing: 1
                                    Label {
                                        Layout.fillWidth: true
                                        text: modelData.label
                                        color: theme.text
                                        font.pixelSize: Math.round(13 * panel.u)
                                        wrapMode: Text.Wrap
                                    }
                                    Label {
                                        visible: !modelData.changeable && modelData.reason !== ""
                                        Layout.fillWidth: true
                                        text: modelData.reason
                                        color: theme.muted
                                        font.pixelSize: Math.round(11 * panel.u)
                                        wrapMode: Text.Wrap
                                    }
                                }
                                Row {
                                    spacing: 0
                                    Repeater {
                                        model: modelData.options
                                        delegate: Rectangle {
                                            required property string modelData
                                            readonly property bool current: modelData === parent.parent.modelData.state
                                            readonly property bool enabledOption: parent.parent.modelData.changeable && !perms.busy
                                            width: optionLabel.implicitWidth + 18
                                            height: Math.round(26 * panel.u)
                                            radius: Math.min(4, theme.radius)
                                            color: current ? theme.accent : "transparent"
                                            border.color: current ? theme.accent : theme.panelBorder
                                            border.width: 1
                                            opacity: enabledOption || current ? 1 : 0.45
                                            Label {
                                                id: optionLabel
                                                anchors.centerIn: parent
                                                text: panel.stateName(parent.modelData)
                                                color: parent.current ? theme.accentForeground : theme.text
                                                font.pixelSize: Math.round(12 * panel.u)
                                            }
                                            MouseArea {
                                                anchors.fill: parent
                                                enabled: parent.enabledOption && !parent.current
                                                cursorShape: enabled ? Qt.PointingHandCursor : Qt.ArrowCursor
                                                onClicked: panel.ask(parent.parent.parent.modelData.id, parent.modelData)
                                            }
                                        }
                                    }
                                }
                            }
                        }
                    }
                }

                // This app's changes, newest first, each with Undo.
                Label {
                    readonly property var mine: perms.journal.filter(function (e) { return e.package === perms.packageName })
                    visible: mine.length > 0
                    Layout.topMargin: 8 * panel.u
                    text: qsTr("Changes made by Snoopy")
                    color: theme.muted
                    font.pixelSize: Math.round(11 * panel.u)
                    font.weight: Font.DemiBold
                    font.capitalization: Font.AllUppercase
                }
                Repeater {
                    model: perms.journal.filter(function (e) { return e.package === perms.packageName }).reverse()
                    delegate: RowLayout {
                        required property var modelData
                        Layout.fillWidth: true
                        Label {
                            Layout.fillWidth: true
                            text: { translator.language; return qsTr("%1: %2 → %3").arg(modelData.label).arg(panel.stateName(modelData.from)).arg(panel.stateName(modelData.to))
                                    + "  ·  " + new Date(modelData.at).toLocaleString(Qt.locale(), Locale.ShortFormat)
                                    + (modelData.undone ? "  ·  " + qsTr("undone") : "") }
                            color: modelData.undone ? theme.muted : theme.text
                            font.pixelSize: Math.round(12 * panel.u)
                            elide: Text.ElideRight
                        }
                        Button {
                            visible: !modelData.undone && modelData.ok
                            enabled: !perms.busy
                            flat: true
                            text: qsTr("Undo")
                            onClicked: perms.undo(modelData.id)
                        }
                    }
                }

                RowLayout {
                    Layout.alignment: Qt.AlignRight
                    Button {
                        flat: true
                        text: qsTr("Close")
                        onClicked: panel.closed()
                    }
                }
            }
        }
    }

    ChangeConfirm {
        anchors.fill: parent
        plan: panel.pending
        visible: panel.pending !== null
        onAccepted: {
            perms.apply(panel.pending.control, panel.pending.to)
            panel.pending = null
        }
        onRejected: panel.pending = null
    }
}
