import QtQuick
import Quickshell
import Quickshell.Io
import qs.Commons
import qs.Commons as Commons
import qs.Ui

// The phone Snoopy reads, in the bar. Its status comes from
// ~/.local/state/snoopy/status.json, which the Snoopy window or its daemon
// (snoopy.service) rewrites after every sample. The phone glyph is dim with
// no phone or a stale status, red with a count when the Watch page has
// alerts, amber when something new is worth a look. Click opens Snoopy on
// the Watch page when there is something new, otherwise on the graph.
BarWidget {
  id: root
  moduleName: "snoopy.phone"

  readonly property string path: Quickshell.env("HOME") + "/.local/state/snoopy/status.json"
  property var status: null
  // A status older than this (the daemon samples every 5 minutes) means
  // nothing is reading the phone.
  readonly property int staleSeconds: 15 * 60
  property real now: Date.now()

  Commons.Translations { id: strings; directory: Qt.resolvedUrl("i18n") }

  readonly property bool fresh: status !== null && status.updatedAt
                                && (now - Date.parse(status.updatedAt)) / 1000 < staleSeconds
  readonly property bool connected: fresh && status.connected === true
  readonly property int alerts: connected ? (status.alerts || 0) : 0
  readonly property int newCount: connected ? (status.newCount || 0) : 0
  readonly property string tone: alerts > 0 ? "bad" : newCount > 0 ? "warn" : connected ? "ok" : "off"

  readonly property string tooltip: {
    var lines = []
    if (!status) {
      lines.push(strings.t("none"))
    } else if (!fresh) {
      lines.push(strings.t("stale"))
    } else if (!status.connected) {
      lines.push(status.message ? String(status.message) : strings.t("noPhone"))
    } else {
      lines.push(strings.t(status.transport === "wifi" ? "overWifi" : "overCable").arg(String(status.model || "")))
      if (alerts > 0) lines.push(strings.tn("alerts", alerts).arg(alerts))
      if ((status.warnings || 0) > 0) lines.push(strings.tn("warnings", status.warnings).arg(status.warnings))
      if (newCount > 0) lines.push(strings.tn("new", newCount).arg(newCount))
      if (alerts === 0 && (status.warnings || 0) === 0) lines.push(strings.t("quiet"))
    }
    lines.push(strings.t("hint"))
    return lines.join("\n")
  }

  function open(page) {
    Quickshell.execDetached(["snoopy", "open", "--page", page])
  }

  implicitWidth: button.implicitWidth
  implicitHeight: button.implicitHeight

  FileView {
    id: file
    path: root.path
    watchChanges: true
    printErrors: false
    onFileChanged: reload()
    onLoaded: {
      try {
        var parsed = JSON.parse(text())
        root.status = parsed && typeof parsed === "object" ? parsed : null
      } catch (e) {}
    }
    onLoadFailed: root.status = null
  }

  // The status file is replaced whole (rename), and a watch can fail to
  // follow the new file; read it again now and then, and keep "stale"
  // honest as time passes.
  Timer {
    interval: 30000
    running: true
    repeat: true
    onTriggered: {
      root.now = Date.now()
      file.reload()
    }
  }

  WidgetButton {
    id: button
    anchors.fill: parent
    bar: root.bar
    text: ""
    labelVisible: false
    hasVisualContent: true
    horizontalMargin: 10
    fixedWidth: root.vertical ? -1 : row.implicitWidth + Style.spaceReal(horizontalMargin) * 2
    fixedHeight: root.vertical ? row.implicitHeight + Style.spaceReal(verticalPadding) * 2 : -1
    tooltipText: root.tooltip

    onPressed: function(b) {
      root.open(b === Qt.RightButton || root.newCount > 0 || root.alerts > 0 ? "watch" : "graph")
    }

    Row {
      id: row
      anchors.centerIn: parent
      spacing: Style.spaceReal(5)
      scale: button.pressScale

      Label {
        text: ""
        elide: Text.ElideNone
        color: root.tone === "bad" ? Commons.Color.urgent : root.tone === "warn" ? "#ffb547" : button.foreground
        opacity: root.tone === "off" ? 0.45 : 1
        font.family: button.fontFamily
        font.pixelSize: button.fontSize
        renderType: Text.NativeRendering
      }
      Label {
        visible: root.alerts > 0 || root.newCount > 0
        text: String(root.alerts > 0 ? root.alerts : root.newCount)
        elide: Text.ElideNone
        color: root.tone === "bad" ? Commons.Color.urgent : "#ffb547"
        font.family: button.fontFamily
        font.pixelSize: button.fontSize
        font.weight: Font.DemiBold
        renderType: Text.NativeRendering
      }
    }
  }
}
