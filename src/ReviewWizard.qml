import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

// "Review my phone": one question at a time, in plain words, with a
// suggested answer. Enter takes the suggestion, 1-3 pick, S skips,
// Backspace goes back, Esc stops. A change is made and checked on the phone
// straight away; Keep is remembered so it isn't asked again. The end lists
// what changed, each with Undo.
Rectangle {
    id: wizard
    signal closed()
    readonly property real u: theme.fontScale
    // "start", "question" or "done".
    property string stage: "start"
    property int index: 0
    property bool askAll: false
    // Per question: {state, result} once answered; "skip" when skipped.
    property var answers: ({})
    property var waiting: null // a result shown before moving on
    // Round 1: what Watch flags and location all the time. Round 2, offered
    // at the end of round 1: what apps can use but haven't in 30 days.
    property int round: 1
    readonly property var questions: perms.questions.filter(function (q) { return q.round === wizard.round })
    readonly property int laterCount: perms.questions.filter(function (q) { return q.round === 2 }).length
    readonly property var current: stage === "question" && index < questions.length ? questions[index] : null

    color: Qt.rgba(0, 0, 0, 0.65)

    function begin() {
        perms.loadReview(askAll)
        startRound(1)
    }
    function startRound(r) {
        round = r
        answers = ({})
        waiting = null
        index = 0
        stage = questions.length > 0 ? "question" : "done"
    }
    function record(i, value) {
        var copy = Object.assign({}, answers)
        copy[i] = value
        answers = copy
    }
    function next() {
        waiting = null
        if (index + 1 < questions.length)
            index += 1
        else
            stage = "done"
    }
    function choose(state) {
        if (!current || perms.busy)
            return
        // Answered already (or shown not to have taken): Enter moves on
        // rather than asking the phone twice.
        var done = answers[index]
        if (waiting || (done && done !== "skip" && done.result !== undefined)) {
            advance.stop()
            next()
            return
        }
        record(index, { state: state })
        perms.answer(index, state)
    }
    function skip() {
        if (!current || perms.busy)
            return
        record(index, "skip")
        next()
    }
    function back() {
        if (perms.busy || index === 0)
            return
        waiting = null
        index -= 1
    }
    function counts() {
        var c = { changed: 0, kept: 0, skipped: 0, failed: 0 }
        for (var i in answers) {
            var a = answers[i]
            if (a === "skip") c.skipped++
            else if (a.result && a.result.kept) c.kept++
            else if (a.result && a.result.verified) c.changed++
            else if (a.result) c.failed++
        }
        return c
    }

    onVisibleChanged: {
        if (visible) {
            stage = "start"
            keys.forceActiveFocus()
        }
    }

    Connections {
        target: perms
        function onAnswered(i, result) {
            var a = wizard.answers[i] || {}
            wizard.record(i, { state: a.state, result: result })
            if (result.kept || result.verified)
                advance.restart()
            else
                wizard.waiting = result // a change that didn't take waits for Enter
        }
    }
    Timer {
        id: advance
        interval: 700
        onTriggered: wizard.next()
    }

    // Holds the keyboard while the wizard is open, ahead of the window's
    // own shortcuts (1 and 2 switch pages there).
    Item {
        id: keys
        anchors.fill: parent
        focus: true
        Keys.onShortcutOverride: (event) => event.accepted = true
        Keys.onPressed: (event) => {
            event.accepted = true
            if (event.key === Qt.Key_Escape) { wizard.closed(); return }
            if (wizard.stage === "start") {
                if (event.key === Qt.Key_Return || event.key === Qt.Key_Enter) wizard.begin()
                return
            }
            if (wizard.stage === "done") {
                if (event.key === Qt.Key_Return || event.key === Qt.Key_Enter) wizard.closed()
                else if (event.key === Qt.Key_M && wizard.round === 1 && wizard.laterCount > 0) wizard.startRound(2)
                return
            }
            if (!wizard.current) return
            if (event.key === Qt.Key_Return || event.key === Qt.Key_Enter) wizard.choose(wizard.current.suggested)
            else if (event.key === Qt.Key_S || event.key === Qt.Key_Right) wizard.skip()
            else if (event.key === Qt.Key_Backspace || event.key === Qt.Key_Left) wizard.back()
            else if (event.key >= Qt.Key_1 && event.key <= Qt.Key_3) {
                var n = event.key - Qt.Key_1
                if (n < wizard.current.options.length) wizard.choose(wizard.current.options[n].state)
            }
        }
    }

    MouseArea {
        anchors.fill: parent
        onClicked: keys.forceActiveFocus()
    }

    Rectangle {
        anchors.centerIn: parent
        width: Math.min(620 * wizard.u, wizard.width - 48)
        height: Math.min(wizard.height - 48, body.implicitHeight + 56)
        color: theme.panel
        radius: theme.radius
        border.color: theme.panelBorder
        border.width: 1

        MouseArea { anchors.fill: parent; onClicked: keys.forceActiveFocus() }

        ColumnLayout {
            id: body
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: parent.top
            anchors.margins: 28
            spacing: 14 * wizard.u

            // ---- Start
            ColumnLayout {
                visible: wizard.stage === "start"
                Layout.fillWidth: true
                spacing: 12 * wizard.u
                Label {
                    text: qsTr("Review my phone")
                    color: theme.text
                    font.pixelSize: Math.round(22 * wizard.u)
                    font.weight: Font.DemiBold
                }
                Label {
                    Layout.fillWidth: true
                    wrapMode: Text.Wrap
                    color: theme.text
                    font.pixelSize: Math.round(14 * wizard.u)
                    text: qsTr("A few questions, one at a time, about what apps on your phone can do. Each has a suggested answer; press Enter to take it. Nothing changes without an answer, and everything can be undone at the end.")
                }
                Label {
                    Layout.fillWidth: true
                    wrapMode: Text.Wrap
                    color: theme.muted
                    font.pixelSize: Math.round(12 * wizard.u)
                    text: monitor.samples === 0 ? qsTr("Waiting for the first sample of the phone…")
                                                : qsTr("Based on the sample from %1. Apps you chose to keep before aren't asked about again.")
                                                      .arg(Qt.formatTime(monitor.updatedAt, "HH:mm"))
                }
                CheckBox {
                    text: qsTr("Ask about everything again, including what I kept")
                    checked: wizard.askAll
                    onToggled: { wizard.askAll = checked; keys.forceActiveFocus() }
                }
                RowLayout {
                    Layout.alignment: Qt.AlignRight
                    Button { flat: true; text: qsTr("Not now"); onClicked: wizard.closed() }
                    Button {
                        highlighted: true
                        enabled: monitor.samples > 0
                        text: qsTr("Start (Enter)")
                        onClicked: { wizard.begin(); keys.forceActiveFocus() }
                    }
                }
            }

            // ---- One question
            ColumnLayout {
                visible: wizard.stage === "question" && wizard.current !== null
                Layout.fillWidth: true
                spacing: 12 * wizard.u

                RowLayout {
                    Layout.fillWidth: true
                    Label {
                        Layout.fillWidth: true
                        text: (wizard.round === 2 ? qsTr("Unused permissions") + " · " : "")
                          + qsTr("Question %1 of %2").arg(wizard.index + 1).arg(wizard.questions.length)
                        color: theme.muted
                        font.pixelSize: Math.round(12 * wizard.u)
                    }
                    BusyIndicator {
                        running: perms.busy
                        visible: perms.busy
                        Layout.preferredWidth: 24 * wizard.u
                        Layout.preferredHeight: 24 * wizard.u
                    }
                }
                Rectangle {
                    Layout.fillWidth: true
                    height: 4
                    radius: 2
                    color: Qt.alpha(theme.muted, 0.25)
                    Rectangle {
                        width: parent.width * (wizard.index + 1) / Math.max(1, wizard.questions.length)
                        height: parent.height
                        radius: 2
                        color: theme.accent
                    }
                }
                RowLayout {
                    spacing: 14
                    Image {
                        Layout.preferredWidth: 52 * wizard.u
                        Layout.preferredHeight: 52 * wizard.u
                        sourceSize: Qt.size(104, 104)
                        asynchronous: true
                        source: wizard.current ? "image://appicon/" + encodeURIComponent(wizard.current.package) : ""
                    }
                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 2
                        Label {
                            Layout.fillWidth: true
                            text: wizard.current ? wizard.current.title : ""
                            color: theme.text
                            font.pixelSize: Math.round(18 * wizard.u)
                            font.weight: Font.DemiBold
                            wrapMode: Text.Wrap
                        }
                        Label {
                            text: wizard.current ? wizard.current.package : ""
                            color: theme.muted
                            font.family: "monospace"
                            font.pixelSize: Math.round(11 * wizard.u)
                        }
                    }
                }
                Label {
                    Layout.fillWidth: true
                    text: wizard.current ? wizard.current.why : ""
                    color: theme.text
                    font.pixelSize: Math.round(14 * wizard.u)
                    wrapMode: Text.Wrap
                }

                Repeater {
                    model: wizard.current ? wizard.current.options : []
                    delegate: Rectangle {
                        required property var modelData
                        required property int index
                        readonly property bool suggested: wizard.current && modelData.state === wizard.current.suggested
                        readonly property var answer: wizard.answers[wizard.index]
                        readonly property bool picked: answer && answer !== "skip" && answer.state === modelData.state
                        Layout.fillWidth: true
                        implicitHeight: optionRow.implicitHeight + 18 * wizard.u
                        radius: theme.radius
                        color: picked ? Qt.alpha(theme.accent, 0.25) : suggested ? Qt.alpha(theme.accent, 0.10) : "transparent"
                        border.color: suggested || picked ? theme.accent : theme.panelBorder
                        border.width: 1
                        opacity: perms.busy && !picked ? 0.5 : 1
                        RowLayout {
                            id: optionRow
                            anchors.left: parent.left
                            anchors.right: parent.right
                            anchors.verticalCenter: parent.verticalCenter
                            anchors.margins: 14
                            spacing: 12
                            Label {
                                text: String(index + 1)
                                color: theme.accent
                                font.family: "monospace"
                                font.pixelSize: Math.round(14 * wizard.u)
                                font.weight: Font.DemiBold
                            }
                            Label {
                                Layout.fillWidth: true
                                text: modelData.label
                                color: theme.text
                                font.pixelSize: Math.round(15 * wizard.u)
                            }
                            Label {
                                visible: parent.parent.suggested
                                text: qsTr("suggested · Enter")
                                color: theme.accent
                                font.pixelSize: Math.round(11 * wizard.u)
                            }
                        }
                        MouseArea {
                            anchors.fill: parent
                            cursorShape: Qt.PointingHandCursor
                            onClicked: { wizard.choose(parent.modelData.state); keys.forceActiveFocus() }
                        }
                    }
                }

                // A change that didn't take, until Enter.
                Label {
                    visible: wizard.waiting !== null
                    Layout.fillWidth: true
                    wrapMode: Text.Wrap
                    color: theme.warn
                    font.pixelSize: Math.round(13 * wizard.u)
                    text: wizard.waiting ? qsTr("It didn't take: %1 Press Enter to go on.").arg(wizard.waiting.output || qsTr("the phone reports no change."))
                                         : ""
                }
                Label {
                    readonly property var a: wizard.answers[wizard.index]
                    visible: wizard.waiting === null && a && a !== "skip" && a.result !== undefined
                    color: theme.good
                    font.pixelSize: Math.round(13 * wizard.u)
                    text: a && a.result ? (a.result.kept ? qsTr("Kept.") : qsTr("Done, checked on the phone.")) : ""
                }

                Label {
                    Layout.fillWidth: true
                    color: theme.muted
                    font.pixelSize: Math.round(11 * wizard.u)
                    text: qsTr("Enter: suggested · 1-3: choose · S: skip · Backspace: back · Esc: stop")
                    wrapMode: Text.Wrap
                }
            }

            // ---- The end
            ColumnLayout {
                visible: wizard.stage === "done"
                Layout.fillWidth: true
                spacing: 10 * wizard.u
                readonly property var c: wizard.counts()
                Label {
                    text: wizard.questions.length === 0 ? qsTr("Nothing to ask") : qsTr("Done")
                    color: theme.text
                    font.pixelSize: Math.round(22 * wizard.u)
                    font.weight: Font.DemiBold
                }
                Label {
                    Layout.fillWidth: true
                    wrapMode: Text.Wrap
                    color: theme.text
                    font.pixelSize: Math.round(14 * wizard.u)
                    text: wizard.questions.length === 0
                          ? qsTr("Nothing on the phone needs a decision right now.")
                          : qsTr("%1 changed, %2 kept, %3 skipped.").arg(parent.c.changed).arg(parent.c.kept).arg(parent.c.skipped)
                            + (parent.c.failed > 0 ? " " + qsTr("%1 didn't take.").arg(parent.c.failed) : "")
                }
                Repeater {
                    model: {
                        var list = []
                        for (var i = 0; i < wizard.questions.length; ++i) {
                            var a = wizard.answers[i]
                            if (a && a !== "skip" && a.result && a.result.verified && a.result.id !== undefined)
                                list.push({ q: wizard.questions[i], result: a.result })
                        }
                        return list
                    }
                    delegate: RowLayout {
                        required property var modelData
                        readonly property var entry: perms.journal.filter(function (e) { return e.id === modelData.result.id })[0]
                        Layout.fillWidth: true
                        Label {
                            Layout.fillWidth: true
                            text: modelData.q.title + (entry && entry.undone ? "  ·  " + qsTr("undone") : "")
                            color: entry && entry.undone ? theme.muted : theme.text
                            font.pixelSize: Math.round(12 * wizard.u)
                            elide: Text.ElideRight
                        }
                        SmallButton {
                            visible: !(entry && entry.undone)
                            text: qsTr("Undo")
                            onClicked: perms.undo(modelData.result.id)
                        }
                    }
                }
                Label {
                    visible: wizard.round === 1 && wizard.laterCount > 0
                    Layout.fillWidth: true
                    Layout.topMargin: 6 * wizard.u
                    wrapMode: Text.Wrap
                    color: theme.muted
                    font.pixelSize: Math.round(13 * wizard.u)
                    text: qsTr("%n more: permissions apps have but haven't used in 30 days (the camera, the microphone, contacts…). Worth a look when you have a few minutes.", "", wizard.laterCount)
                }
                RowLayout {
                    Layout.alignment: Qt.AlignRight
                    Button {
                        visible: wizard.round === 1 && wizard.laterCount > 0
                        flat: true
                        text: qsTr("Review those too (M)")
                        onClicked: { wizard.startRound(2); keys.forceActiveFocus() }
                    }
                    Button { highlighted: true; text: qsTr("Close (Enter)"); onClicked: wizard.closed() }
                }
            }
        }
    }
}
