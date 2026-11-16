// Names for the kinds of information and connection the report gives in
// English, in the window's language. Bindings pass translator.language so
// they re-run when it changes.

function information(label, language) {
    switch (label) {
    case "Location": return qsTr("Location")
    case "Camera": return qsTr("Camera")
    case "Microphone": return qsTr("Microphone")
    case "Contacts": return qsTr("Contacts")
    case "Photos & files": return qsTr("Photos & files")
    case "Clipboard": return qsTr("Clipboard")
    case "Identity": return qsTr("Identity")
    case "Messages": return qsTr("Messages")
    case "Motion": return qsTr("Motion")
    }
    return label
}

function connection(kind, language) {
    if (kind === "Encrypted")
        return qsTr("Encrypted")
    if (kind === "DNS")
        return qsTr("DNS lookup")
    return kind
}

// 1.5 MB, in the window's number format.
function bytes(size, language) {
    var value = size || 0
    var units = ["B", "KB", "MB", "GB"]
    for (var i = 0; i < units.length; ++i) {
        if (value < 1024 || i === units.length - 1)
            return i === 0 ? qsTr("%1 B").arg(value.toLocaleString(Qt.locale(), 'f', 0))
                           : qsTr("%1 %2").arg(value.toLocaleString(Qt.locale(), 'f', 1)).arg(units[i])
        value /= 1024
    }
    return String(size)
}
