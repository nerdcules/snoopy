#pragma once

#include <QHash>
#include <QJsonObject>
#include <QString>
#include <QStringList>

class Adb;

// One sample of the phone: the raw text of each dump, keyed by the names
// below (which are also the file names of a saved capture, name + ".txt"),
// and meta (serial, model, android, sdk, captured_at, errors).
//
// A saved capture from the Python android-diagnoser loads unchanged: the
// names, the files and meta.json are the same.
struct Capture {
    // In the order they are collected.
    static const QStringList fields;

    QHash<QString, QString> texts;
    QJsonObject meta;

    QString text(const QString &field) const { return texts.value(field); }
};

// Every dump, then the phone's model and Android version. Throws AdbError
// when there is no phone to read. A dump that fails is recorded in
// meta["errors"][name] and left empty; the rest still count.
Capture collectSnapshot(Adb &adb);

// The lighter set `snoopy watch` repeats: sockets, focus, app ops and the
// package list.
Capture collectWatch(Adb &adb);

// Writes every field as <name>.txt and meta.json into directory, creating it.
bool saveCapture(const Capture &capture, const QString &directory, QString *error);

// Reads a directory written by saveCapture (or by android-diagnoser
// scan --out). Missing files are empty. False when the directory is missing.
bool loadCapture(const QString &directory, Capture *capture, QString *error);

// UTC now as 2026-11-16T05:18:48Z, the form the report carries.
QString utcNow();
