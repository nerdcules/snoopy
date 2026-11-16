import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts
import "graph.js" as G

ApplicationWindow {
    id: win
    width: 1280
    height: 820
    minimumWidth: 860
    minimumHeight: 560
    visible: true
    title: "Snoopy"
    color: theme.window

    Material.theme: Material.Dark
    Material.accent: theme.accent
    Material.roundedScale: theme.radius

    property bool helpVisible: false
    property bool wirelessVisible: false
    property int page: 0
    property bool logVisible: true
    readonly property real u: theme.fontScale
    // Narrow or tiled windows put the inspector under the graph.
    readonly property bool wide: width >= 1100 * u

    // Esc steps back one thing at a time: help, the filter, the selection.
    function showWireless(show) {
        if (show && !wirelessVisible)
            pairing.start()
        else if (!show)
            pairing.cancel()
        wirelessVisible = show
    }

    function stepBack() {
        if (wirelessVisible)
            showWireless(false)
        else if (helpVisible)
            helpVisible = false
        else if (filter.activeFocus || filter.text !== "") {
            filter.text = ""
            filter.focus = false
            graph.forceActiveFocus()
        } else
            graph.selectedId = ""
    }

    Shortcut { sequence: "?"; context: Qt.ApplicationShortcut; onActivated: win.helpVisible = !win.helpVisible }
    Shortcut { sequence: "Escape"; context: Qt.ApplicationShortcut; onActivated: win.stepBack() }
    Shortcut { sequence: "Q"; context: Qt.ApplicationShortcut; onActivated: Qt.quit() }
    Shortcut { sequence: "Space"; context: Qt.ApplicationShortcut; onActivated: monitor.paused = !monitor.paused }
    Shortcut { sequence: "R"; context: Qt.ApplicationShortcut; onActivated: monitor.sampleNow() }
    Shortcut { sequence: "/"; context: Qt.ApplicationShortcut; onActivated: filter.forceActiveFocus() }
    Shortcut { sequences: ["J", "Down"]; context: Qt.ApplicationShortcut; onActivated: graph.step(1) }
    Shortcut { sequences: ["K", "Up"]; context: Qt.ApplicationShortcut; onActivated: graph.step(-1) }
    Shortcut { sequences: ["H", "Left"]; context: Qt.ApplicationShortcut; onActivated: graph.side(-1) }
    Shortcut { sequences: ["L", "Right"]; context: Qt.ApplicationShortcut; onActivated: graph.side(1) }
    Shortcut { sequence: "E"; context: Qt.ApplicationShortcut; onActivated: win.logVisible = !win.logVisible }
    Shortcut { sequence: "W"; context: Qt.ApplicationShortcut; onActivated: win.showWireless(!win.wirelessVisible) }
    Shortcut { sequence: "1"; context: Qt.ApplicationShortcut; onActivated: win.page = 0 }
    Shortcut { sequence: "2"; context: Qt.ApplicationShortcut; onActivated: win.page = 1 }
    Shortcut { sequence: "Tab"; context: Qt.ApplicationShortcut; onActivated: win.page = 1 - win.page }
    Shortcut { sequence: "M"; context: Qt.ApplicationShortcut; onActivated: monitor.acknowledgeFindings() }
    Shortcut { sequence: "I"; context: Qt.ApplicationShortcut; onActivated: watchView.showInfo = !watchView.showInfo }
    Shortcut { sequence: "T"; context: Qt.ApplicationShortcut; onActivated: monitor.link = monitor.link === "wifi" ? "usb" : "wifi" }
    Shortcut { sequence: "C"; context: Qt.ApplicationShortcut; onActivated: monitor.clearEvents() }
    Shortcut { sequences: ["+", "="]; context: Qt.ApplicationShortcut; onActivated: monitor.interval = monitor.interval - 5 }
    Shortcut { sequence: "-"; context: Qt.ApplicationShortcut; onActivated: monitor.interval = monitor.interval + 5 }
    Shortcut { sequences: ["Ctrl++", "Ctrl+="]; context: Qt.ApplicationShortcut; onActivated: theme.increaseFont() }
    Shortcut { sequence: "Ctrl+-"; context: Qt.ApplicationShortcut; onActivated: theme.decreaseFont() }
    Shortcut { sequence: "Ctrl+0"; context: Qt.ApplicationShortcut; onActivated: theme.resetFont() }

    // Right-to-left languages mirror the window; the Translator sets the
    // application's layout direction.
    Item {
        anchors.fill: parent
        LayoutMirroring.enabled: Qt.application.layoutDirection === Qt.RightToLeft
        LayoutMirroring.childrenInherit: true

        ColumnLayout {
            anchors.fill: parent
            anchors.margins: 16
            spacing: 12

            RowLayout {
                Layout.fillWidth: true
                spacing: 16

                Label {
                    text: "Snoopy"
                    color: theme.text
                    font.pixelSize: Math.round(20 * win.u)
                    font.weight: Font.DemiBold
                }

                Row {
                    spacing: 4
                    Repeater {
                        model: [qsTr("Graph"), monitor.newFindings > 0 ? qsTr("Watch (%n new)", "", monitor.newFindings) : qsTr("Watch")]
                        delegate: Rectangle {
                            required property int index
                            required property string modelData
                            readonly property bool active: win.page === index
                            width: tabLabel.implicitWidth + 28
                            height: Math.round(30 * win.u)
                            radius: theme.radius
                            color: active ? theme.accent : "transparent"
                            border.color: active ? theme.accent : (index === 1 && monitor.newFindings > 0 ? theme.warn : theme.panelBorder)
                            border.width: 1
                            Label {
                                id: tabLabel
                                anchors.centerIn: parent
                                text: parent.modelData
                                color: parent.active ? theme.accentForeground : theme.text
                                font.pixelSize: Math.round(13 * win.u)
                                font.weight: Font.DemiBold
                            }
                            MouseArea {
                                anchors.fill: parent
                                cursorShape: Qt.PointingHandCursor
                                onClicked: win.page = parent.index
                            }
                        }
                    }
                }

                Label {
                    readonly property var d: monitor.device
                    visible: !!d.model
                    text: d.android ? qsTr("%1 · Android %2").arg(d.model).arg(d.android) : (d.model || "")
                    color: theme.text
                    font.pixelSize: Math.round(13 * win.u)
                }

                // Cable or Wi-Fi: only the chosen one is used (T).
                Row {
                    spacing: 0
                    Repeater {
                        model: [["usb", qsTr("Cable")], ["wifi", qsTr("Wi-Fi")]]
                        delegate: Rectangle {
                            required property var modelData
                            required property int index
                            readonly property bool active: monitor.link === modelData[0]
                            width: linkLabel.implicitWidth + 20
                            height: Math.round(24 * win.u)
                            radius: Math.min(4, theme.radius)
                            color: active ? theme.accent : "transparent"
                            border.color: active ? theme.accent : theme.panelBorder
                            border.width: 1
                            Label {
                                id: linkLabel
                                anchors.centerIn: parent
                                text: parent.modelData[1]
                                color: parent.active ? theme.accentForeground : theme.muted
                                font.pixelSize: Math.round(12 * win.u)
                                font.weight: parent.active ? Font.DemiBold : Font.Normal
                            }
                            MouseArea {
                                anchors.fill: parent
                                cursorShape: Qt.PointingHandCursor
                                onClicked: monitor.link = parent.modelData[0]
                            }
                        }
                    }
                }

                Item { Layout.fillWidth: true }

                TextField {
                    id: filter
                    visible: win.page === 0
                    Layout.preferredWidth: 260 * win.u
                    placeholderText: qsTr("Filter apps")
                    font.pixelSize: Math.round(13 * win.u)
                    Keys.onEscapePressed: win.stepBack()
                }

                Rectangle {
                    width: statusRow.implicitWidth + 20
                    height: Math.round(26 * win.u)
                    radius: theme.radius
                    color: theme.panel
                    border.color: theme.panelBorder
                    border.width: 1
                    readonly property color dot: monitor.paused ? theme.warn
                                               : monitor.status === "live" ? theme.good
                                               : monitor.status === "waiting" ? theme.bad : theme.muted
                    Row {
                        id: statusRow
                        anchors.centerIn: parent
                        spacing: 8
                        Rectangle {
                            width: 8
                            height: 8
                            radius: 4
                            anchors.verticalCenter: parent.verticalCenter
                            color: parent.parent.dot
                            SequentialAnimation on opacity {
                                running: monitor.busy
                                loops: Animation.Infinite
                                NumberAnimation { to: 0.25; duration: 500 }
                                NumberAnimation { to: 1; duration: 500 }
                            }
                        }
                        Label {
                            anchors.verticalCenter: parent.verticalCenter
                            text: monitor.paused ? qsTr("Paused")
                                : monitor.busy ? qsTr("Sampling…")
                                : monitor.status === "live" ? qsTr("Live")
                                : monitor.status === "waiting" ? qsTr("No phone") : qsTr("Starting")
                            color: theme.text
                            font.pixelSize: Math.round(12 * win.u)
                        }
                    }
                }

                Label {
                    text: {
                        translator.language
                        var every = qsTr("every %1 s").arg(monitor.interval)
                        return monitor.samples > 0
                               ? qsTr("updated %1, %2").arg(Qt.formatTime(monitor.updatedAt, "HH:mm:ss")).arg(every)
                               : every
                    }
                    color: theme.muted
                    font.pixelSize: Math.round(12 * win.u)
                }
            }

            Rectangle {
                Layout.fillWidth: true
                visible: monitor.message !== ""
                implicitHeight: errorLabel.implicitHeight + 16
                radius: theme.radius
                color: Qt.alpha(theme.warn, 0.12)
                border.color: Qt.alpha(theme.warn, 0.5)
                border.width: 1
                Label {
                    id: errorLabel
                    anchors.fill: parent
                    anchors.margins: 8
                    anchors.leftMargin: 12
                    text: monitor.samples > 0
                          ? qsTr("%1 Showing the last sample, from %2.").arg(monitor.message)
                                .arg(Qt.formatTime(monitor.updatedAt, "HH:mm:ss"))
                          : monitor.message
                    color: theme.warn
                    font.pixelSize: Math.round(13 * win.u)
                    wrapMode: Text.Wrap
                }
            }

            StackLayout {
                Layout.fillWidth: true
                Layout.fillHeight: true
                currentIndex: win.page

                GridLayout {
                    columns: win.wide ? 2 : 1
                    rowSpacing: 12
                    columnSpacing: 12

                    GraphView {
                        id: graph
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        Layout.minimumHeight: 240 * win.u
                        cards: monitor.graph
                        query: filter.text
                        focus: true
                    }

                    Inspector {
                        Layout.preferredWidth: win.wide ? 360 * win.u : -1
                        Layout.fillWidth: !win.wide
                        Layout.fillHeight: win.wide
                        Layout.preferredHeight: win.wide ? -1 : 220 * win.u
                        node: G.find(graph.graph, graph.focusId)
                    }
                }

                WatchView {
                    id: watchView
                }
            }

            EventLog {
                Layout.fillWidth: true
                Layout.preferredHeight: 150 * win.u
                visible: win.logVisible && win.page === 0
            }

            Label {
                Layout.fillWidth: true
                text: win.page === 1
                      ? qsTr("1/2 graph/watch · M mark seen · I info · R sample · T cable/Wi-Fi · ? help · Q quit")
                      : qsTr("1/2 graph/watch · Space pause · R sample · / filter · J/K H/L select · T cable/Wi-Fi · W pair · ? help · Q quit")
                color: theme.muted
                font.pixelSize: Math.round(12 * win.u)
                elide: Text.ElideRight
            }
        }

        HelpOverlay {
            anchors.fill: parent
            visible: win.helpVisible
            onClosed: win.helpVisible = false
        }

        WirelessOverlay {
            anchors.fill: parent
            visible: win.wirelessVisible
            onClosed: win.showWireless(false)
        }
    }
}
