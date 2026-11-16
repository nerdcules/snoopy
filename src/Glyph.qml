import QtQuick

// A line symbol for each kind of information, and a globe for destinations,
// drawn from the same 24 × 24 paths android-diagnoser's dashboard used.
Item {
    id: glyph

    property string name: "Destination"
    property color color: theme.text
    property real size: 24

    readonly property var paths: ({
        "Location": ["M12 21s-6.5-5.8-6.5-11a6.5 6.5 0 0 1 13 0c0 5.2-6.5 11-6.5 11z", "M12 11.2a2.1 2.1 0 1 0 0-4.2 2.1 2.1 0 0 0 0 4.2z"],
        "Camera": ["M4 8.5h3.2L9 6h6l1.8 2.5H20a1.5 1.5 0 0 1 1.5 1.5v8A1.5 1.5 0 0 1 20 19H4a1.5 1.5 0 0 1-1.5-1.5v-8A1.5 1.5 0 0 1 4 8.5z", "M12 16.2a3 3 0 1 0 0-6 3 3 0 0 0 0 6z"],
        "Microphone": ["M12 3.5a3 3 0 0 1 3 3v5a3 3 0 0 1-6 0v-5a3 3 0 0 1 3-3z", "M8 12.5a4 4 0 0 0 8 0", "M12 16.5v3", "M9 19.5h6"],
        "Contacts": ["M12 12.2a3 3 0 1 0 0-6 3 3 0 0 0 0 6z", "M6 19.2c.9-2.8 3-4.2 6-4.2s5.1 1.4 6 4.2"],
        "Photos & files": ["M4.5 6.5h15v12h-15z", "M4.5 15.5l3.6-3.6 2.8 2.8 2.2-2.2 4.2 4.2"],
        "Clipboard": ["M9 4.5h6v2.2H9z", "M8.5 5.2h-1A1.5 1.5 0 0 0 6 6.7v12.2h12V6.7a1.5 1.5 0 0 0-1.5-1.5h-1"],
        "Identity": ["M4 7h16v11H4z", "M7.5 12.2h5", "M7.5 15.2h3.5", "M16.2 13.2a1.6 1.6 0 1 0 0-3.2 1.6 1.6 0 0 0 0 3.2z"],
        "Messages": ["M5 6.5h14v9.2H8.2L5 18.5z"],
        "Motion": ["M3.5 12h2.2l2-5 3.2 10 2.2-5H20.5"],
        "Destination": ["M12 4.5a7.5 7.5 0 1 0 0 15 7.5 7.5 0 0 0 0-15z", "M4.8 12h14.4", "M12 4.6c2 2.2 3 4.7 3 7.4s-1 5.2-3 7.4c-2-2.2-3-4.7-3-7.4s1-5.2 3-7.4z"]
    })

    width: size
    height: size

    // A Canvas rather than Shape: it paints like any other item, so it is
    // clipped and faded with the node in every scene graph backend (Shape
    // escaped the graph's clip in the offscreen renderer).
    Canvas {
        id: canvas
        anchors.fill: parent
        onPaint: {
            var ctx = getContext("2d")
            ctx.reset()
            ctx.scale(width / 24, height / 24)
            ctx.strokeStyle = glyph.color
            ctx.lineWidth = 1.7
            ctx.lineCap = "round"
            ctx.lineJoin = "round"
            var list = glyph.paths[glyph.name] || glyph.paths["Destination"]
            for (var i = 0; i < list.length; ++i) {
                ctx.path = list[i]
                ctx.stroke()
            }
        }
    }

    onNameChanged: canvas.requestPaint()
    onColorChanged: canvas.requestPaint()
    onSizeChanged: canvas.requestPaint()
}
