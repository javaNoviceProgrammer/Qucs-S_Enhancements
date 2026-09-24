/***************************************************************************
                                autosave.cpp
                               --------------
 ***************************************************************************/

/***************************************************************************
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 ***************************************************************************/

#include "autosave.h"

#include <QCryptographicHash>
#include <QDebug>
#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QStandardPaths>

#include "qucsdoc.h"
#include "schematic.h"
#include "textdoc.h"

namespace qucs_s::autosave {

namespace {

QString g_directoryOverride;

// Stable file name stem for a document: hash of its identity, so that
// arbitrary paths (with separators, unicode, ...) become safe file names.
QString keyFor(const QString& identity)
{
    return QString::fromLatin1(
        QCryptographicHash::hash(identity.toUtf8(), QCryptographicHash::Sha1).toHex().left(20));
}

QString identityFor(const QucsDoc* doc, int untitledId)
{
    if (!doc->getDocName().isEmpty())
        return QDir::cleanPath(doc->getDocName());
    const bool schematic = dynamic_cast<const Schematic*>(doc) != nullptr;
    return QStringLiteral("untitled:%1:%2").arg(schematic ? "schematic" : "text").arg(untitledId);
}

QString suffixFor(const QucsDoc* doc)
{
    if (!doc->getDocName().isEmpty())
        return QFileInfo(doc->getDocName()).suffix();
    return dynamic_cast<const Schematic*>(doc) ? QStringLiteral("sch") : QStringLiteral("txt");
}

bool writeMeta(const QString& metaPath, const QString& original, bool untitled,
               bool schematic, const QString& copy)
{
    QJsonObject meta;
    meta["original"] = original;
    meta["untitled"] = untitled;
    meta["kind"] = schematic ? "schematic" : "text";
    meta["copy"] = copy;
    meta["when"] = QDateTime::currentDateTime().toString(Qt::ISODate);
    QSaveFile f(metaPath);
    if (!f.open(QIODevice::WriteOnly))
        return false;
    f.write(QJsonDocument(meta).toJson(QJsonDocument::Compact));
    return f.commit();
}

void removeKey(const QString& key)
{
    QDir dir(directory());
    for (const QFileInfo& fi : dir.entryInfoList({key + ".*"}, QDir::Files))
        QFile::remove(fi.absoluteFilePath());
}

} // namespace

QString directory()
{
    QString dir = g_directoryOverride;
    if (dir.isEmpty()) {
        dir = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
        if (dir.isEmpty())
            dir = QDir::homePath() + QStringLiteral("/.qucs-s");
        dir += QStringLiteral("/autosave");
    }
    QDir().mkpath(dir);
    return dir;
}

void setDirectory(const QString& dir)
{
    g_directoryOverride = dir;
}

QDateTime writtenAt(const QucsDoc* doc, int untitledId)
{
    if (doc == nullptr)
        return {};
    const QFileInfo copy(directory() + QStringLiteral("/") + keyFor(identityFor(doc, untitledId))
                         + QStringLiteral(".") + suffixFor(doc));
    return copy.exists() ? copy.lastModified() : QDateTime();
}

QString write(QucsDoc* doc, int untitledId)
{
    if (doc == nullptr)
        return {};
    const QString identity = identityFor(doc, untitledId);
    const QString key = keyFor(identity);
    const QString copy = directory() + QStringLiteral("/") + key + QStringLiteral(".") + suffixFor(doc);
    const QString meta = directory() + QStringLiteral("/") + key + QStringLiteral(".meta");

    // Write next to the final name and rename, so a crash mid-write cannot
    // leave a truncated copy behind that would later be offered as valid.
    const QString tmp = copy + QStringLiteral(".part");
    if (!doc->writeTo(tmp)) {
        QFile::remove(tmp);
        return {};
    }
    QFile::remove(copy);
    if (!QFile::rename(tmp, copy)) {
        QFile::remove(tmp);
        return {};
    }
    const bool untitled = doc->getDocName().isEmpty();
    const bool schematic = dynamic_cast<Schematic*>(doc) != nullptr;
    if (!writeMeta(meta, untitled ? QString() : QDir::cleanPath(doc->getDocName()),
                   untitled, schematic, copy)) {
        QFile::remove(copy);
        return {};
    }
    return copy;
}

void remove(const QString& originalPath)
{
    if (originalPath.isEmpty())
        return;
    removeKey(keyFor(QDir::cleanPath(originalPath)));
}

void removeUntitled(int untitledId, bool schematic)
{
    removeKey(keyFor(QStringLiteral("untitled:%1:%2").arg(schematic ? "schematic" : "text").arg(untitledId)));
}

void clear()
{
    QDir dir(directory());
    for (const QFileInfo& fi : dir.entryInfoList(QDir::Files))
        QFile::remove(fi.absoluteFilePath());
}

QList<Entry> pending()
{
    QList<Entry> result;
    QDir dir(directory());
    for (const QFileInfo& fi : dir.entryInfoList({QStringLiteral("*.meta")}, QDir::Files, QDir::Time | QDir::Reversed)) {
        QFile f(fi.absoluteFilePath());
        if (!f.open(QIODevice::ReadOnly))
            continue;
        const QJsonObject meta = QJsonDocument::fromJson(f.readAll()).object();
        Entry e;
        e.key = fi.completeBaseName();
        e.path = meta.value("copy").toString();
        e.original = meta.value("original").toString();
        e.untitled = meta.value("untitled").toBool();
        e.schematic = meta.value("kind").toString() != QStringLiteral("text");
        e.when = QDateTime::fromString(meta.value("when").toString(), Qt::ISODate);
        if (e.path.isEmpty() || !QFileInfo::exists(e.path)) {
            // orphaned sidecar
            QFile::remove(fi.absoluteFilePath());
            continue;
        }
        result.append(e);
    }
    return result;
}

} // namespace qucs_s::autosave
