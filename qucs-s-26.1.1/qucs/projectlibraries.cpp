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

#include <QCryptographicHash>
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
#include <utility>

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

// \a relative as a path in the folder \a dir, when it is one there: not
// out of it by ".." or through a link to a folder elsewhere - the folders
// on its way that are there, as they really are (the system resolves the
// "..": a/../../x is ../x cleaned). Empty otherwise: what a record names -
// edited by hand, or come with a project from elsewhere - is never taken
// away or written outside it. (Always under \a dir: /x is its x.)
QString pathIn(const QString& dir, const QString& relative)
{
    const QString clean = QDir::cleanPath(QDir::fromNativeSeparators(relative));
    const QString top = QFileInfo(dir).canonicalFilePath();
    if (top.isEmpty()) return {};
    for (QString at = QFileInfo(top + QLatin1Char('/') + clean).absolutePath();; at = QFileInfo(at).absolutePath()) {
        if (const QFileInfo info(at); info.exists())
            return inside(info.canonicalFilePath(), top) ? QDir::cleanPath(dir + QLatin1Char('/') + clean) : QString();
        if (!inside(at, top)) return {};   // (the folder itself gone meanwhile)
    }
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
    // Not found here - or no library of its name has the part: not loaded,
    // its library is not known either.
    if (!info.isFile() || !LibComp::hasComponent(info.absoluteFilePath(), comp)) {
        const QString name = QFileInfo(lib).fileName();
        if (!name.isEmpty() && !w.unresolved->contains(name)) *w.unresolved << name;
        return;
    }
    const QString folder = folderOf(info.absoluteFilePath());
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
    const auto where = [&](QChar kind, const QString& name, const QString& comp = QString()) {
        const QString key = kind + folder + QLatin1Char('\n') + name + QLatin1Char('\n') + comp;
        auto it = w.found.constFind(key);
        if (it == w.found.constEnd()) {
            QString path = kind == QLatin1Char('L') ? LibComp::libraryFileOf(name, folder, comp, w.project)
                                                    : Subcircuit::subcircuitFileOf(name, folder);
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
            if (values.size() >= 2) addLibraryPart(where(QLatin1Char('L'), values.at(0), values.at(1)), values.at(0), values.at(1), w);
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
            if (part->Props.size() >= 2) {
                const QString lib = part->Props.at(0)->Value, comp = part->Props.at(1)->Value;
                addLibraryPart(LibComp::libraryFileOf(lib, sch->getFileInfo().dir().path(), comp, w.project), lib, comp, w);
            }
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
    QString sum;        // a copy's: SHA-256 of it as Qucs-S made it (none in a record of before)
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
                        i.value(QLatin1String("kind")).toString(), i.value(QLatin1String("sum")).toString()};
        // (Inside the folder only, as far as its path reads; what is taken
        // away or written is one pathIn() finds in the folder as it is.)
        if (!item.path.isEmpty() && !item.path.startsWith(QLatin1String("..")) && !QDir::isAbsolutePath(item.path))
            r.items << item;
    }
    *out = r;
    return !r.library.isEmpty();
}

// readRecord(), each file read again only when it changed - or this wrote
// it: on a disk whose times are coarse (HFS+, FAT) a record written again
// within the second has the time it had.
QHash<QString, std::pair<QDateTime, Record>> recordsRead;

bool readRecordOnce(const QString& file, Record* out)
{
    const QFileInfo info(file);
    if (!info.isFile()) return false;
    auto it = recordsRead.constFind(file);
    if (it == recordsRead.constEnd() || it->first != info.lastModified()) {
        Record r;
        readRecord(file, &r);
        it = recordsRead.insert(file, {info.lastModified(), r});
    }
    *out = it->second;
    return !out->library.isEmpty();
}

// SHA-256 of the file \a path, in hex; none when it cannot be read.
QString sumOf(const QString& path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return {};
    QCryptographicHash hash(QCryptographicHash::Sha256);
    if (!hash.addData(&f)) return {};
    return QString::fromLatin1(hash.result().toHex());
}

