import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

// Tightening in bulk: pick a preset, review its steps (each can be left out),
// apply. Every step is a separate, checked and undoable change; the recent
// changes, from any app, are listed with Undo.
Rectangle {
    id: panel
    signal closed()
    property var chosen: ({})
    property bool asking: false
    readonly property real u: theme.fontScale
    color: Qt.rgba(0, 0, 0, 0.6)

    function load(id) {
        perms.loadPreset(id)
        var all = {}
        for (var i = 0; i < perms.presetSteps.length; ++i)
            all[i] = true
        chosen = all
    }
    function selected() {
        var list = []
        for (var i = 0; i < perms.presetSteps.length; ++i)
            if (chosen[i])
                list.push(i)
        return list
    }

    // Reopened: the current preset again, read from the latest sample, every
    // step ticked.
    onVisibleChanged: {
        if (!visible)
            return
        perms.refreshJournal()
        if (perms.presetId !== "")
            load(perms.presetId)
    }

    MouseArea {
        anchors.fill: parent
        onClicked: panel.closed()
    }

    Rectangle {
        anchors.centerIn: parent
        width: Math.min(680 * panel.u, panel.width - 48)
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
                    Label {
                        Layout.fillWidth: true
                        text: qsTr("Tighten")
                        color: theme.text
                        font.pixelSize: Math.round(18 * panel.u)
                        font.weight: Font.DemiBold
                    }
                    BusyIndicator {
                        running: perms.busy
                        visible: perms.busy
                        Layout.preferredWidth: 28 * panel.u
                        Layout.preferredHeight: 28 * panel.u
                    }
                }
                Label {
                    Layout.fillWidth: true
                    wrapMode: Text.Wrap
                    color: theme.muted
                    font.pixelSize: Math.round(12 * panel.u)
                    text: qsTr("Only third-party apps. An app that loses a permission can ask for it again, and Android will show you the prompt.")
                }

                Repeater {
                    model: perms.presets
                    delegate: RadioButton {
                        required property var modelData
                        Layout.fillWidth: true
                        checked: perms.presetId === modelData.id
                        text: modelData.title
                        font.pixelSize: Math.round(13 * panel.u)
                        onClicked: panel.load(modelData.id)
                    }
                }

                Label {
                    visible: perms.presetId !== "" && perms.presetSteps.length === 0
                    text: qsTr("Nothing to do: no app matches.")
                    color: theme.good
                    font.pixelSize: Math.round(13 * panel.u)
                }

                Repeater {
                    model: perms.presetSteps
                    delegate: CheckBox {
                        required property var modelData
                        required property int index
                        Layout.fillWidth: true
                        checked: !!panel.chosen[index]
                        text: modelData.title + "  —  " + modelData.reason + "  (" + modelData.package + ")"
                        font.pixelSize: Math.round(12 * panel.u)
                        onToggled: {
                            var copy = Object.assign({}, panel.chosen)
                            copy[index] = checked
                            panel.chosen = copy
                        }
                    }
                }

                Label {
                    readonly property var r: perms.lastResult
                    visible: r && r.preset === true
                    Layout.fillWidth: true
                    wrapMode: Text.Wrap
                    color: r && r.held === r.applied ? theme.good : theme.warn
                    font.pixelSize: Math.round(13 * panel.u)
                    text: r && r.preset ? qsTr("%1 of %2 changes checked on the phone.").arg(r.held).arg(r.applied) : ""
                }

                RowLayout {
                    Layout.alignment: Qt.AlignRight
                    Button {
                        flat: true
                        text: qsTr("Close")
                        onClicked: panel.closed()
                    }
                    Button {
                        highlighted: true
                        enabled: !perms.busy && panel.selected().length > 0
                        visible: perms.presetSteps.length > 0
                        text: qsTr("Apply %n changes", "", panel.selected().length)
                        onClicked: panel.asking = true
                    }
                }

                Label {
                    visible: perms.journal.length > 0
                    Layout.topMargin: 8 * panel.u
                    text: qsTr("Recent changes made by Snoopy")
                    color: theme.muted
                    font.pixelSize: Math.round(11 * panel.u)
                    font.weight: Font.DemiBold
                    font.capitalization: Font.AllUppercase
                }
                Repeater {
                    model: perms.journal.slice(-12).reverse()
                    delegate: RowLayout {
                        required property var modelData
                        Layout.fillWidth: true
                        Label {
                            Layout.fillWidth: true
                            text: modelData.package + "  ·  " + modelData.label + ": " + modelData.from + " → " + modelData.to
                                  + (modelData.verified ? "" : "  ·  " + qsTr("not confirmed"))
                                  + (modelData.undone ? "  ·  " + qsTr("undone") : "")
                            color: modelData.undone ? theme.muted : theme.text
                            font.pixelSize: Math.round(11 * panel.u)
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
            }
        }
    }

    ChangeConfirm {
        anchors.fill: parent
        visible: panel.asking
        plan: panel.asking ? {
            "label": qsTr("%n apps' permissions", "", panel.selected().length),
            "detail": qsTr("%1 changes, one at a time. Each is checked on the phone and can be undone from the list below.").arg(panel.selected().length),
            "commands": perms.presetSteps.filter(function (s, i) { return panel.chosen[i] }).map(function (s) { return s.title }),
            "blocked": false
        } : null
        onAccepted: {
            perms.applyPreset(panel.selected())
            panel.asking = false
        }
        onRejected: panel.asking = false
    }
}
