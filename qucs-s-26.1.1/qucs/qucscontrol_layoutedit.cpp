/*
 * qucscontrol_layoutedit.cpp - Claude's edit_layout: a GDSII or OASIS
 *                              layout changed in place (layoutedit.h),
 *                              each version before an edit kept so that
 *                              'undo' puts it back, the open tab read
 *                              again and what changed told
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#include "qucscontrol.h"
#include "qucscontrol_p.h"

#include "layoutdoc.h"
#include "layoutedit.h"
#include "main.h"
#include "qucs.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>

#include <memory>

using namespace qucs_s::control;

namespace {

/// How many versions of a layout before its edits are kept, for undo.
constexpr int MostKept = 20;

QByteArray fileHash(const QString& path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return {};
    QCryptographicHash h(QCryptographicHash::Sha256);
    h.addData(&f);
    return h.result().toHex();
}

/// Where a layout's versions before its edits are kept: a folder of the
/// application's temporary files, by the layout's path.
QString keptFolder(const QString& file)
{
    const QByteArray key = QCryptographicHash::hash(QFileInfo(file).absoluteFilePath().toUtf8(), QCryptographicHash::Sha1).toHex().left(16);
    return QDir(QucsSettings.tempFilesDir.absolutePath()).filePath(QStringLiteral("layout-edits/") + QString::fromLatin1(key));
}

/// The versions kept, the latest last: each its copy's file and the hash of
/// the layout as the edit after it left it.
QJsonArray keptOf(const QString& folder)
{
    QFile f(QDir(folder).filePath(QStringLiteral("kept.json")));
    if (!f.open(QIODevice::ReadOnly)) return {};
    return QJsonDocument::fromJson(f.readAll()).array();
}

bool keep(const QString& folder, const QJsonArray& kept)
{
    QFile f(QDir(folder).filePath(QStringLiteral("kept.json")));
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;
    return f.write(QJsonDocument(kept).toJson(QJsonDocument::Compact)) > 0;
}

/// \a from copied over \a to whole: beside it first, then renamed.
bool copyOver(const QString& from, const QString& to)
{
    const QFileInfo target(to);
    const QString temporary = target.absolutePath() + QStringLiteral("/.") + target.fileName() + QStringLiteral(".undoing");
    QFile::remove(temporary);
    if (!QFile::copy(from, temporary)) return false;
    if (QFileInfo::exists(to) && !QFile::remove(to)) {
        QFile::remove(temporary);
        return false;
    }
    return QFile::rename(temporary, to);
}

} // namespace

QJsonObject QucsControl::editLayout(const QJsonObject& args)
{
    using namespace qucs_s::layout;
    // The layout: 'path' (its file, or an open tab's name), else the one in
    // front.
    QString file = args.value(QLatin1String("path")).toString().trimmed();
    LayoutDoc* tab = nullptr;
    if (file.isEmpty()) {
        tab = qobject_cast<LayoutDoc*>(a_app->DocumentTab->count() > 0 ? a_app->DocumentTab->currentWidget() : nullptr);
        if (tab == nullptr || tab->getDocName().isEmpty()) return errorResult(tr("Which layout? 'path' (the document in front is none)."));
        file = tab->getDocName();
    } else if (!QFileInfo(absolute(file)).isFile()) {
        QString unused;
        if (QucsDoc* named = document(QJsonObject{{QStringLiteral("path"), file}}, &unused); named != nullptr && dynamic_cast<LayoutDoc*>(named) != nullptr)
            file = named->getDocName();
    }
    file = QFileInfo(absolute(file)).absoluteFilePath();
    if (!QFileInfo(file).isFile()) return errorResult(tr("There is no file %1.").arg(QDir::toNativeSeparators(file)));
    for (QucsDoc* d : a_app->allDocuments())
        if (auto* l = dynamic_cast<LayoutDoc*>(d); l != nullptr && sameFile(l->getDocName(), file)) tab = l;
    if (tab != nullptr && tab->isLoading() && !tab->waitForLoaded(120000)) return errorResult(tr("%1 is still being read.").arg(QFileInfo(file).fileName()));
    const QString folder = keptFolder(file);
    QJsonArray kept = keptOf(folder);
    // As it was before: the tab's reading, else read now - to say what
    // changed.
    std::shared_ptr<const Layout> before = tab != nullptr ? tab->layout() : nullptr;
    QString readError;
    if (!before) before = read(file, &readError);
    const auto told = [&](QJsonObject o) {
        // Read again, and what changed.
        const std::shared_ptr<const Layout> after = read(file, &readError);
        if (!after) {
            o.insert(QStringLiteral("read back"), tr("could not be read: %1").arg(readError));
        } else if (before) {
            const QJsonObject changed = differences(*before, *after);
            o.insert(QStringLiteral("changed"), changed.isEmpty() ? QJsonValue(tr("nothing, as read back")) : QJsonValue(changed));
        }
        if (tab != nullptr) tab->reload();
        o.insert(QStringLiteral("undo"), kept.isEmpty() ? tr("no earlier version kept")
                                                        : tr("edit_layout's undo puts the file back as it was before the last edit (%1 kept)").arg(kept.size()));
        return jsonResult(o);
    };

    if (args.value(QLatin1String("undo")).toBool()) {
        if (args.contains(QLatin1String("edits")) || args.contains(QLatin1String("cell")))
            return errorResult(tr("'undo' puts the last edit back, alone: no 'edits' or 'cell' with it."));
        if (kept.isEmpty()) return errorResult(tr("No edit of %1 is kept to undo.").arg(QFileInfo(file).fileName()));
        const QJsonObject last = kept.last().toObject();
        const QString copy = QDir(folder).filePath(last.value(QStringLiteral("copy")).toString());
        if (fileHash(file) != last.value(QStringLiteral("after")).toString().toLatin1())
            return errorResult(tr("%1 has changed since the last edit_layout (by hand, the router or another program): undoing would lose that. "
                                  "The version before that edit is kept at %2.")
                                   .arg(QFileInfo(file).fileName(), QDir::toNativeSeparators(copy)));
        if (!QFileInfo::exists(copy) || !copyOver(copy, file)) return errorResult(tr("%1 could not be put back.").arg(QFileInfo(file).fileName()));
        QFile::remove(copy);
        kept.removeLast();
        keep(folder, kept);
        return told(QJsonObject{{QStringLiteral("file"), QDir::toNativeSeparators(file)},
                                {QStringLiteral("done"), tr("put back as it was before the last edit")}});
    }

    if (!args.value(QLatin1String("edits")).isArray()) return errorResult(tr("'edits' is a list of edits (or 'undo': true)."));
    // The cell: as named, else the one its tab shows, else the largest top
    // cell.
    QString cell = args.value(QLatin1String("cell")).toString().trimmed();
    if (cell.isEmpty() && tab != nullptr && tab->layout() && tab->view()->cell() >= 0)
        cell = tab->layout()->cells.at(tab->view()->cell()).name;
    // The version before, kept first.
    if (!QDir().mkpath(folder)) return errorResult(tr("The folder %1 for the versions kept cannot be made.").arg(QDir::toNativeSeparators(folder)));
    int next = 1;
    for (const QJsonValue& v : std::as_const(kept)) next = std::max(next, v.toObject().value(QStringLiteral("copy")).toString().section(QLatin1Char('.'), 0, 0).toInt() + 1);
    const QString name = QStringLiteral("%1.%2").arg(next).arg(QFileInfo(file).suffix());
    const QString copy = QDir(folder).filePath(name);
    QFile::remove(copy);
    if (!QFile::copy(file, copy)) return errorResult(tr("%1 could not be kept before it is changed.").arg(QFileInfo(file).fileName()));
    const EditResult r = qucs_s::layout::editLayout(file, cell, args.value(QLatin1String("edits")).toArray());
    if (!r.ok) {
        QFile::remove(copy);
        return errorResult(r.error);
    }
    kept.append(QJsonObject{{QStringLiteral("copy"), name}, {QStringLiteral("after"), QString::fromLatin1(fileHash(file))}});
    while (kept.size() > MostKept) {
        QFile::remove(QDir(folder).filePath(kept.first().toObject().value(QStringLiteral("copy")).toString()));
        kept.removeFirst();
    }
    keep(folder, kept);
    return told(QJsonObject{{QStringLiteral("file"), QDir::toNativeSeparators(file)}, {QStringLiteral("cell"), r.cell}, {QStringLiteral("done"), r.done}});
}