// \a path is still the copy Qucs-S made of \a item: the sum the record has
// of it; in a record of before sums, the same bytes as its original (another
// file). A file of the user's under its name - or one a record that is not
// Qucs-S's names - is not.
bool stillOurCopy(const QString& path, const Item& item)
{
    if (!item.sum.isEmpty()) return sumOf(path) == item.sum;
    const QFileInfo original(item.original);
    if (!original.isFile() || misc::isSameFile(item.original, path) || original.size() != QFileInfo(path).size()) return false;
    return sumOf(item.original) == sumOf(path);
}

void writeRecord(const QString& file, const Record& r)
{
    recordsRead.remove(file);
    const QDir dir = QFileInfo(file).absoluteDir();
    QJsonArray files;
    for (const Item& item : r.items) {
        QJsonObject o{{QStringLiteral("path"), item.path}, {QStringLiteral("original"), item.original},
                      {QStringLiteral("kind"), item.kind}};
        if (item.kind != QLatin1String("link")) o.insert(QStringLiteral("sum"), sumOf(dir.absoluteFilePath(item.path)));
        files.append(o);
    }
    const QJsonObject o{{QStringLiteral("library"), r.library}, {QStringLiteral("folder"), r.folder},
                        {QStringLiteral("created"), r.created}, {QStringLiteral("files"), files},
                        {QStringLiteral("about"), QStringLiteral("Kept by Qucs-S: the Verilog-A of this library's devices "
                                                                 "the project's schematics use. Edits are lost.")}};
    QFile f(file);
    if (f.open(QIODevice::WriteOnly)) f.write(QJsonDocument(o).toJson());
}

// ---- the files ------------------------------------------------------------

bool (*linkMaker)(const QString& target, const QString& link) = nullptr;   // setLinkMaker()

