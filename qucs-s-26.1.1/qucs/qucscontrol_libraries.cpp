/*
 * qucscontrol_libraries.cpp - Claude's library tools: list_libraries (the
 *                             Libraries panel's sections, their libraries
 *                             and a library's parts), create_library (the
 *                             project's subcircuits made a library, as
 *                             Project > Create Library makes one) and
 *                             import_library (a library file, and its
 *                             folder of models, brought into user_lib, the
 *                             project or a folder of the library search
 *                             paths)
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

#include "main.h"
#include "misc.h"
#include "projectView.h"
#include "qucs.h"
#include "qucsdoc.h"
#include "qucslib_common.h"
#include "dialogs/librarydialog.h"
#include "components/libcomp.h"

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>

using namespace qucs_s::control;

namespace {

// What a .lib file is: a Qucs-S library, a SPICE library of subcircuits, or
// neither; its name (a Qucs-S library's own, else its file's) and its
// parts' count. Empty kind when it cannot be read.
struct LibraryFile {
    QString kind;   // qucs, spice, none; empty: not readable
    QString name;
    int parts = 0;
};

LibraryFile readLibraryFile(const QString& file)
{
    LibraryFile lib;
    QFile f(file);
    if (!f.open(QIODevice::ReadOnly)) return lib;
    const QString text = QString::fromUtf8(f.readAll());
    static const QRegularExpression header(QStringLiteral("<Qucs Library \\S+ \"([^\"]*)\">"));
    static const QRegularExpression subckt(QStringLiteral("^\\s*\\.subckt\\s"),
                                           QRegularExpression::CaseInsensitiveOption | QRegularExpression::MultilineOption);
    lib.name = QFileInfo(file).baseName();
    if (const QRegularExpressionMatch m = header.match(text); m.hasMatch() && m.capturedStart() < 1024) {
        lib.kind = QStringLiteral("qucs");
        if (!m.captured(1).trimmed().isEmpty()) lib.name = m.captured(1).trimmed();
        lib.parts = int(text.count(QStringLiteral("\n<Component ")));
    } else {
        for (auto it = subckt.globalMatch(text); it.hasNext(); it.next()) ++lib.parts;
        lib.kind = lib.parts > 0 ? QStringLiteral("spice") : QStringLiteral("none");
    }
    return lib;
}

// The sections of the Libraries panel, in its order: each its kind, folder
// and whether it is there.
struct Section {
    QString kind;   // installed, user, search path, project
    QString folder;
};

QList<Section> librarySections()
{
    QList<Section> sections{{QStringLiteral("installed"), QucsSettings.LibDir},
                            {QStringLiteral("user"), QucsSettings.qucsWorkspaceDir.filePath(QStringLiteral("user_lib"))}};
    QStringList listed{QFileInfo(sections.at(0).folder).canonicalFilePath(), QFileInfo(sections.at(1).folder).canonicalFilePath()};
    const bool project = QucsMain != nullptr && !QucsMain->ProjName.isEmpty();
    if (project) listed << QucsSettings.QucsWorkDir.canonicalPath();
    for (const QString& path : std::as_const(QucsSettings.LibraryPaths)) {
        const QString canonical = QFileInfo(QDir::cleanPath(path)).canonicalFilePath();
        if (!canonical.isEmpty() && listed.contains(canonical)) continue;   // shown already, as the panel does
        if (!canonical.isEmpty()) listed << canonical;
        sections.append({QStringLiteral("search path"), QDir::cleanPath(path)});
    }
    if (project) sections.append({QStringLiteral("project"), QucsSettings.QucsWorkDir.absolutePath()});
    return sections;
}

// How a part of \a file is placed: a Qucs-S library's with add_component
// type Lib (an installed library by its name, another by its path, as the
// panel places it), a SPICE library's as SpLib.
QJsonObject placement(const QString& kind, const QString& file, const QString& part, bool installed)
{
    if (kind == QLatin1String("spice"))
        return QJsonObject{{QStringLiteral("type"), QStringLiteral("SpLib")},
                           {QStringLiteral("properties"), QJsonObject{{QStringLiteral("File"), file}, {QStringLiteral("Device"), part}}}};
    // By its name when that finds it, else by its path (LibComp::referenceTo()).
    const QString lib = installed ? QFileInfo(file).completeBaseName() : LibComp::referenceTo(file);
    return QJsonObject{{QStringLiteral("type"), QStringLiteral("Lib")},
                       {QStringLiteral("properties"), QJsonObject{{QStringLiteral("Lib"), lib}, {QStringLiteral("Comp"), part}}}};
}

// The parts of a library, each its name, description and how it is placed.
QJsonArray partsOf(const QString& file, const QString& kind, bool installed)
{
    ComponentLibrary parsed;
    QJsonArray parts;
    if (parseComponentLibrary(file.left(file.size() - 4), parsed, QUCS_COMP_LIB_FULL, false) != QUCS_COMP_LIB_OK) return parts;
    for (const ComponentLibraryItem& item : std::as_const(parsed.components)) {
        QString definition = item.definition, description;
        getSection(QStringLiteral("Description"), definition, description);
        QJsonObject o{{QStringLiteral("part"), item.name}, {QStringLiteral("place"), placement(kind, file, item.name, installed)}};
        if (!description.trimmed().isEmpty()) o.insert(QStringLiteral("description"), description.trimmed().left(300));
        parts.append(o);
    }
    return parts;
}

// The folder 'destination' names: user_lib (when not given), project, or a
// folder of the library search paths. Empty, and why, otherwise.
QString destinationFolder(const QString& given, QString* error)
{
    const QString d = given.trimmed();
    if (d.isEmpty() || d.compare(QLatin1String("user_lib"), Qt::CaseInsensitive) == 0)
        return QucsSettings.qucsWorkspaceDir.filePath(QStringLiteral("user_lib"));
    if (d.compare(QLatin1String("project"), Qt::CaseInsensitive) == 0) {
        if (QucsMain == nullptr || QucsMain->ProjName.isEmpty()) {
            *error = QucsControl::tr("No project is open: 'destination' project needs one (open_project), or give user_lib.");
            return {};
        }
        return QucsSettings.QucsWorkDir.absolutePath();
    }
    const QString wanted = QFileInfo(QDir::cleanPath(absolute(d))).canonicalFilePath();
    QStringList folders;
    for (const QString& path : std::as_const(QucsSettings.LibraryPaths)) {
        folders << QDir::toNativeSeparators(path);
        if (!wanted.isEmpty() && QFileInfo(QDir::cleanPath(path)).canonicalFilePath() == wanted) return QDir::cleanPath(path);
    }
    *error = QucsControl::tr("%1 is not a folder of the library search paths%2. 'destination' is user_lib, project or one of "
                             "them; set_settings (scope app, \"Locations/Library search paths\") adds a folder.")
                 .arg(QDir::toNativeSeparators(d), folders.isEmpty() ? QucsControl::tr(" (there are none)")
                                                                     : QStringLiteral(" (%1)").arg(folders.join(QStringLiteral(", "))));
    return {};
}

// \a from (a folder) copied into \a to, every file; the files written.
bool copyFolder(const QString& from, const QString& to, QStringList* written)
{
    if (!QDir().mkpath(to)) return false;
    // (Listed before one is written: a copy is not walked into and copied
    // again.)
    QStringList files;
    for (QDirIterator it(from, QDir::Files | QDir::NoDotAndDotDot | QDir::Hidden, QDirIterator::Subdirectories); it.hasNext();)
        files << it.next();
    for (const QString& file : std::as_const(files)) {
        const QString target = QDir(to).filePath(QDir(from).relativeFilePath(file));
        if (!QDir().mkpath(QFileInfo(target).absolutePath()) || !QFile::copy(file, target)) return false;
        *written << target;
    }
    return true;
}

// A SPICE include line (.include, .inc, .lib FILE): its file, quoted or not,
// is captured 1, 2 or 3.
const QRegularExpression& includeLine()
{
    static const QRegularExpression include(QStringLiteral(R"re(^\s*\.(?:include|inc|lib)\s+(?:"([^"]+)"|'([^']+)'|(\S+)))re"),
                                            QRegularExpression::CaseInsensitiveOption | QRegularExpression::MultilineOption);
    return include;
}

int namedGroup(const QRegularExpressionMatch& m)
{
    return !m.captured(1).isEmpty() ? 1 : !m.captured(2).isEmpty() ? 2 : 3;
}

// The files a SPICE library \a file includes (.include, .inc, .lib FILE),
// and those they include: each by its path from the library's folder in
// \a beside; one a relative path leads out of the folder to in \a outside.
// Those named by a full path are found where they are, and not listed.
void spiceIncludesOf(const QString& file, QList<std::pair<QString, QString>>* beside, QStringList* outside)
{
    const QRegularExpression& include = includeLine();
    const QDir root = QFileInfo(file).absoluteDir();
    QStringList pending{QFileInfo(file).absoluteFilePath()};
    QSet<QString> seen{QFileInfo(file).canonicalFilePath()};
    while (!pending.isEmpty() && seen.size() < 1000) {
        QFile f(pending.takeFirst());
        if (!f.open(QIODevice::ReadOnly)) continue;
        const QString text = QString::fromUtf8(f.readAll());
        const QDir here = QFileInfo(f.fileName()).absoluteDir();
        for (auto it = include.globalMatch(text); it.hasNext();) {
            const QRegularExpressionMatch m = it.next();
            const QString named = m.captured(namedGroup(m));
            if (QFileInfo(named).isAbsolute() || named.startsWith(QLatin1Char('~'))) continue;   // found where it is
            const QFileInfo info(QDir::cleanPath(here.absoluteFilePath(named)));
            if (!info.isFile() || seen.contains(info.canonicalFilePath())) continue;   // (a .lib line naming a section)
            seen.insert(info.canonicalFilePath());
            pending << info.absoluteFilePath();
            const QString relative = root.relativeFilePath(info.absoluteFilePath());
            if (relative.startsWith(QLatin1String(".."))) *outside << info.absoluteFilePath();
            else beside->append({info.absoluteFilePath(), relative});
        }
    }
}

// The SPICE file \a file, read from \a wasIn and written as \a file, with
// each include line naming a file that \a moved (its old path, canonical, to
// its new one) took elsewhere than its path now leads named by the path that
// does. Its other bytes are written as they were (read as Latin-1).
bool followMoved(const QString& file, const QDir& wasIn, const QHash<QString, QString>& moved)
{
    QFile f(file);
    if (!f.open(QIODevice::ReadOnly)) return false;
    QString text = QString::fromLatin1(f.readAll());
    f.close();
    const QDir now = QFileInfo(file).absoluteDir();
    QList<QRegularExpressionMatch> lines;   // (last first: an earlier one's place is kept)
    for (auto it = includeLine().globalMatch(text); it.hasNext();) lines.prepend(it.next());
    bool changed = false;
    for (const QRegularExpressionMatch& m : std::as_const(lines)) {
        const int n = namedGroup(m);
        const QString named = QString::fromUtf8(m.captured(n).toLatin1());
        if (QFileInfo(named).isAbsolute() || named.startsWith(QLatin1Char('~'))) continue;
        const QString to = moved.value(QFileInfo(QDir::cleanPath(wasIn.absoluteFilePath(named))).canonicalFilePath());
        if (to.isEmpty() || QDir::cleanPath(now.absoluteFilePath(named)) == to) continue;
        text.replace(m.capturedStart(n), m.capturedLength(n), QString::fromLatin1(now.relativeFilePath(to).toUtf8()));
        changed = true;
    }
    if (!changed) return true;
    return f.open(QIODevice::WriteOnly | QIODevice::Truncate) && f.write(text.toLatin1()) == text.size();
}

// The other libraries of the name of \a file, when there are: "also_named",
// and what that means for a part placed by the name.
void addNamedLike(QJsonObject& result, const QString& file)
{
    const QStringList others = LibComp::librariesNamedLike(file);
    if (others.isEmpty()) return;
    QJsonArray named;
    for (const QString& other : others) named.append(QDir::toNativeSeparators(other));
    result.insert(QStringLiteral("also_named"), named);
    result.insert(QStringLiteral("warning"),
                  QucsControl::tr("Another library is named %1 too (also_named). A part placed by that name is taken from the first "
                                  "of them that has it - installed, beside the schematic, the project's, user_lib's, the library "
                                  "search paths' in their order - so a part of a name both have may be the other's: each part's "
                                  "'place' here names this library by its path where the name finds another. A name of its own "
                                  "avoids it (import_library's 'name').")
                      .arg(QFileInfo(file).completeBaseName()));
}

} // namespace

QJsonObject QucsControl::listLibraries(const QJsonObject& args)
{
    const QString wanted = args.value(QLatin1String("library")).toString().trimmed();
    const QStringList blacklisted = getBlacklistedLibraries(QucsSettings.LibDir);
    QJsonArray sections;
    for (const Section& section : librarySections()) {
        const bool installed = section.kind == QLatin1String("installed");
        QJsonObject s{{QStringLiteral("section"), section.kind}, {QStringLiteral("folder"), QDir::toNativeSeparators(section.folder)}};
        if (!QFileInfo(section.folder).isDir()) {
            s.insert(QStringLiteral("missing"), true);
            sections.append(s);
            continue;
        }
        QJsonArray libraries;
        for (const QFileInfo& fi : QDir(section.folder).entryInfoList(QStringList("*.lib"), QDir::Files, QDir::Name)) {
            const LibraryFile lib = readLibraryFile(fi.absoluteFilePath());
            // One library, by its name or its file: its parts.
            if (!wanted.isEmpty()) {
                const bool same = lib.name.compare(wanted, Qt::CaseInsensitive) == 0
                                  || fi.completeBaseName().compare(wanted, Qt::CaseInsensitive) == 0
                                  || QFileInfo(absolute(wanted)).canonicalFilePath() == fi.canonicalFilePath();
                if (!same) continue;
                if (lib.kind.isEmpty() || lib.kind == QLatin1String("none"))
                    return errorResult(tr("%1 (%2) is not a library Qucs-S reads.").arg(wanted, QDir::toNativeSeparators(fi.absoluteFilePath())));
                QJsonObject o{{QStringLiteral("library"), lib.name},
                              {QStringLiteral("file"), QDir::toNativeSeparators(fi.absoluteFilePath())},
                              {QStringLiteral("kind"), lib.kind},
                              {QStringLiteral("section"), section.kind},
                              {QStringLiteral("parts"), partsOf(fi.absoluteFilePath(), lib.kind, installed)}};
                o.insert(QStringLiteral("note"), tr("Each part's 'place' is its add_component; describe_part (library, part) gives a Qucs-S "
                                                    "library part's pins."));
                if (const QString why = LibComp::newerVersionReason(fi.absoluteFilePath()); !why.isEmpty())
                    o.insert(QStringLiteral("not read"), tr("Its parts cannot be placed: %1.").arg(why));
                return jsonResult(o);
            }
            QJsonObject o{{QStringLiteral("name"), lib.name}, {QStringLiteral("file"), QDir::toNativeSeparators(fi.absoluteFilePath())}};
            if (lib.kind.isEmpty()) o.insert(QStringLiteral("unreadable"), tr("it cannot be opened"));
            else if (lib.kind == QLatin1String("none")) o.insert(QStringLiteral("unreadable"), tr("not a library Qucs-S reads"));
            else {
                o.insert(QStringLiteral("kind"), lib.kind);
                o.insert(QStringLiteral("parts"), lib.parts);
                if (const QString why = LibComp::newerVersionReason(fi.absoluteFilePath()); !why.isEmpty())
                    o.insert(QStringLiteral("not read"), why);
            }
            if (installed && blacklisted.contains(fi.fileName()))
                o.insert(QStringLiteral("hidden"), tr("not shown with this simulator (its blacklist)"));
            libraries.append(o);
        }
        s.insert(QStringLiteral("libraries"), libraries);
        sections.append(s);
    }
    if (!wanted.isEmpty())
        return errorResult(tr("No library %1 in the Libraries panel's folders: list_libraries without 'library' lists them.").arg(wanted));
    return jsonResult(QJsonObject{
        {QStringLiteral("sections"), sections},
        {QStringLiteral("note"), tr("As the Libraries panel shows them. 'library' gives one's parts; find_library_component finds a part by its "
                                    "values; create_library makes one of the project's subcircuits; import_library brings one in; set_settings "
                                    "(scope app, \"Locations/Library search paths\") adds a folder of them.")}});
}

QJsonObject QucsControl::createLibrary(const QJsonObject& args)
{
    if (QucsMain == nullptr || QucsMain->ProjName.isEmpty())
        return errorResult(tr("No project is open: a library is made of a project's subcircuits (open_project, or new_project)."));
    const QString name = args.value(QLatin1String("name")).toString().trimmed();
    if (name.isEmpty()) return errorResult(tr("'name' is the library's name: letters, digits and _ (MyAmps)."));
    QString error;
    const QString folder = destinationFolder(args.value(QLatin1String("destination")).toString(), &error);
    if (folder.isEmpty()) return errorResult(error);

    // The project's subcircuits, as the dialog lists them; those asked for,
    // by their names with or without .sch.
    if (ProjectView* content = a_app->projectView()) content->refresh();
    const QStringList available = a_app->projectView() != nullptr ? a_app->projectView()->exportSchematic() : QStringList();
    if (available.isEmpty())
        return errorResult(tr("The project %1 has no subcircuit (a schematic with ports): create_subcircuit makes one.").arg(QucsMain->ProjName));
    QStringList chosen;
    const QJsonValue subcircuits = args.value(QLatin1String("subcircuits"));
    if (subcircuits.isUndefined() || subcircuits.isNull()) {
        chosen = available;
    } else {
        if (!subcircuits.isArray() || subcircuits.toArray().isEmpty())
            return errorResult(tr("'subcircuits' is a list of the project's subcircuits (%1); all of them when not given.")
                                   .arg(available.join(QStringLiteral(", "))));
        for (const QJsonValue& v : subcircuits.toArray()) {
            const QString given = v.toString().trimmed();
            // Its path (amp, amp.sch, sub/amp), else its name alone when one
            // subcircuit has it (the last of several was taken, unsaid).
            QStringList found;
            for (const QString& a : available)
                if (a.compare(given, Qt::CaseInsensitive) == 0 || a.compare(given + QStringLiteral(".sch"), Qt::CaseInsensitive) == 0)
                    found = {a};
            if (found.isEmpty())
                for (const QString& a : available)
                    if (QFileInfo(a).completeBaseName().compare(given, Qt::CaseInsensitive) == 0) found << a;
            if (found.isEmpty())
                return errorResult(tr("%1 is not a subcircuit of the project %2: they are %3.").arg(given, QucsMain->ProjName, available.join(QStringLiteral(", "))));
            if (found.size() > 1)
                return errorResult(tr("%1 is the name of %2: name the one meant by its path.").arg(given, found.join(QStringLiteral(", "))));
            if (!chosen.contains(found.first())) chosen << found.first();
        }
    }
    // Made of the files: one open with unsaved changes is saved first.
    for (QucsDoc* doc : a_app->allDocuments())
        for (const QString& sub : std::as_const(chosen))
            if (doc->getDocChanged() && !doc->getDocName().isEmpty()
                && QFileInfo(doc->getDocName()).canonicalFilePath() == QFileInfo(QucsSettings.QucsWorkDir.filePath(sub)).canonicalFilePath())
                return errorResult(tr("%1 is open with unsaved changes, and a library is made of the saved file: save_document it "
                                      "first (or close it without them).").arg(sub));
    LibraryDialog::Request request;
    request.name = name;
    request.subcircuits = chosen;
    request.folder = folder;
    const QJsonObject descriptions = args.value(QLatin1String("descriptions")).toObject();
    for (auto it = descriptions.begin(); it != descriptions.end(); ++it) request.descriptions.insert(it.key(), it.value().toString());
    request.analogOnly = !args.value(QLatin1String("digital_models")).toBool();
    request.embedVerilogA = args.value(QLatin1String("embed_verilog_a")).toBool(QucsSettings.EmbedVerilogAInLibraries);
    request.groundPin = args.value(QLatin1String("ground_pin")).toBool(QucsSettings.LibraryGroundPin);
    request.replace = args.value(QLatin1String("replace")).toBool();
    // (Replaced: the old one goes to the trash once the new one is made -
    // read while it is made, when a subcircuit places its parts - and stays
    // as it was when the new one cannot be.)
    const QString file = QDir(folder).filePath(name + QStringLiteral(".lib"));
    LibraryDialog dialog(a_app);
    dialog.fillSchematicList(available);
    QString log;
    QStringList trashed;
    const bool made = dialog.create(request, &log, &error, &trashed);
    a_app->fillLibrariesTreeView();
    const QStringList messages = log.split(QLatin1Char('\n'), Qt::SkipEmptyParts);
    if (!made && messages.isEmpty()) return errorResult(error + QLatin1Char('.'));
    if (!made) {
        QJsonObject o{{QStringLiteral("error"), error}, {QStringLiteral("messages"), QJsonArray::fromStringList(messages)}};
        return textResult(QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Compact)), true);
    }
    QJsonObject result{{QStringLiteral("library"), name},
                       {QStringLiteral("file"), QDir::toNativeSeparators(file)},
                       {QStringLiteral("parts"), partsOf(file, QStringLiteral("qucs"), false)},
                       {QStringLiteral("messages"), QJsonArray::fromStringList(messages)}};
    if (const QString models = QDir(folder).filePath(name); QFileInfo(models).isDir()) {
        QJsonArray files;
        for (QDirIterator it(models, QDir::Files, QDirIterator::Subdirectories); it.hasNext();)
            files.append(QDir(folder).relativeFilePath(it.next()));
        result.insert(QStringLiteral("models"), files);
    }
    result.insert(QStringLiteral("note"), tr("It is in the Libraries panel; each part's 'place' is its add_component.%1")
                                              .arg(trashed.isEmpty() ? QString() : tr(" The library it replaced is in the trash.")));
    addNamedLike(result, file);
    return jsonResult(result);
}

QJsonObject QucsControl::importLibrary(const QJsonObject& args)
{
    const QString given = args.value(QLatin1String("path")).toString().trimmed();
    const QString newName = args.value(QLatin1String("name")).toString().trimmed();
    const QString destination = args.value(QLatin1String("destination")).toString();
    const bool replace = args.value(QLatin1String("replace")).toBool();
    if (given.isEmpty()) return errorResult(tr("'path' is the library file brought in (a .lib; its folder of models beside it comes too)."));
    static const QRegularExpression whole(QStringLiteral("^\\w+$"));
    if (!newName.isEmpty() && !whole.match(newName).hasMatch())
        return errorResult(tr("'name' is the name it is brought in under: letters, digits and _ (%1 is not).").arg(newName));
    const QString source = QDir::cleanPath(absolute(given));
    const QFileInfo info(source);
    if (!info.isFile()) return errorResult(tr("There is no file %1.").arg(QDir::toNativeSeparators(source)));
    if (info.suffix().compare(QLatin1String("lib"), Qt::CaseInsensitive) != 0)
        return errorResult(tr("%1 is not a library: a Qucs-S library or a SPICE library of subcircuits is a .lib file.").arg(info.fileName()));
    const LibraryFile lib = readLibraryFile(source);
    if (lib.kind.isEmpty()) return errorResult(tr("%1 cannot be read.").arg(QDir::toNativeSeparators(source)));
    if (lib.kind == QLatin1String("none"))
        return errorResult(tr("%1 is neither a Qucs-S library (<Qucs Library ...> first) nor a SPICE library with a .subckt: nothing was "
                              "brought in. A SPICE file of .model cards is found by find_library_component in the project as it is.")
                               .arg(info.fileName()));
    QString error;
    const QString folder = destinationFolder(destination, &error);
    if (folder.isEmpty()) return errorResult(error);
    // Under its own name, or 'name' (its parts' Lib is the file's name; the
    // library's models keep the name it was made under, which is read too).
    const QString base = newName.isEmpty() ? info.completeBaseName() : newName;
    const QString target = QDir(folder).filePath(base + QStringLiteral(".lib"));
    if (QFileInfo(target).canonicalFilePath() == info.canonicalFilePath())
        return errorResult(tr("%1 is there already: it is in the Libraries panel as it is.").arg(QDir::toNativeSeparators(target)));
    // Its folder of models (NAME/ beside NAME.lib), as Create Library writes
    // it and the library reads it.
    const QString modelsFrom = info.absoluteDir().filePath(info.completeBaseName());
    const QString modelsTo = QDir(folder).filePath(base);
    if (QFileInfo(modelsFrom).isDir()) {
        // (Brought into its own folder of models, it was copied into itself,
        // its copies copied again.)
        const QString inside = QFileInfo(modelsFrom).canonicalFilePath(), into = QFileInfo(folder).canonicalFilePath();
        if (!into.isEmpty() && (into == inside || into.startsWith(inside + QLatin1Char('/'))))
            return errorResult(tr("%1 is the library's own folder of models, or in it: a library is not brought into itself. Its "
                                  "folder %2 is a library search path as it is.")
                                   .arg(QDir::toNativeSeparators(folder), QDir::toNativeSeparators(info.absolutePath())));
    }
    // A SPICE library's includes, beside it: brought along where it finds
    // them (left behind, every run stopped at the missing include).
    QList<std::pair<QString, QString>> beside;
    QStringList outside;
    if (lib.kind == QLatin1String("spice")) spiceIncludesOf(source, &beside, &outside);
    if (!outside.isEmpty()) {
        QStringList shown;
        for (const QString& f : std::as_const(outside)) shown << QDir::toNativeSeparators(f);
        return errorResult(tr("%1 includes %2 by a path that leads out of its folder, which would not be found from where it is brought: "
                              "nothing was brought in. Use it where it is - set_settings (scope app, \"Locations/Library search paths\") "
                              "adds its folder %3.")
                               .arg(info.fileName(), shown.join(QStringLiteral(", ")), QDir::toNativeSeparators(info.absolutePath())));
    }
    // A SPICE library's files go into a folder of its own, NAME/, as a Qucs-S
    // library's models do, each at its path from the library's folder (its
    // folder of models too, then): in the folder they shared, a library
    // brought in after another with an include of the same path changed that
    // one's results. The library's include lines name them there.
    const bool spice = lib.kind == QLatin1String("spice");
    QString modelsInto = modelsTo;
    if (spice) modelsInto = QDir(modelsTo).filePath(info.completeBaseName());
    QList<std::pair<QString, QString>> includes;   // to copy: source, target
    QHash<QString, QString> moved{{info.canonicalFilePath(), target}};
    for (const auto& [from, relative] : std::as_const(beside)) {
        const QString to = QDir(modelsTo).filePath(relative);
        moved.insert(QFileInfo(from).canonicalFilePath(), to);
        // (One in its folder of models comes with the folder.)
        if (!QFileInfo(modelsFrom).isDir() || !relative.startsWith(info.completeBaseName() + QLatin1Char('/'))) includes.append({from, to});
    }
    const bool hasFolder = QFileInfo(modelsFrom).isDir() || !beside.isEmpty();
    // What would be written over: refused unless 'replace'.
    QStringList taken;
    if (QFileInfo::exists(target)) taken << target;
    if (hasFolder && QFileInfo::exists(modelsTo)) taken << modelsTo;
    if (!taken.isEmpty()) {
        QStringList shown;
        for (const QString& f : std::as_const(taken)) shown << QDir::toNativeSeparators(f);
        if (!replace)
            return errorResult(tr("%1 is there already ('replace' puts it in the trash and brings this one in%2).")
                                   .arg(shown.join(QStringLiteral(", ")), newName.isEmpty() ? tr("; 'name' brings it in under another name") : QString()));
        // (Its own files there would go to the trash before they were copied.)
        for (const QString& old : std::as_const(taken))
            for (const QString& from : QStringList{source, modelsFrom} + QStringList(moved.keys())) {
                const QString was = QFileInfo(old).canonicalFilePath(), file = QFileInfo(from).canonicalFilePath();
                if (!file.isEmpty() && (file == was || file.startsWith(was + QLatin1Char('/'))))
                    return errorResult(tr("%1, which 'replace' would put in the trash, holds %2, which the library brings: nothing was "
                                          "brought in. 'name' brings it in under another name.")
                                           .arg(QDir::toNativeSeparators(old), QDir::toNativeSeparators(from)));
            }
        for (const QString& old : std::as_const(taken))
            if (!misc::moveToTrash(old, nullptr))
                return errorResult(tr("%1 could not be moved to the trash: nothing was brought in.").arg(QDir::toNativeSeparators(old)));
    }
    if (!QDir().mkpath(folder)) return errorResult(tr("The folder %1 cannot be made.").arg(QDir::toNativeSeparators(folder)));
    if (!QFile::copy(source, target)) return errorResult(tr("%1 could not be copied to %2.").arg(QDir::toNativeSeparators(source), QDir::toNativeSeparators(folder)));
    QStringList written{target};
    if (QFileInfo(modelsFrom).isDir() && !copyFolder(modelsFrom, modelsInto, &written))
        return errorResult(tr("The library was copied, but not all of its folder %1: %2 files were.")
                               .arg(QDir::toNativeSeparators(modelsFrom)).arg(written.size() - 1));
    for (const auto& [from, to] : std::as_const(includes)) {
        if (!QDir().mkpath(QFileInfo(to).absolutePath()) || !QFile::copy(from, to))
            return errorResult(tr("The library was copied, but not %1, which it includes.").arg(QDir::toNativeSeparators(from)));
        written << to;
    }
    // Its include lines (and theirs) name its files where they are now.
    if (spice) {
        QList<std::pair<QString, QString>> files{{source, target}};
        for (const auto& [from, relative] : std::as_const(beside)) files.append({from, QDir(modelsTo).filePath(relative)});
        for (const auto& [from, to] : std::as_const(files))
            if (!followMoved(to, QFileInfo(from).absoluteDir(), moved))
                return errorResult(tr("The library was copied, but %1 could not be written to name its includes where they are now.")
                                       .arg(QDir::toNativeSeparators(to)));
    }
    a_app->fillLibrariesTreeView();
    QJsonArray files;
    for (const QString& f : std::as_const(written)) files.append(QDir::toNativeSeparators(f));
    // Its parts are placed by the file's name: its 'library'. The name it
    // was made under, when another: 'made as'.
    QJsonObject result{{QStringLiteral("library"), base},
                       {QStringLiteral("kind"), lib.kind},
                       {QStringLiteral("file"), QDir::toNativeSeparators(target)},
                       {QStringLiteral("written"), files},
                       {QStringLiteral("parts"), partsOf(target, lib.kind, false)},
                       {QStringLiteral("note"), tr("It is in the Libraries panel; each part's 'place' is its add_component.%1%2")
                                                    .arg(spice && !beside.isEmpty()
                                                             ? tr(" The files it includes are in its own folder %1/, where its include "
                                                                  "lines now name them: no other library's include is the same file.")
                                                                   .arg(base)
                                                             : QString(),
                                                         taken.isEmpty() ? QString() : tr(" What it replaced is in the trash."))}};
    if (lib.kind == QLatin1String("qucs") && lib.name != base) result.insert(QStringLiteral("made as"), lib.name);
    QStringList warnings;
    if (lib.kind == QLatin1String("qucs")) {
        // A library this Qucs-S does not read: said now, not when a part of
        // it will not be placed.
        if (const QString why = LibComp::newerVersionReason(target); !why.isEmpty()) {
            result.insert(QStringLiteral("made by"), LibComp::newerVersion(target));
            warnings << tr("Its parts cannot be placed: %1.").arg(why);
        }
        // The files its parts name in its folder that did not come with it
        // (a .lib sent alone): every run of such a part stopped there.
        // (Its SPICE models' under ngspice and Xyce, its Qucsator models'
        // under Qucsator.)
        QJsonArray missing;
        bool spiceNeeds = false, qucsNeeds = false, spiceModels = true;
        for (const QJsonValue& part : result.value(QStringLiteral("parts")).toArray()) {
            const QString name = part.toObject().value(QStringLiteral("part")).toString();
            const QStringList spiceMissing = LibComp::missingFiles(target, name, true, false), qucsMissing = LibComp::missingFiles(target, name, false, true);
            spiceNeeds |= !spiceMissing.isEmpty();
            qucsNeeds |= !qucsMissing.isEmpty();
            if (!qucsMissing.isEmpty() && !LibComp::hasSpiceModel(target, name)) spiceModels = false;
            for (const QString& file : spiceMissing + qucsMissing)
                if (!missing.contains(QJsonValue(base + QLatin1Char('/') + file))) missing.append(base + QLatin1Char('/') + file);
        }
        if (!missing.isEmpty()) {
            result.insert(QStringLiteral("missing"), missing);
            warnings << tr("Its parts need files that did not come with it (missing): its folder %1 beside %2 was not there, or "
                           "lacks them. Bring the folder too, or %3.")
                            .arg(info.completeBaseName(), QDir::toNativeSeparators(source),
                                 spiceNeeds && qucsNeeds ? tr("a run of a part that needs them stops at them (describe_part tells, for "
                                                              "each part, which its SPICE model needs and which its Qucsator model)")
                                 : spiceNeeds ? tr("a run of a part that needs them under ngspice or Xyce stops at them (its SPICE "
                                                   "model's)")
                                 : spiceModels ? tr("a run of a part that needs them under Qucsator stops at them (its Qucsator model's: "
                                                    "under ngspice or Xyce the parts are simulated from their SPICE models)")
                                               : tr("a run of a part that needs them under Qucsator stops at them (its Qucsator model's)"));
        }
        addNamedLike(result, target);
    }
    if (!warnings.isEmpty()) {
        const QString named = result.value(QStringLiteral("warning")).toString();
        result.insert(QStringLiteral("warning"), (warnings + (named.isEmpty() ? QStringList() : QStringList{named})).join(QLatin1Char(' ')));
    }
    return jsonResult(result);
}
