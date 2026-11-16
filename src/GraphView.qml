import QtQuick
import QtQuick.Controls
import "graph.js" as G

// Information → app → destination, left to right. Hovering a node lights it
// and its neighbours; clicking (or J/K and H/L) selects one for the
// inspector.
Item {
    id: view

    property var cards: []
    property string query: ""
    property string selectedId: ""
    property string hoveredId: ""
    readonly property string focusId: hoveredId !== "" ? hoveredId : selectedId
    property var graph: ({ apps: [], infos: [], dests: [], edges: [] })
    readonly property var nodes: graph.infos.concat(graph.apps, graph.dests)
    readonly property var hot: G.neighbors(graph, focusId)
    readonly property real u: theme.fontScale
    readonly property bool empty: nodes.length === 0
    property real graphHeight: 0
    property string signature: ""

    function rebuild() {
        var width = Math.max(flick.width, 560 * u)
        var shown = G.filterCards(cards || [], query)
        // Samples arrive every few seconds with the same apps; keep the
        // nodes (and their loaded icons) when nothing visible changed.
        var next = width + "\n" + u + "\n" + JSON.stringify(shown)
        if (next === signature)
            return
        signature = next
        var built = G.build(shown)
        graphHeight = G.layout(built, width, {
            gap: 96 * u, minGap: 86 * u, pad: 56 * u, left: 110 * u, right: 120 * u
        })
        graph = built
        if (selectedId !== "" && G.find(built, selectedId) === null)
            selectedId = ""
        if (hoveredId !== "" && G.find(built, hoveredId) === null)
            hoveredId = ""
        canvas.requestPaint()
    }

    // Moves the selection down (1) or up (-1) its column, or starts at the
    // first app.
    function step(delta) {
        var node = G.find(graph, selectedId)
        if (node === null) {
            if (graph.apps.length)
                select(graph.apps[0].id)
            return
        }
        var column = G.columns(graph).filter(function (c) { return c.indexOf(node) >= 0 })[0]
        var next = column[Math.max(0, Math.min(column.length - 1, column.indexOf(node) + delta))]
        select(next.id)
    }

    // Moves to the nearest node in the column left (-1) or right (1).
    function side(delta) {
        var node = G.find(graph, selectedId)
        if (node === null) {
            step(1)
            return
        }
        var cols = G.columns(graph)
        var index = cols.findIndex(function (c) { return c.indexOf(node) >= 0 })
        for (var i = index + delta; i >= 0 && i < cols.length; i += delta) {
            if (!cols[i].length)
                continue
            var best = cols[i][0]
            cols[i].forEach(function (other) {
                if (Math.abs(other.y - node.y) < Math.abs(best.y - node.y))
                    best = other
            })
            select(best.id)
            return
        }
    }

    function select(id) {
        selectedId = id
        var node = G.find(graph, id)
        if (node !== null)
            flick.contentY = Math.max(0, Math.min(node.y - flick.height / 2, flick.contentHeight - flick.height))
    }

    onCardsChanged: rebuild()
    onQueryChanged: rebuild()
    onWidthChanged: rebuild()
    onUChanged: rebuild()
    onHotChanged: canvas.requestPaint()

    Flickable {
        id: flick
        anchors.fill: parent
        contentWidth: width
        contentHeight: Math.max(view.graphHeight, height)
        clip: true
        boundsBehavior: Flickable.StopAtBounds
        ScrollBar.vertical: ScrollBar { }

        MouseArea {
            width: flick.contentWidth
            height: flick.contentHeight
            onClicked: view.selectedId = ""
        }

        Canvas {
            id: canvas
            width: flick.contentWidth
            height: flick.contentHeight
            renderStrategy: Canvas.Cooperative

            function rim(from, to, radius) {
                var dx = to.x - from.x, dy = to.y - from.y
                var length = Math.hypot(dx, dy) || 1
                return { x: from.x + dx / length * radius, y: from.y + dy / length * radius }
            }

            onPaint: {
                var ctx = getContext("2d")
                ctx.reset()
                var u = view.u
                var radius = { information: 28 * u, app: 34 * u, destination: 25 * u }
                var byId = {}
                view.nodes.forEach(function (node) { byId[node.id] = node })
                var focused = view.focusId !== ""
                view.graph.edges.forEach(function (edge) {
                    var source = byId[edge.source], target = byId[edge.target]
                    if (!source || !target)
                        return
                    var from = rim(source, target, radius[source.type])
                    var to = rim(target, source, radius[target.type])
                    var bend = (from.x + to.x) / 2
                    var lit = view.hot[edge.id] === true
                    ctx.strokeStyle = edge.relation === "READ" ? theme.information
                                    : edge.relation === "LOOKUP" ? theme.lookup : theme.destination
                    ctx.globalAlpha = focused ? (lit ? 0.95 : 0.08) : 0.55
                    ctx.lineWidth = (lit ? 2.4 : 1.6) * u
                    if (edge.relation === "LOOKUP")
                        ctx.setLineDash([4, 4])
                    else
                        ctx.setLineDash([])
                    ctx.beginPath()
                    ctx.moveTo(from.x, from.y)
                    ctx.bezierCurveTo(bend, from.y, bend, to.y, to.x, to.y)
                    ctx.stroke()
                })
            }
        }

        Repeater {
            model: view.nodes
            delegate: GraphNode {
                required property var modelData
                node: modelData
                lit: view.hot[modelData.id] === true
                dim: view.focusId !== "" && !lit
                selected: view.selectedId === modelData.id
                onHovered: (inside) => {
                    if (inside)
                        view.hoveredId = modelData.id
                    else if (view.hoveredId === modelData.id)
                        view.hoveredId = ""
                }
                onClicked: view.selectedId = modelData.id
            }
        }
    }

    Label {
        anchors.centerIn: parent
        width: Math.min(parent.width - 48, 520 * view.u)
        visible: view.empty
        horizontalAlignment: Text.AlignHCenter
        wrapMode: Text.Wrap
        color: theme.muted
        font.pixelSize: Math.round(15 * view.u)
        text: view.query !== "" && (view.cards || []).length
              ? qsTr("No app matches “%1”.").arg(view.query)
              : monitor.status === "live"
                ? qsTr("No app has an open connection or a recent read in this sample.")
                : qsTr("Plug the phone in and accept USB debugging. Apps show up here on their own.")
    }
}
