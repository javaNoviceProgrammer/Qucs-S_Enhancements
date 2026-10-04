/*
 * projectlibraries.cpp - the Verilog-A of the library devices a project's
 * schematics use, linked into the project
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#include "projectlibraries.h"

#include "misc.h"
#include "osdiselection.h"
#include "schematic.h"
#include "components/libcomp.h"
#include "components/subcircuit.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMap>
#include <QSet>

#include <algorithm>
#include <tuple>

namespace qucs_s::projectlibraries {

Mode defaultMode()
{
#ifdef Q_OS_WIN
    return Mode::Copy;   // (a symbolic link there needs Developer Mode or an administrator)
#else
    return Mode::Link;
#endif
}

namespace {

QString real(const QString& path)
{
    const QFileInfo info(path);
    const QString canonical = info.canonicalFilePath();
    return canonical.isEmpty() ? QDir::cleanPath(info.absoluteFilePath()) : canonical;
}

bool inside(const QString& path, const QString& folder)
{
    return path == folder || path.startsWith(folder + QLatin1Char('/'));
}

// ---- what the schematics use -------------------------------------------

struct Walk {
    QString project;   // real path
    QSet<QString> visited;
    QList<Use>* uses;
    QStringList* unresolved;
    QHash<QString, QStringList> attached;   // a library's component's files, each read once
    QHash<QString, QString> found;          // a name in a folder, where it is (each looked for once)
};

// A part of the library \a libraryFile (its Lib \a lib), component \a comp:
// its Verilog-A sources, as the part finds them (LibComp).
void addLibraryPart(const QString& libraryFile, const QString& lib, const QString& comp, Walk& w)
{
    const QFileInfo info(libraryFile);
    if (!info.isFile()) {
        const QString name = QFileInfo(lib).fileName();
        if (!name.isEmpty() && !w.unresolved->contains(name)) *w.unresolved << name;
        return;
    }
    const QString folder = real(info.absoluteDir().absoluteFilePath(info.completeBaseName()));
    if (inside(folder, w.project)) return;   // the project's own library: its files are the project's
    const QString key = info.absoluteFilePath() + QLatin1Char('\n') + comp;
    auto it = w.attached.constFind(key);
    if (it == w.attached.constEnd()) it = w.attached.insert(key, LibComp::verilogAFilesOf(info.absoluteFilePath(), comp));
    for (const QString& file : *it) {
        if (!file.endsWith(QLatin1String(".va"), Qt::CaseInsensitive) || !QFileInfo(file).isFile()) continue;
        const Use use{info.completeBaseName(), folder, real(file)};
        const bool known = std::any_of(w.uses->cbegin(), w.uses->cend(), [&](const Use& u) {
            return u.folder == use.folder && u.source == use.source;
        });
        if (!known) w.uses->append(use);
    }
}

// The quoted values of a component's line, in order: "VaLib" and "vres" of
// <Lib X1 1 340 200 -20 14 0 0 "VaLib" 1 "vres" 1>.
QStringList quotedValues(const QString& line)
{
    QStringList values;
    for (qsizetype at = 0;;) {
        const qsizetype open = line.indexOf(QLatin1Char('"'), at);
        if (open < 0) break;
        const qsizetype close = line.indexOf(QLatin1Char('"'), open + 1);
        if (close < 0) break;
        values << line.mid(open + 1, close - open - 1);
        at = close + 1;
    }
    return values;
}

// A schematic file read as text, not loaded (that took a quarter second
// each): its library parts and the subcircuits outside the project it places.
void walkFile(const QString& file, Walk& w)
{
    if (w.visited.contains(file)) return;   // (as given: one real path for each line was a second of a large schematic)
    w.visited.insert(file);
    const QString key = real(file);
    if (key != file) {
        if (w.visited.contains(key)) return;
        w.visited.insert(key);
    }
    QFile f(file);
    if (!f.open(QIODevice::ReadOnly)) return;
    const QString text = QString::fromUtf8(f.readAll());
    const QString folder = QFileInfo(file).absolutePath();
    // (A schematic of thousands of one subcircuit: its file looked for once.)
    const auto where = [&](QChar kind, const QString& name) {
        const QString key = kind + folder + QLatin1Char('\n') + name;
        auto it = w.found.constFind(key);
        if (it == w.found.constEnd()) {
            QString path = kind == QLatin1Char('L') ? LibComp::libraryFileOf(name, folder) : Subcircuit::subcircuitFileOf(name, folder);
            // A subcircuit of the project's, or not there: none to follow.
            if (kind == QLatin1Char('S') && (!QFileInfo(path).isFile() || inside(real(path), w.project))) path.clear();
            it = w.found.insert(key, path);
        }
        return *it;
    };
    bool components = false;
    for (const QString& raw : text.split(QLatin1Char('\n'))) {
        const QString line = raw.trimmed();
        if (line == QLatin1String("<Components>")) components = true;
        else if (line == QLatin1String("</Components>")) components = false;
        else if (!components) continue;
        else if (line.startsWith(QLatin1String("<Lib "))) {
            const QStringList values = quotedValues(line);
            if (values.size() >= 2) addLibraryPart(where(QLatin1Char('L'), values.at(0)), values.at(0), values.at(1), w);
        } else if (line.startsWith(QLatin1String("<Sub "))) {
            // One of the project's is looked at on its own.
            const QStringList values = quotedValues(line);
            const QString sub = values.isEmpty() ? QString() : where(QLatin1Char('S'), values.at(0));
            if (!sub.isEmpty()) walkFile(sub, w);
        }
    }
}

// A schematic in memory (unsaved changes): its parts as they are.
void walk(Schematic* sch, Walk& w)
{
    for (Component* c : sch->a_DocComps) {
        if (auto* part = dynamic_cast<LibComp*>(c)) {
            if (part->Props.size() >= 2) addLibraryPart(part->libraryFile(), part->Props.at(0)->Value, part->Props.at(1)->Value, w);
        } else if (c->Model == QLatin1String("Sub")) {
            const QString file = static_cast<Subcircuit*>(c)->getSubcircuitFile();
            if (!file.isEmpty() && QFileInfo(file).isFile() && !inside(real(file), w.project)) walkFile(file, w);
        }
    }
}

// ---- the record of a library's folder ------------------------------------

struct Item {
    QString path;       // relative to the folder
    QString original;   // the library's file
    QString kind;       // link, copy, or include (a copy of a file a copied source includes)
};

struct Record {
    QString library;
    QString folder;     // the library's folder
    bool created = false;
    QList<Item> items;
};

bool readRecord(const QString& file, Record* out)
{
    QFile f(file);
    if (!f.open(QIODevice::ReadOnly)) return false;
    const QJsonObject o = QJsonDocument::fromJson(f.readAll()).object();
    Record r;
    r.library = o.value(QLatin1String("library")).toString();
    r.folder = o.value(QLatin1String("folder")).toString();
    r.created = o.value(QLatin1String("created")).toBool();
    for (const QJsonValue& v : o.value(QLatin1String("files")).toArray()) {
        const QJsonObject i = v.toObject();
        const Item item{i.value(QLatin1String("path")).toString(), i.value(QLatin1String("original")).toString(),
                        i.value(QLatin1String("kind")).toString()};
        // (Inside the folder only: a record edited by hand takes nothing elsewhere away.)
        if (!item.path.isEmpty() && !item.path.startsWith(QLatin1String("..")) && !QDir::isAbsolutePath(item.path))
            r.items << item;
    }
    *out = r;
    return !r.library.isEmpty();
}

void writeRecord(const QString& file, const Record& r)
{
    QJsonArray files;
    for (const Item& item : r.items)
        files.append(QJsonObject{{QStringLiteral("path"), item.path}, {QStringLiteral("original"), item.original},
                                 {QStringLiteral("kind"), item.kind}});
    const QJsonObject o{{QStringLiteral("library"), r.library}, {QStringLiteral("folder"), r.folder},
                        {QStringLiteral("created"), r.created}, {QStringLiteral("files"), files},
                        {QStringLiteral("about"), QStringLiteral("Kept by Qucs-S: the Verilog-A of this library's devices "
                                                                 "the project's schematics use. Edits are lost.")}};
    QFile f(file);
    if (f.open(QIODevice::WriteOnly)) f.write(QJsonDocument(o).toJson());
}

// ---- the files ------------------------------------------------------------

// Takes away what Qucs-S put there, if it is still that: a link, or a
// file (not a link) for a copy.
bool removeItem(const QString& path, const QString& kind)
{
    const QFileInfo info(path);
    if (kind == QLatin1String("link") ? !info.isSymLink() : (info.isSymLink() || !info.isFile())) return false;
    return QFile::remove(path);
}

// The model OpenVAF compiled beside a source.
void removeModel(const QString& source)
{
    const QFileInfo va(source);
    const QString model = va.absoluteDir().absoluteFilePath(va.completeBaseName() + QStringLiteral(".osdi"));
    const QFileInfo info(model);
    if (info.isFile() && !info.isSymLink()) QFile::remove(model);
}

// The folders left empty from \a dir up to \a top, \a top not included.
void removeEmptyUpTo(QString dir, const QString& top)
{
    dir = QDir::cleanPath(dir);
    while (dir != top && inside(dir, top)) {
        if (!QDir().rmdir(dir)) break;
        dir = QFileInfo(dir).absolutePath();
    }
}

// A copy of \a from at \a to, made or renewed (the original changed);
// false when a file of the user's is in the way.
bool copyFresh(const QString& from, const QString& to, bool ours, bool* made)
{
    const QFileInfo there(to);
    if (there.exists() || there.isSymLink()) {
        if (!ours) return false;
        const QFileInfo original(from);
        if (!there.isSymLink() && there.size() == original.size() && there.lastModified() >= original.lastModified())
            return true;
        QFile::remove(to);
    }
    QDir().mkpath(there.absolutePath());
    if (!QFile::copy(from, to)) return false;
    *made = true;
    return true;
}

} // namespace

void usedSources(const QString& projectDir, const QList<Schematic*>& open, QList<Use>* uses, QStringList* unresolved)
{
    Walk w{real(projectDir), {}, uses, unresolved, {}};
    const QDir project(projectDir);
    const QString scratch = QString::fromLatin1(misc::ScratchFolder) + QLatin1Char('/');
    for (const QString& file : misc::projectFiles(project, {QStringLiteral("*.sch")}))
        if (!file.startsWith(scratch)) walkFile(project.absoluteFilePath(file), w);
    for (Schematic* sch : open)
        if (sch != nullptr) walk(sch, w);
}

Report sync(const QString& projectDir, const QList<Schematic*>& open, Mode mode)
{
    Report report;
    const QDir project(projectDir);
    if (projectDir.isEmpty() || !project.exists()) return report;
    QList<Use> uses;
    QStringList unresolved;
    usedSources(projectDir, open, &uses, &unresolved);
    std::sort(uses.begin(), uses.end(), [](const Use& a, const Use& b) {
        return std::tie(a.library, a.folder, a.source) < std::tie(b.library, b.folder, b.source);
    });

    // The library folders there are, by their records.
    QMap<QString, Record> found;   // by the folder's name
    for (const QFileInfo& dir : project.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot | QDir::NoSymLinks)) {
        Record r;
        if (readRecord(QDir(dir.absoluteFilePath()).absoluteFilePath(QLatin1String(RecordName)), &r)) found.insert(dir.fileName(), r);
    }

    // A folder for each library used: the one its record has; else one of
    // a library of its name no schematic uses here (the library moved, or
    // another computer has it elsewhere); else its name - or, taken, _2, _3...
    QStringList folders;           // the libraries' folders, in order
    QHash<QString, QString> nameOf;
    for (const Use& u : std::as_const(uses))
        if (!folders.contains(u.folder)) {
            folders << u.folder;
            nameOf.insert(u.folder, u.library);
        }
    QHash<QString, QString> placed;   // the library's folder -> the folder's name in the project
    QSet<QString> taken;
    for (const QString& folder : std::as_const(folders))
        for (auto it = found.cbegin(); it != found.cend(); ++it)
            if (it->folder == folder && !taken.contains(it.key())) {
                placed.insert(folder, it.key());
                taken.insert(it.key());
                break;
            }
    for (const QString& folder : std::as_const(folders)) {
        if (placed.contains(folder)) continue;
        for (auto it = found.cbegin(); it != found.cend(); ++it)
            if (!taken.contains(it.key()) && it->library == nameOf.value(folder) && !folders.contains(it->folder)) {
                placed.insert(folder, it.key());
                taken.insert(it.key());
                break;
            }
    }
    for (const QString& folder : std::as_const(folders)) {
        if (placed.contains(folder)) continue;
        const QString base = nameOf.value(folder);
        for (int n = 1;; ++n) {
            const QString name = n == 1 ? base : QStringLiteral("%1_%2").arg(base).arg(n);
            // Not another library's, nor the project's own library of that name (NAME.lib and its folder).
            if (taken.contains(name) || found.contains(name) || QFileInfo::exists(project.filePath(name + QStringLiteral(".lib"))))
                continue;
            placed.insert(folder, name);
            taken.insert(name);
            break;
        }
    }

    // The folders of libraries no schematic uses now: emptied of what Qucs-S
    // put there - unless a part names it and it is not found here.
    const auto takeAway = [&](const QString& name, const Item& item) {
        const QString dir = project.absoluteFilePath(name);
        const QString path = QDir(dir).absoluteFilePath(item.path);
        if (removeItem(path, item.kind)) {
            if (item.kind != QLatin1String("include")) {
                removeModel(path);
                report.removed << name + QLatin1Char('/') + item.path;
            }
        }
        removeEmptyUpTo(QFileInfo(path).absolutePath(), QDir::cleanPath(dir));
    };
    const auto close = [&](const QString& name, const Record& r) {
        const QString dir = project.absoluteFilePath(name);
        QFile::remove(QDir(dir).absoluteFilePath(QLatin1String(RecordName)));
        if (r.created) QDir().rmdir(dir);   // (only when empty)
    };
    for (auto it = found.cbegin(); it != found.cend(); ++it) {
        if (taken.contains(it.key())) continue;
        if (unresolved.contains(it->library)) continue;   // not found here: kept as it is
        for (const Item& item : it->items) takeAway(it.key(), item);
        close(it.key(), *it);
    }

    // Each library used: its sources in its folder, and nothing else of Qucs-S's.
    for (const QString& folder : std::as_const(folders)) {
        const QString name = placed.value(folder);
        const QString dir = project.absoluteFilePath(name);
        const bool known = found.contains(name);
        Record r = found.value(name);
        if (!known) r.created = !QFileInfo::exists(dir);
        r.library = nameOf.value(folder);
        r.folder = folder;
        QHash<QString, Item> had;
        for (const Item& item : std::as_const(r.items)) had.insert(item.path, item);
        QList<Item> items;
        const auto keep = [&](const Item& item) {
            if (std::none_of(items.cbegin(), items.cend(), [&](const Item& i) { return i.path == item.path; })) items << item;
        };
        for (const Use& u : std::as_const(uses)) {
            if (u.folder != folder) continue;
            QString relative = QDir(folder).relativeFilePath(u.source);
            if (relative.startsWith(QLatin1String(".."))) relative = QFileInfo(u.source).fileName();
            const QString path = QDir(dir).absoluteFilePath(relative);
            const bool ours = had.contains(relative);
            const QFileInfo there(path);
            QDir().mkpath(there.absolutePath());
            if (mode == Mode::Link) {
                if (there.isSymLink() && real(there.symLinkTarget()) == u.source) {
                    // as it should be
                } else if ((there.exists() || there.isSymLink()) && !ours) {
                    report.conflicts << name + QLatin1Char('/') + relative;
                    continue;
                } else {
                    // New - or it led elsewhere (the library moved, another
                    // computer), or a copy of before: made again, its model
                    // with it (of another file).
                    if (there.exists() || there.isSymLink()) {
                        QFile::remove(path);
                        removeModel(path);
                    }
                    if (!QFile::link(u.source, path)) {
                        report.conflicts << name + QLatin1Char('/') + relative;
                        continue;
                    }
                    report.made << name + QLatin1Char('/') + relative;
                }
                keep(Item{relative, u.source, QStringLiteral("link")});
            } else {
                bool made = false;
                if (!copyFresh(u.source, path, ours, &made)) {
                    report.conflicts << name + QLatin1Char('/') + relative;
                    continue;
                }
                if (made) report.made << name + QLatin1Char('/') + relative;
                keep(Item{relative, u.source, QStringLiteral("copy")});
                // The files it includes, at their places beside it, so that
                // its `include lines find them (those of the library's folder).
                for (const QString& included : osdi::sourceIncludes(u.source)) {
                    const QString original = real(included);
                    const QString at = QDir(folder).relativeFilePath(original);
                    if (at.startsWith(QLatin1String(".."))) continue;
                    bool copied = false;
                    if (copyFresh(original, QDir(dir).absoluteFilePath(at), had.contains(at), &copied))
                        keep(Item{at, original, QStringLiteral("include")});
                }
            }
        }
        for (const Item& item : std::as_const(r.items))
            if (std::none_of(items.cbegin(), items.cend(), [&](const Item& i) { return i.path == item.path; }))
                takeAway(name, item);
        r.items = items;
        if (items.isEmpty()) {
            close(name, r);
        } else {
            writeRecord(QDir(dir).absoluteFilePath(QLatin1String(RecordName)), r);
        }
    }
    return report;
}

Entry entryOf(const QString& path)
{
    if (path.isEmpty()) return {};
    const QFileInfo info(path);
    QDir dir = info.absoluteDir();
    for (int up = 0; up < 8; ++up) {
        const QString record = dir.absoluteFilePath(QLatin1String(RecordName));
        if (QFileInfo::exists(record)) {
            Record r;
            if (!readRecord(record, &r)) return {};
            const QString relative = dir.relativeFilePath(info.absoluteFilePath());
            for (const Item& item : std::as_const(r.items)) {
                if (item.path != relative) continue;
                // Still Qucs-S's: a link, or a copy that is no link.
                if ((item.kind == QLatin1String("link")) != info.isSymLink()) return {};
                return {r.library, item.original};
            }
            return {};
        }
        if (!dir.cdUp()) break;
    }
    return {};
}

} // namespace qucs_s::projectlibraries
