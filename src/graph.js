.pragma library

// The window's graph, from the cards Monitor.graph holds: information kinds
// on the left, apps in the middle, destinations on the right, laid out as
// android-diagnoser's dashboard did. Pure functions, so tests can run them.

// The cards whose text mentions query anywhere (name, package, a read, a
// destination), case-insensitively. An empty query keeps them all.
function filterCards(cards, query) {
    var q = (query || "").trim().toLowerCase()
    if (!q)
        return cards
    return cards.filter(function (card) { return JSON.stringify(card).toLowerCase().indexOf(q) >= 0 })
}

// Long reverse names keep their first label and their last two.
function prettyName(name) {
    if (!name || name.length <= 42)
        return name
    var parts = name.split(".")
    if (parts.length <= 3)
        return name
    var tail = parts.slice(-2).join(".")
    var head = parts[0]
    if (head.length > 28)
        head = head.slice(0, 26) + "…"
    return head + "." + tail
}

// Nodes and edges. An information node is shared by every app that read
// it, and a destination by every app connected to the same endpoint the same
// way. Each node keeps the lines its inspector shows.
function build(cards) {
    var infos = {}, infoOrder = []
    var dests = {}, destOrder = []
    var apps = [], edges = []
    cards.forEach(function (card, index) {
        var app = {
            id: "app:" + (card.package || card.name), type: "app", label: card.name,
            card: card, index: index, lines: []
        }
        apps.push(app)
        ;(card.reads || []).forEach(function (read) {
            if (!infos[read.label]) {
                infos[read.label] = { id: "info:" + read.label, type: "information", label: read.label, lines: [] }
                infoOrder.push(read.label)
            }
            var info = infos[read.label]
            info.lines.push({ app: card.name, text: read.detail })
            edges.push({ id: info.id + ">" + app.id, source: info.id, target: app.id, relation: "READ", text: read.detail })
        })
        ;(card.destinations || []).forEach(function (dest) {
            var key = (dest.endpoint || dest.label) + "|" + dest.kind + "|" + dest.relation
            if (!dests[key]) {
                dests[key] = {
                    id: "dest:" + key, type: "destination",
                    label: dest.name ? prettyName(dest.name) : dest.label,
                    name: dest.name || "", host: dest.host || dest.label, kind: dest.kind,
                    relation: dest.relation, lines: []
                }
                destOrder.push(key)
            }
            var node = dests[key]
            node.lines.push({ app: card.name, text: dest.detail })
            edges.push({
                id: app.id + ">" + node.id + ":" + card.name, source: app.id, target: node.id,
                relation: dest.relation === "lookup" ? "LOOKUP" : "CONNECTS", text: dest.detail
            })
        })
    })
    return {
        apps: apps,
        infos: infoOrder.map(function (k) { return infos[k] }),
        dests: destOrder.map(function (k) { return dests[k] }),
        edges: edges
    }
}

// Places every node. Apps keep their order (by bytes) one gap apart; each
// information and destination node sits at the mean height of the apps it
// links to, then nodes in a column are pushed apart to at least minGap.
// Returns the height the graph needs.
function layout(graph, width, metrics) {
    var gap = metrics.gap, minGap = metrics.minGap, pad = metrics.pad
    // When the information or destination column holds more nodes than
    // there are apps, the apps spread over the same height, so edges run
    // across rather than steeply down.
    var tallest = Math.max(graph.infos.length, graph.dests.length)
    if (graph.apps.length > 1 && tallest > graph.apps.length)
        gap = Math.max(gap, (tallest - 1) * minGap / (graph.apps.length - 1))
    var byId = {}
    graph.apps.forEach(function (node, index) { node.y = index * gap; byId[node.id] = node })
    graph.infos.forEach(function (node) { byId[node.id] = node })
    graph.dests.forEach(function (node) { byId[node.id] = node })
    seed(graph.infos, graph.edges, byId, true)
    seed(graph.dests, graph.edges, byId, false)
    spread(graph.infos, minGap)
    spread(graph.dests, minGap)
    var all = graph.infos.concat(graph.apps, graph.dests)
    var bottom = 0
    all.forEach(function (node) { bottom = Math.max(bottom, node.y) })
    all.forEach(function (node) { node.y += pad })
    graph.infos.forEach(function (node) { node.x = metrics.left })
    graph.apps.forEach(function (node) { node.x = width / 2 })
    graph.dests.forEach(function (node) { node.x = width - metrics.right })
    return bottom + pad * 2
}

function seed(nodes, edges, byId, incoming) {
    nodes.forEach(function (node) {
        var ys = []
        edges.forEach(function (edge) {
            if ((incoming ? edge.source : edge.target) !== node.id)
                return
            var other = byId[incoming ? edge.target : edge.source]
            if (other)
                ys.push(other.y)
        })
        node.y = ys.length ? ys.reduce(function (a, b) { return a + b }, 0) / ys.length : 0
    })
    nodes.sort(function (a, b) { return a.y - b.y || a.label.localeCompare(b.label) })
}

function spread(nodes, minGap) {
    var last = -1e9
    nodes.forEach(function (node) {
        if (node.y < last + minGap)
            node.y = last + minGap
        last = node.y
    })
}

// The node, the nodes it links to, and the edges between them.
function neighbors(graph, id) {
    var ids = {}
    if (!id)
        return ids
    ids[id] = true
    graph.edges.forEach(function (edge) {
        if (edge.source === id || edge.target === id) {
            ids[edge.source] = true
            ids[edge.target] = true
            ids[edge.id] = true
        }
    })
    return ids
}

// Columns in order, for moving the selection with the keyboard.
function columns(graph) {
    return [graph.infos, graph.apps, graph.dests]
}

function find(graph, id) {
    var all = graph.infos.concat(graph.apps, graph.dests)
    for (var i = 0; i < all.length; ++i)
        if (all[i].id === id)
            return all[i]
    return null
}
