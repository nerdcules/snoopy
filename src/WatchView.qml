import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Things on the phone worth a look, from the last sample, grouped. Each line
// says what was seen and what it can and can't mean; NEW marks what the
// phone's baseline didn't have (M marks it all seen).
Item {
    id: view

    property bool showInfo: false
    signal manage(string pkg, string control, string to)
    signal tighten()
    readonly property real u: theme.fontScale
    readonly property var groups: [
        ["background", qsTr("Used while not on screen"),
         qsTr("The last day from this sample, and earlier this week from Snoopy's own history (kept 30 days). Some of this is expected: navigation, a call, step counting, a music recognizer, backups. It is worth a look when you can't think why an app would.")],
        ["control", qsTr("Can watch or control the phone"), ""],
        ["network", qsTr("Network"), ""],
        ["outside", qsTr("From outside an app store"), ""],
        ["device", qsTr("The phone"), ""]
    ]
    readonly property var shown: (monitor.findings || []).filter(function (f) { return view.showInfo || f.level !== "info" })
    readonly property int hiddenInfo: (monitor.findings || []).filter(function (f) { return f.level === "info" }).length

    function rows(group) {
        return shown.filter(function (f) { return f.group === group })
    }
    function levelColor(level) {
        return level === "alert" ? theme.bad : level === "warn" ? theme.warn : theme.muted
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 10 * view.u

        RowLayout {
            Layout.fillWidth: true
            spacing: 12
            Label {
                Layout.fillWidth: true
                wrapMode: Text.Wrap
                color: monitor.newFindings > 0 ? theme.warn : theme.muted
                font.pixelSize: Math.round(13 * view.u)
                text: monitor.samples === 0
                      ? qsTr("Nothing to show until the first sample.")
                      : monitor.newFindings > 0
                        ? qsTr("%n new since you last marked them seen.", "", monitor.newFindings)
                        : qsTr("Nothing new since the first look at this phone. The first look is the baseline: later changes are marked NEW.")
            }
            Button {
                visible: monitor.newFindings > 0
                text: qsTr("Mark all seen (M)")
                onClicked: monitor.acknowledgeFindings()
            }
            Button {
                visible: monitor.samples > 0
                text: qsTr("Tighten… (Shift+T)")
                onClicked: view.tighten()
            }
            Button {
                flat: true
                text: view.showInfo ? qsTr("Hide info (I)") : qsTr("Show %n info lines (I)", "", view.hiddenInfo)
                onClicked: view.showInfo = !view.showInfo
            }
        }

        ScrollView {
            id: scroll
            Layout.fillWidth: true
            Layout.fillHeight: true
            contentWidth: availableWidth
            clip: true

            ColumnLayout {
                width: scroll.availableWidth
                spacing: 14 * view.u

                Label {
                    visible: monitor.samples > 0 && view.shown.length === 0
                    Layout.fillWidth: true
                    wrapMode: Text.Wrap
                    color: theme.good
                    font.pixelSize: Math.round(14 * view.u)
                    text: qsTr("No alerts or warnings in this sample.")
                }

                Repeater {
                    model: view.groups
                    delegate: ColumnLayout {
                        required property var modelData
                        readonly property var items: view.rows(modelData[0])
                        visible: items.length > 0
                        Layout.fillWidth: true
                        spacing: 6 * view.u

                        Label {
                            text: parent.modelData[1]
                            color: theme.muted
                            font.pixelSize: Math.round(11 * view.u)
                            font.weight: Font.DemiBold
                            font.capitalization: Font.AllUppercase
                        }
                        Label {
                            visible: parent.modelData[2] !== ""
                            Layout.fillWidth: true
                            text: parent.modelData[2]
                            color: theme.muted
                            font.pixelSize: Math.round(12 * view.u)
                            wrapMode: Text.Wrap
                        }

                        Repeater {
                            model: parent.items
                            delegate: Rectangle {
                                id: findingRow
                                required property var modelData
                                readonly property string level: modelData.level
                                Layout.fillWidth: true
                                opacity: modelData.earlier ? 0.75 : 1
                                implicitHeight: body.implicitHeight + 20 * view.u
                                radius: theme.radius
                                color: theme.panel
                                border.color: modelData.new ? Qt.alpha(theme.warn, 0.6) : theme.panelBorder
                                border.width: 1

                                Rectangle {
                                    width: 4
                                    anchors.left: parent.left
                                    anchors.top: parent.top
                                    anchors.bottom: parent.bottom
                                    anchors.margins: 1
                                    radius: Math.min(2, theme.radius)
                                    color: view.levelColor(modelData.level)
                                }

                                ColumnLayout {
                                    id: body
                                    anchors.left: parent.left
                                    anchors.right: parent.right
                                    anchors.verticalCenter: parent.verticalCenter
                                    anchors.leftMargin: 16 * view.u
                                    anchors.rightMargin: 12 * view.u
                                    spacing: 3 * view.u

                                    RowLayout {
                                        Layout.fillWidth: true
                                        spacing: 8
                                        Label {
                                            visible: !!modelData.new
                                            text: qsTr("NEW")
                                            color: theme.window
                                            font.pixelSize: Math.round(10 * view.u)
                                            font.weight: Font.Bold
                                            leftPadding: 5
                                            rightPadding: 5
                                            background: Rectangle { color: theme.warn; radius: Math.min(3, theme.radius) }
                                        }
                                        Label {
                                            Layout.fillWidth: true
                                            text: modelData.title
                                            color: theme.text
                                            font.pixelSize: Math.round(14 * view.u)
                                            font.weight: Font.DemiBold
                                            wrapMode: Text.Wrap
                                            textFormat: Text.PlainText
                                        }
                                    }
                                    Label {
                                        Layout.fillWidth: true
                                        text: modelData.detail
                                        color: theme.muted
                                        font.pixelSize: Math.round(12 * view.u)
                                        wrapMode: Text.Wrap
                                        textFormat: Text.PlainText
                                    }
                                    // The last 7 days, oldest first: a square per day,
                                    // stronger for more use.
                                    RowLayout {
                                        visible: !!modelData.week
                                        spacing: 3
                                        Repeater {
                                            model: modelData.week || []
                                            delegate: Rectangle {
                                                required property int modelData
                                                required property int index
                                                width: 12 * view.u
                                                height: width
                                                radius: Math.min(2, theme.radius)
                                                color: modelData === 0 ? Qt.alpha(theme.muted, 0.18)
                                                                       : Qt.alpha(view.levelColor(findingRow.level),
                                                                                  Math.min(1, 0.35 + modelData / 6))
                                                border.width: index === 6 ? 1 : 0
                                                border.color: theme.muted
                                            }
                                        }
                                        Label {
                                            Layout.leftMargin: 6
                                            text: modelData.week ? qsTr("on %1 of the last 7 days, %n times", "", modelData.weekTimes).arg(modelData.weekDays) : ""
                                            color: theme.muted
                                            font.pixelSize: Math.round(11 * view.u)
                                        }
                                    }
                                    RowLayout {
                                        visible: !!modelData.package
                                        spacing: 6
                                        SmallButton {
                                            visible: !!modelData.fix
                                            primary: true
                                            text: modelData.fix ? modelData.fix.label : ""
                                            onClicked: view.manage(modelData.package, modelData.fix.control, modelData.fix.to)
                                        }
                                        SmallButton {
                                            text: qsTr("Permissions…")
                                            onClicked: view.manage(modelData.package, "", "")
                                        }
                                    }
                                    Label {
                                        Layout.fillWidth: true
                                        visible: !!modelData.package || !!modelData.since
                                        text: {
                                            translator.language
                                            var parts = []
                                            if (modelData.package)
                                                parts.push(modelData.package)
                                            if (modelData.since)
                                                parts.push(qsTr("first seen %1").arg(new Date(modelData.since).toLocaleString(Qt.locale(), Locale.ShortFormat)))
                                            return parts.join("  ·  ")
                                        }
                                        color: theme.muted
                                        font.family: "monospace"
                                        font.pixelSize: Math.round(11 * view.u)
                                        elide: Text.ElideRight
                                    }
                                }
                            }
                        }
                    }
                }

                Label {
                    Layout.fillWidth: true
                    Layout.topMargin: 6 * view.u
                    wrapMode: Text.Wrap
                    color: theme.muted
                    font.pixelSize: Math.round(11 * view.u)
                    text: qsTr("What this can't see without root: what an app does inside itself beyond what Android records, the modem and SIM, anything that hides from the phone's own reports, user-installed certificates (the shell isn't allowed to list them), and logs older than the log buffer. A quiet page doesn't prove the phone is clean.")
                }
            }
        }
    }
}