bool makeLink(const QString& target, const QString& link)
{
    return linkMaker != nullptr ? linkMaker(target, link) : QFile::link(target, link);
}

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

    // The library folders there are, by their records: in Libraries/; and,
    // made before there was one, in the project's folder itself (moved).
    const QString root = project.absoluteFilePath(QLatin1String(FolderName));
    const QString marker = QDir(root).absoluteFilePath(QLatin1String(MarkerName));
    // Libraries/ a link (come with a project from elsewhere): what is there
    // is not the project's - nothing read, written or taken away through it.
    if (QFileInfo(root).isSymLink()) {
        report.conflicts << QString::fromLatin1(FolderName);
        return report;
    }
    const auto recordsIn = [](const QString& folder) {
        QMap<QString, Record> records;   // by the folder's name
        for (const QFileInfo& dir : QDir(folder).entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot | QDir::NoSymLinks)) {
            Record r;
            if (readRecord(QDir(dir.absoluteFilePath()).absoluteFilePath(QLatin1String(RecordName)), &r)) records.insert(dir.fileName(), r);
        }
        return records;
    };
    const QMap<QString, Record> found = recordsIn(root);
    const QMap<QString, Record> before = recordsIn(project.absolutePath());

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
    QHash<QString, QString> placed;   // the library's folder -> its folder's name in Libraries/
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
            if (taken.contains(name) || found.contains(name)) continue;   // another library's
            placed.insert(folder, name);
            taken.insert(name);
            break;
        }
    }

    // What no schematic uses now: emptied of what Qucs-S put there - unless
    // a part names its library and it is not found here.
    const auto shown = [&](const QString& path) { return QDir::fromNativeSeparators(project.relativeFilePath(path)); };
    const auto takeAway = [&](const QString& dir, const Item& item) {
        const QString path = pathIn(dir, item.path);
        if (path.isEmpty()) return;   // (not in its folder)
        if (item.kind != QLatin1String("link") && !stillOurCopy(path, item)) return;   // the user's, or changed: kept
        if (removeItem(path, item.kind)) {
            if (item.kind != QLatin1String("include")) {
                removeModel(path);
                report.removed << shown(path);
            }
        }
        removeEmptyUpTo(QFileInfo(path).absolutePath(), QDir::cleanPath(dir));
    };
    const auto close = [&](const QString& dir, const Record& r) {
        QFile::remove(QDir(dir).absoluteFilePath(QLatin1String(RecordName)));
        if (r.created) QDir().rmdir(dir);   // (only when empty)
    };
    for (auto it = before.cbegin(); it != before.cend(); ++it) {
        if (unresolved.contains(it->library)) continue;   // not found here: kept as it is
        const QString dir = project.absoluteFilePath(it.key());
        for (const Item& item : it->items) takeAway(dir, item);
        close(dir, *it);
    }
    for (auto it = found.cbegin(); it != found.cend(); ++it) {
        if (taken.contains(it.key())) continue;
        if (unresolved.contains(it->library)) continue;
        const QString dir = QDir(root).absoluteFilePath(it.key());
        for (const Item& item : it->items) takeAway(dir, item);
        close(dir, *it);
    }

    // Each library used: its sources in its folder, and nothing else of Qucs-S's.
    for (const QString& folder : std::as_const(folders)) {
        const QString name = placed.value(folder);
        const QString dir = QDir(root).absoluteFilePath(name);
        if (!QFileInfo::exists(root)) {
            // Libraries/ made: marked as Qucs-S's, taken away when empty again.
            QDir().mkpath(root);
            QFile m(marker);
            if (m.open(QIODevice::WriteOnly))
                m.write("Made by Qucs-S for the Verilog-A of the library devices the project's schematics use: "
                        "taken away when it holds nothing else.\n");
        }
        const bool known = found.contains(name);
        Record r = found.value(name);
        if (!known) r.created = !QFileInfo::exists(dir);
        // Its folder a link (to a folder elsewhere): not written through.
        if (QFileInfo(dir).isSymLink()) {
            report.conflicts << shown(dir);
            continue;
        }
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
            QDir().mkpath(dir);
            const QString path = pathIn(dir, relative);
            if (path.isEmpty()) {   // a folder on its way leads elsewhere
                report.conflicts << shown(QDir(dir).absoluteFilePath(relative));
                continue;
            }
            const bool ours = had.contains(relative);
            const QFileInfo there(path);
            QDir().mkpath(there.absolutePath());
            // A link by the way from it to the file (taken from where the
            // link really is): it holds when the project and the library move
            // together, or are another user's in the same places.
            const QString target = QDir(real(there.absolutePath())).relativeFilePath(u.source);
            bool copy = mode == Mode::Copy;
            if (!copy) {
                if (there.isSymLink() && real(there.symLinkTarget()) == u.source) {
                    keep(Item{relative, u.source, QStringLiteral("link"), {}});   // as it should be
                    continue;
                }
                if ((there.exists() || there.isSymLink()) && !ours) {
                    report.conflicts << shown(path);
                    continue;
                }
                if (ours && had.value(relative).kind != QLatin1String("link") && there.isFile() && !there.isSymLink()) {
                    // A copy of before (copies, or no links could be made here
                    // then): a link in its place if one can be made here now.
                    const QString trial = path + QStringLiteral(".qucs-link");
                    QFile::remove(trial);
                    const bool linkable = makeLink(target, trial);
                    QFile::remove(trial);
                    if (linkable) {
                        QFile::remove(path);
                        removeModel(path);
                    }
                    copy = !linkable;
                } else if (there.exists() || there.isSymLink()) {
                    // It led elsewhere (the library moved, another computer):
                    // made again, its model with it (of another file).
                    QFile::remove(path);
                    removeModel(path);
                }
                if (!copy) {
                    if (makeLink(target, path)) {
                        report.made << shown(path);
                        keep(Item{relative, u.source, QStringLiteral("link"), {}});
                        continue;
                    }
                    copy = true;   // a disk with no symbolic links (exFAT, some network shares): a copy
                }
            }
            bool made = false;
            if (!copyFresh(u.source, path, ours, &made)) {
                report.conflicts << shown(path);
                continue;
            }
            if (made) report.made << shown(path);
            keep(Item{relative, u.source, QStringLiteral("copy"), {}});
            // The files it includes, at their places beside it, so that its
            // `include lines find them (those of the library's folder).
            for (const QString& included : osdi::sourceIncludes(u.source)) {
                const QString original = real(included);
                const QString at = QDir(folder).relativeFilePath(original);
                if (at.startsWith(QLatin1String(".."))) continue;
                const QString to = pathIn(dir, at);
                bool copied = false;
                if (!to.isEmpty() && copyFresh(original, to, had.contains(at), &copied))
                    keep(Item{at, original, QStringLiteral("include"), {}});
            }
        }
        for (const Item& item : std::as_const(r.items))
            if (std::none_of(items.cbegin(), items.cend(), [&](const Item& i) { return i.path == item.path; }))
                takeAway(dir, item);
        r.items = items;
        if (items.isEmpty()) {
            close(dir, r);
        } else {
            writeRecord(QDir(dir).absoluteFilePath(QLatin1String(RecordName)), r);
        }
    }
    // Libraries/ as Qucs-S made it (its mark), holding nothing else now: taken away.
    if (QDir(root).entryList(QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot) == QStringList{QLatin1String(MarkerName)}) {
        QFile::remove(marker);
        QDir().rmdir(root);
    }
    return report;
}

