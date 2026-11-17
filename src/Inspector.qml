import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "kinds.js" as Kinds

// The selected (or hovered) node in full. With nothing selected, what the
// colours mean and what the sample can and can't show.
Rectangle {
    id: inspector

    property var node: null
    signal manage(string pkg)
    readonly property real u: theme.fontScale

    color: theme.panel
    radius: theme.radius
    border.color: theme.panelBorder
    border.width: 1

    component Heading: Label {
        Layout.fillWidth: true
        Layout.topMargin: 6 * inspector.u
        color: theme.muted
        font.pixelSize: Math.round(11 * inspector.u)
        font.weight: Font.DemiBold
        font.capitalization: Font.AllUppercase
        wrapMode: Text.Wrap
    }
    component Body: Label {
        Layout.fillWidth: true
        color: theme.text
        font.pixelSize: Math.round(13 * inspector.u)
        wrapMode: Text.Wrap
        textFormat: Text.PlainText
    }
    component Dot: Rectangle {
        width: 10 * inspector.u
        height: width
        radius: width / 2
    }
    // One "app — detail" or "label: detail" entry.
    component Entry: ColumnLayout {
        property string title
        property string detail
        property color mark: theme.text
        Layout.fillWidth: true
        spacing: 2
        RowLayout {
            Layout.fillWidth: true
            spacing: 8
            Dot { color: mark }
            Label {
                Layout.fillWidth: true
                text: title
                color: theme.text
                font.pixelSize: Math.round(13 * inspector.u)
                font.weight: Font.DemiBold
                elide: Text.ElideRight
            }
        }
        Label {
            Layout.fillWidth: true
            Layout.leftMargin: 18 * inspector.u
            visible: detail !== ""
            text: detail
            color: theme.muted
            font.pixelSize: Math.round(12 * inspector.u)
            wrapMode: Text.Wrap
            textFormat: Text.PlainText
        }
    }

    ScrollView {
        id: scroll
        anchors.fill: parent
        anchors.margins: 16 * inspector.u
        contentWidth: availableWidth
        clip: true

        ColumnLayout {
            width: scroll.availableWidth
            spacing: 8 * inspector.u

            // Nothing selected: the legend and the limits.
            ColumnLayout {
                visible: inspector.node === null
                Layout.fillWidth: true
                spacing: 8 * inspector.u

                Label {
                    Layout.fillWidth: true
                    text: qsTr("What you see")
                    color: theme.text
                    font.pixelSize: Math.round(16 * inspector.u)
                    font.weight: Font.DemiBold
                }
                Repeater {
                    model: [
                        [theme.information, qsTr("Information an app read in the last day, as the phone recorded it.")],
                        [theme.text, qsTr("Apps, ordered by bytes sent and received since the phone's counters last reset.")],
                        [theme.destination, qsTr("Where an app has an open socket: the address, its reverse DNS name and the port.")],
                        [theme.lookup, qsTr("Names the phone looked up for an app (dashed), and known trackers among them.")]
                    ]
                    delegate: RowLayout {
                        required property var modelData
                        Layout.fillWidth: true
                        spacing: 10
                        Dot { color: modelData[0]; Layout.alignment: Qt.AlignTop; Layout.topMargin: 4 * inspector.u }
                        Body { text: modelData[1] }
                    }
                }
                Heading { text: qsTr("Limits") }
                Repeater {
                    model: monitor.limits
                    delegate: Body {
                        required property string modelData
                        text: "· " + modelData
                        color: theme.muted
                        font.pixelSize: Math.round(12 * inspector.u)
                    }
                }
                Body {
                    visible: monitor.limits.length === 0
                    color: theme.muted
                    text: qsTr("Nothing is decrypted: an HTTPS connection shows its address, not its contents.")
                }
                Heading { text: qsTr("Dumps that failed"); visible: monitor.errors.length > 0 }
                Repeater {
                    model: monitor.errors
                    delegate: Body {
                        required property string modelData
                        text: modelData
                        color: theme.warn
                        font.pixelSize: Math.round(12 * inspector.u)
                    }
                }
                Body {
                    Layout.topMargin: 8 * inspector.u
                    color: theme.muted
                    font.pixelSize: Math.round(12 * inspector.u)
                    text: qsTr("Hover or click a circle, or press J to start at the first app.")
                }
            }

            // A node.
            ColumnLayout {
                visible: inspector.node !== null
                Layout.fillWidth: true
                spacing: 6 * inspector.u

                readonly property var n: inspector.node || ({ type: "", label: "", lines: [] })
                readonly property var card: n.type === "app" ? n.card : null

                Label {
                    Layout.fillWidth: true
                    text: parent.n.type === "information" ? Kinds.information(parent.n.label, translator.language) : parent.n.label
                    color: theme.text
                    font.pixelSize: Math.round(18 * inspector.u)
                    font.weight: Font.DemiBold
                    wrapMode: Text.WrapAnywhere
                }
                Label {
                    Layout.fillWidth: true
                    color: parent.n.type === "information" ? theme.information
                         : parent.n.type === "destination" ? theme.destination : theme.muted
                    font.pixelSize: Math.round(12 * inspector.u)
                    text: parent.n.type === "information" ? qsTr("Information read")
                        : parent.n.type === "app" ? qsTr("App")
                        : parent.n.relation === "lookup" ? qsTr("Name looked up") : qsTr("Destination")
                }

                // App: package, bytes, reads and destinations.
                Body {
                    visible: parent.card !== null && !!parent.card.package
                    text: parent.card ? parent.card.package : ""
                    color: theme.muted
                    font.family: "monospace"
                    font.pixelSize: Math.round(12 * inspector.u)
                    wrapMode: Text.WrapAnywhere
                }
                Button {
                    visible: parent.card !== null && !!parent.card.package
                    text: qsTr("Permissions… (P)")
                    flat: true
                    onClicked: inspector.manage(parent.card.package)
                }
                Body {
                    visible: parent.card !== null && (parent.card.tx_bytes > 0 || parent.card.rx_bytes > 0)
                    text: parent.card ? qsTr("%1 sent, %2 received").arg(Kinds.bytes(parent.card.tx_bytes, translator.language))
                                                                     .arg(Kinds.bytes(parent.card.rx_bytes, translator.language)) : ""
                }
                Body {
                    visible: parent.card !== null && (parent.card.destinations || []).length === 0
                    color: theme.muted
                    text: qsTr("No open connection in this sample.")
                }
                Heading { text: qsTr("Read"); visible: parent.card !== null && (parent.card.reads || []).length > 0 }
                Repeater {
                    model: parent.card ? parent.card.reads : []
                    delegate: Entry {
                        required property var modelData
                        title: Kinds.information(modelData.label, translator.language)
                        detail: modelData.detail
                        mark: theme.information
                    }
                }
                Heading { text: qsTr("Connected to"); visible: parent.card !== null && (parent.card.destinations || []).length > 0 }
                Repeater {
                    model: parent.card ? parent.card.destinations : []
                    delegate: Entry {
                        required property var modelData
                        title: (modelData.name || modelData.label) + " · " + Kinds.connection(modelData.kind, translator.language)
                        detail: modelData.detail
                        mark: modelData.relation === "lookup" ? theme.lookup : theme.destination
                    }
                }

                // Destination: names and the address.
                Body {
                    visible: parent.n.type === "destination" && !!parent.n.name && parent.n.name !== parent.n.label
                    text: parent.n.name || ""
                }
                Body {
                    visible: parent.n.type === "destination" && !!parent.n.host && parent.n.host !== parent.n.label
                    text: parent.n.host || ""
                    color: theme.muted
                    font.family: "monospace"
                }
                Body {
                    visible: parent.n.type === "destination"
                    text: Kinds.connection(parent.n.kind || "", translator.language)
                    color: theme.muted
                }

                // Information and destinations: one entry per app.
                Heading {
                    visible: parent.n.type !== "app" && parent.n.lines.length > 0
                    text: parent.n.type === "information" ? qsTr("Read by") : qsTr("From")
                }
                Repeater {
                    model: parent.n.type !== "app" ? parent.n.lines : []
                    delegate: Entry {
                        required property var modelData
                        title: modelData.app
                        detail: modelData.text
                        mark: theme.text
                    }
                }
            }
        }
    }

    // A fade over the bottom edge while there is more below, so a cut-off
    // list doesn't read as the end of it.
    Rectangle {
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        anchors.margins: 1
        height: 36 * inspector.u
        radius: theme.radius
        visible: !scroll.contentItem.atYEnd
        gradient: Gradient {
            GradientStop { position: 0; color: Qt.alpha(theme.panel, 0) }
            GradientStop { position: 1; color: theme.panel }
        }
        Label {
            anchors.horizontalCenter: parent.horizontalCenter
            anchors.bottom: parent.bottom
            anchors.bottomMargin: 2
            text: "⌄"
            color: theme.muted
            font.pixelSize: Math.round(16 * inspector.u)
        }
    }
}