QStringList linkedFolders(const QString& projectDir, const QString& library)
{
    if (projectDir.isEmpty() || library.isEmpty()) return {};
    const QString root = QDir(projectDir).absoluteFilePath(QLatin1String(FolderName));
    QStringList folders;
    for (const QFileInfo& dir : QDir(root).entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot | QDir::NoSymLinks)) {
        Record r;
        if (readRecordOnce(QDir(dir.absoluteFilePath()).absoluteFilePath(QLatin1String(RecordName)), &r) && r.library == library
            && !folders.contains(r.folder))
            folders << r.folder;
    }
    return folders;
}

QString folderOf(const QString& libraryFile)
{
    const QFileInfo info(libraryFile);
    return real(info.absoluteDir().absoluteFilePath(info.completeBaseName()));
}

void setLinkMaker(bool (*make)(const QString& target, const QString& link))
{
    linkMaker = make;
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

QString notToWrite(const QString& path)
{
    if (path.isEmpty()) return {};
    const QFileInfo info(path);
    // A link that leads nowhere (the library moved) has no file to tell it
    // by: the same link, its folder as it really is, its name in any case.
    const auto place = [](const QFileInfo& f) { return QFileInfo(f.absolutePath()).canonicalFilePath(); };
    QDir dir = info.absoluteDir();
    for (int up = 0; up < 8; ++up) {
        const QString record = dir.absoluteFilePath(QLatin1String(RecordName));
        if (QFileInfo::exists(record)) {
            Record r;
            if (!readRecord(record, &r)) return {};
            for (const Item& item : std::as_const(r.items)) {
                const QFileInfo at(dir.absoluteFilePath(item.path));
                const bool same = misc::isSameFile(at.absoluteFilePath(), info.absoluteFilePath())
                                  || (at.isSymLink() && info.isSymLink() && place(at) == place(info)
                                      && at.fileName().compare(info.fileName(), Qt::CaseInsensitive) == 0
                                      && at.symLinkTarget() == info.symLinkTarget());
                if (!same) continue;
                if (item.kind == QLatin1String("link"))
                    return QObject::tr("%1 is the Verilog-A of the library %2, linked into the project from %3: written, "
                                       "the library's own file would change, which every project using it shares. Write "
                                       "to another file - the library's Verilog-A is changed in the library.")
                        .arg(QDir::toNativeSeparators(info.absoluteFilePath()), r.library, QDir::toNativeSeparators(item.original));
                return QObject::tr("%1 is a copy of the library %2's Verilog-A (%3) that Qucs-S keeps in the project: what "
                                   "is written there is lost when it is renewed. Write to another file.")
                    .arg(QDir::toNativeSeparators(info.absoluteFilePath()), r.library, QDir::toNativeSeparators(item.original));
            }
            return {};
        }
        if (!dir.cdUp()) break;
    }
    return {};
}

} // namespace qucs_s::projectlibraries
