/*
 * qucscontrol_resources.cpp - what Claude's Qucs-S tools serve beside the
 *                             tools: MCP resources and their changes,
 *                             the uses that cannot be undone, questions
 *                             for the user
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#include "qucscontrol.h"
#include <QCryptographicHash>
#include "qucscontrol_p.h"

#include "qucs.h"
#include "erc.h"
#include "main.h"
#include "misc.h"
#include "projectView.h"
#include "projectlibraries.h"
#include "pythondoc.h"
#include "textdoc.h"
#include "workspace.h"
#include "qucsdoc.h"
#include "schematic.h"
#include "wire.h"
#include "components/component.h"
#include "diagrams/diagram.h"
#include "paintings/painting.h"

#include <QDir>
#include <QElapsedTimer>

#include <chrono>
#include <QFile>
#include <QPointer>
#include <QSet>
#include <QEventLoop>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTimer>
#include <QUrl>

using namespace qucs_s::control;

namespace {

const QString kPrefix = QStringLiteral("qucs://");

// The kinds of a document's resources: its URI's host, what it is, its type.
const struct {
    const char* kind;
    const char* what;
    const char* mime;
} kKinds[] = {
    {"schematic", "its .sch text, unsaved changes included", "text/x-qucs-schematic"},
    {"netlist", "the netlist a simulation would be given now", "text/x-spice"},
    {"netlist-map", "the netlist's lines with the part that wrote each, and the pins each node joins", "application/json"},
    {"dataset", "the variables of its last simulation's dataset", "application/json"},
};

QString uriOf(const char* kind, const QString& path)
{
    return kPrefix + QLatin1String(kind) + QLatin1Char('/') + QString::fromLatin1(QUrl::toPercentEncoding(path));
}

// What is selected in a schematic, to select again once it is made again
// from its text (as a preview puts it back): parts by name - an unnamed
// one by its type and place - wires by their ends, diagrams and paintings
// by their places in the lists. (A move previewed, then made, found
// nothing selected.)
struct Selected {
    QSet<QString> parts, wires;
    QSet<int> diagrams, paintings;

    static QString partKey(const Component* c)
    {
        return c->Name.isEmpty() || c->Name == QLatin1String("*") ? QStringLiteral("%1@%2,%3").arg(c->Model).arg(c->cx).arg(c->cy) : c->Name;
    }
    static QString wireKey(const Wire* w) { return QStringLiteral("%1,%2,%3,%4").arg(w->x1).arg(w->y1).arg(w->x2).arg(w->y2); }

    static Selected of(const Schematic* sch)
    {
        Selected s;
        for (const Component* c : sch->a_DocComps)
            if (c->isSelected) s.parts.insert(partKey(c));
        for (const Wire* w : sch->a_DocWires)
            if (w->isSelected) s.wires.insert(wireKey(w));
        int i = 0;
        for (const Diagram* d : sch->a_DocDiags) {
            if (d->isSelected) s.diagrams.insert(i);
            ++i;
        }
        i = 0;
        for (const Painting* p : sch->a_DocPaints) {
            if (p->isSelected) s.paintings.insert(i);
            ++i;
        }
        return s;
    }
    void apply(Schematic* sch) const
    {
        for (Component* c : sch->a_DocComps) c->isSelected = parts.contains(partKey(c));
        for (Wire* w : sch->a_DocWires) w->isSelected = wires.contains(wireKey(w));
        int i = 0;
        for (Diagram* d : sch->a_DocDiags) d->isSelected = diagrams.contains(i++);
        i = 0;
        for (Painting* p : sch->a_DocPaints) p->isSelected = paintings.contains(i++);
    }
};

} // namespace

QJsonArray QucsControl::resources() const
{
    QJsonArray list{QJsonObject{{QStringLiteral("uri"), kPrefix + QStringLiteral("state")},
                                {QStringLiteral("name"), QStringLiteral("state")},
                                {QStringLiteral("title"), tr("The Qucs-S window")},
                                {QStringLiteral("description"), tr("What get_state tells: the documents open, their revisions, the panes, the simulator")},
                                {QStringLiteral("mimeType"), QStringLiteral("application/json")}},
                    QJsonObject{{QStringLiteral("uri"), kPrefix + QStringLiteral("ngspice-commands")},
                                {QStringLiteral("name"), QStringLiteral("ngspice-commands")},
                                {QStringLiteral("title"), tr("ngspice's commands")},
                                {QStringLiteral("description"), tr("What ngspice_commands tells: every ngspice command in a line, by category, "
                                                                   "which the installed ngspice has, and how Qucs-S uses them")},
                                {QStringLiteral("mimeType"), QStringLiteral("text/plain")}},
                    QJsonObject{{QStringLiteral("uri"), kPrefix + QStringLiteral("instructions")},
                                {QStringLiteral("name"), QStringLiteral("instructions")},
                                {QStringLiteral("title"), tr("How to use these tools")},
                                {QStringLiteral("description"), tr("The server's instructions whole, for a client that cuts them "
                                                                   "(Claude Code keeps 2,048 characters unless "
                                                                   "CLAUDE_CODE_MAX_MCP_DESCRIPTION_LENGTH says more)")},
                                {QStringLiteral("mimeType"), QStringLiteral("text/plain")}}};
    for (QucsDoc* doc : a_app->allDocuments()) {
        auto* sch = dynamic_cast<Schematic*>(doc);
        if (sch == nullptr || doc->getDocName().isEmpty()) continue;
        const QString path = doc->getDocName();
        const QString file = QFileInfo(path).fileName();
        for (const auto& k : kKinds) {
            if (qstrcmp(k.kind, "dataset") == 0 && !datasetWritten(doc).isValid()) continue;
            if (sch->getIsSymbolOnly() && qstrcmp(k.kind, "schematic") != 0) continue;
            list.append(QJsonObject{{QStringLiteral("uri"), uriOf(k.kind, path)},
                                    {QStringLiteral("name"), QStringLiteral("%1 (%2)").arg(file, QLatin1String(k.kind))},
                                    {QStringLiteral("description"), tr("%1 of %2").arg(tr(k.what), QDir::toNativeSeparators(path))},
                                    {QStringLiteral("mimeType"), QLatin1String(k.mime)}});
        }
    }
    return list;
}

QJsonArray QucsControl::resourceTemplates() const
{
    QJsonArray list;
    for (const auto& k : kKinds)
        list.append(QJsonObject{{QStringLiteral("uriTemplate"), kPrefix + QLatin1String(k.kind) + QStringLiteral("/{path}")},
                                {QStringLiteral("name"), QLatin1String(k.kind)},
                                {QStringLiteral("description"), tr("%1 of an open schematic: {path} its file's path, percent-encoded").arg(tr(k.what))},
                                {QStringLiteral("mimeType"), QLatin1String(k.mime)}});
    return list;
}

// A resource's URI read: its kind and the document it is of.
bool QucsControl::resourceOf(const QString& uri, QString* kind, QucsDoc** doc, QString* error) const
{
    for (const QString& whole : {QStringLiteral("state"), QStringLiteral("ngspice-commands"), QStringLiteral("instructions")})
        if (uri == kPrefix + whole) {
            *kind = whole;
            *doc = nullptr;
            return true;
        }
    if (!uri.startsWith(kPrefix)) {
        *error = tr("%1 is not a Qucs-S resource (qucs://...).").arg(uri);
        return false;
    }
    const QString rest = uri.mid(kPrefix.size());
    const qsizetype slash = rest.indexOf(QLatin1Char('/'));
    *kind = slash < 0 ? rest : rest.left(slash);
    bool known = false;
    for (const auto& k : kKinds) known = known || *kind == QLatin1String(k.kind);
    if (!known || slash < 0) {
        *error = tr("There is no resource %1 (resources/templates/list gives their forms).").arg(uri);
        return false;
    }
    const QString path = QUrl::fromPercentEncoding(rest.mid(slash + 1).toUtf8());
    *doc = document(QJsonObject{{QStringLiteral("path"), path}}, error);
    if (*doc == nullptr) return false;
    if (dynamic_cast<Schematic*>(*doc) == nullptr) {
        *error = tr("%1 is not a schematic.").arg(titleOf(*doc));
        return false;
    }
    return true;
}

QJsonArray QucsControl::readResource(const QString& uri, QString* error)
{
    QString kind;
    QucsDoc* doc = nullptr;
    if (!resourceOf(uri, &kind, &doc, error)) return {};
    const auto contents = [&uri](const QString& text, const QString& mime) {
        return QJsonArray{QJsonObject{{QStringLiteral("uri"), uri}, {QStringLiteral("mimeType"), mime}, {QStringLiteral("text"), text}}};
    };
    // (A tool's answer: its first text - notes may follow it.)
    const auto fromTool = [&](const QString& tool, const QJsonObject& args, const QString& mime) -> QJsonArray {
        const QJsonObject r = callNow(tool, args, 60000);
        const QString text = r.value(QLatin1String("content")).toArray().first().toObject().value(QLatin1String("text")).toString();
        if (r.value(QLatin1String("isError")).toBool()) {
            *error = text;
            return {};
        }
        return contents(text, mime);
    };
    if (kind == QLatin1String("state")) return fromTool(QStringLiteral("get_state"), {}, QStringLiteral("application/json"));
    if (kind == QLatin1String("ngspice-commands")) return fromTool(QStringLiteral("ngspice_commands"), {}, QStringLiteral("text/plain"));
    if (kind == QLatin1String("instructions")) return contents(instructions(), QStringLiteral("text/plain"));
    const QJsonObject path{{QStringLiteral("path"), doc->getDocName()}};
    if (kind == QLatin1String("schematic")) return contents(static_cast<Schematic*>(doc)->documentText(), QStringLiteral("text/x-qucs-schematic"));
    if (kind == QLatin1String("dataset")) return fromTool(QStringLiteral("get_dataset"), path, QStringLiteral("application/json"));
    // The netlist, and its map: one writing of it.
    QJsonObject withMap = path;
    withMap.insert(QStringLiteral("map"), true);
    const QJsonArray map = fromTool(QStringLiteral("get_netlist"), withMap, QStringLiteral("application/json"));
    if (map.isEmpty() || kind == QLatin1String("netlist-map")) return map;
    const QJsonObject o = QJsonDocument::fromJson(map.first().toObject().value(QLatin1String("text")).toString().toUtf8()).object();
    QStringList lines;
    for (const QJsonValue& l : o.value(QLatin1String("netlist")).toArray()) lines << l.toString();
    return contents(lines.join(QLatin1Char('\n')) + QLatin1Char('\n'), QStringLiteral("text/x-spice"));
}

QString QucsControl::resourceVersion(const QString& uri) const
{
    QString kind, error;
    QucsDoc* doc = nullptr;
    if (!resourceOf(uri, &kind, &doc, &error)) return {};
    // (During a preview, as they were before it: what it changes is put back.)
    const auto revisionOf = [this](const QucsDoc* d) { return a_revisionsKept.value(d, {d->revision(), d->getDocChanged()}); };
    if (kind == QLatin1String("state")) {
        QString v;
        for (QucsDoc* d : a_app->allDocuments())
            v += QStringLiteral("%1:%2:%3;").arg(seenKey(d)).arg(revisionOf(d).first).arg(revisionOf(d).second ? 1 : 0);
        return v;
    }
    if (kind == QLatin1String("ngspice-commands") || kind == QLatin1String("instructions")) return QStringLiteral("1");
    if (kind == QLatin1String("dataset")) return datasetWritten(doc).toString(Qt::ISODateWithMs);
    return QString::number(revisionOf(doc).first);
}

QString QucsControl::askedEachTime(const QString& tool, const QJsonObject& a) const
{
    const std::function<QString(const QString&, const QJsonObject&)> always = [&always](const QString& tool, const QJsonObject& a) {
        // A batch: as the first of its calls that is.
        if (tool == QLatin1String("batch")) {
            for (const QJsonValue& v : a.value(QLatin1String("calls")).toArray())
                if (const QString why = always(v.toObject().value(QLatin1String("tool")).toString(), v.toObject().value(QLatin1String("arguments")).toObject());
                    !why.isEmpty())
                    return why;
            return QString();
        }
        if ((tool == QLatin1String("git_remote") && a.value(QLatin1String("action")).toString() == QLatin1String("push"))
            || (tool == QLatin1String("git_commit") && a.value(QLatin1String("push")).toBool()))
            return tr("A push publishes the branch's commits on the remote, for everyone who can read it: asked about each time.");
        if (tool == QLatin1String("console") && (!a.value(QLatin1String("input")).toString().isEmpty() || a.value(QLatin1String("interrupt")).toBool()))
            return tr("What is typed into a console runs with your rights, outside Claude Code's own rules for commands: asked about each "
                      "time.");
        return QString();
    };
    QStringList why;
    if (const QString said = always(tool, a); !said.isEmpty()) why << said;
    // And what the guards the user chose name (a batch: of all its calls).
    if (const QString said = guardedSaid(guarded(tool, a)); !said.isEmpty()) why << said;
    return why.join(QLatin1Char(' '));
}

// ---- the guards: a schematic's commands, files outside the workspace

namespace {

// \a path as it is on disk: as far as it is there, its links resolved
// (canonical), the rest as named.
QString resolvedPath(const QString& path)
{
    const QString clean = QDir::cleanPath(QFileInfo(path).absoluteFilePath());
    QString rest;
    QFileInfo at(clean);
    while (!at.exists()) {
        const QString parent = at.absolutePath();
        if (parent == at.absoluteFilePath()) return clean;   // (the root)
        rest = rest.isEmpty() ? at.fileName() : at.fileName() + QLatin1Char('/') + rest;
        at = QFileInfo(parent);
    }
    const QString real = at.canonicalFilePath();
    return rest.isEmpty() ? real : QDir::cleanPath(real + QLatin1Char('/') + rest);
}

// Whether \a file is \a folder or in it.
bool within(const QString& file, const QString& folder)
{
#ifdef Q_OS_WIN
    const Qt::CaseSensitivity cs = Qt::CaseInsensitive;
#else
    const Qt::CaseSensitivity cs = Qt::CaseSensitive;
#endif
    if (folder.isEmpty()) return false;
    return file.compare(folder, cs) == 0 || file.startsWith(folder.endsWith(QLatin1Char('/')) ? folder : folder + QLatin1Char('/'), cs);
}

QStringList native(const QStringList& paths)
{
    QStringList out;
    for (const QString& p : paths) out << QDir::toNativeSeparators(p);
    return out;
}

} // namespace

void QucsControl::Guarded::add(const Guarded& other)
{
    for (const QString& c : other.commands)
        if (!commands.contains(c)) commands << c;
    for (const QString& f : other.read)
        if (!read.contains(f)) read << f;
    for (const QString& f : other.written)
        if (!written.contains(f)) written << f;
}

QucsControl::Guarded QucsControl::Guarded::beyond(const Guarded& other) const
{
    Guarded out;
    QStringList folders;
    for (const QString& f : written) folders << QFileInfo(f).absolutePath();
    for (const QString& c : other.commands)
        if (!commands.contains(c)) out.commands << c;
    for (const QString& f : other.read)
        if (!read.contains(f) && !written.contains(f)) out.read << f;
    for (const QString& f : other.written)
        if (!folders.contains(QFileInfo(f).absolutePath())) out.written << f;
    return out;
}

void QucsControl::setAsking(quint64 caller, bool asking)
{
    if (asking) a_asking.insert(caller);
    else a_asking.remove(caller);
}

QucsControl::Guarded QucsControl::guarded(const QString& tool, const QJsonObject& a) const
{
    Guarded g;
    if (tool == QLatin1String("batch")) {
        for (const QJsonValue& v : a.value(QLatin1String("calls")).toArray())
            g.add(guarded(v.toObject().value(QLatin1String("tool")).toString(), v.toObject().value(QLatin1String("arguments")).toObject()));
        return g;
    }
    if (a_guards.commands && (tool == QLatin1String("simulate") || tool == QLatin1String("tune"))) {
        QString error;
        if (Schematic* sch = schematic(a, &error, false)) {
            g.commands = commandsNamed(sch);
        } else {
            // A file not open, which simulate opens first: as it is there.
            const QString path = a.value(QLatin1String("path")).toString().trimmed();
            const QString file = path.isEmpty() ? QString() : absolute(path);
            if (!file.isEmpty() && QFileInfo(file).isFile() && QFileInfo(file).suffix().compare(QLatin1String("sch"), Qt::CaseInsensitive) == 0) {
                Schematic there(nullptr, file);
                misc::ErrorCapture quiet;
                if (there.loadDocument()) g.commands = commandsNamed(&there);
            }
        }
    }
    // The window's Simulate, Calculate DC Bias, the tuner: what Simulate
    // runs on the document ('path''s, else the one in front).
    if (a_guards.commands && tool == QLatin1String("trigger_action"))
        if (QAction* action = actionNamed(a.value(QLatin1String("action")).toString().trimmed());
            action != nullptr && (action == a_app->simulate || action == a_app->dcbias || action == a_app->tune)) {
            QString error;
            if (QucsDoc* doc = document(a, &error)) g.commands = runOf(QucsApp::documentWidget(doc));
        }
    if (a_guards.outside) filesOutside(tool, a, &g);
    return g;
}

QStringList QucsControl::runOf(QWidget* w) const
{
    QucsDoc* doc = w != nullptr ? QucsApp::docIn(w) : nullptr;
    if (doc == nullptr) return {};
    const QString file = doc->getDocName();
    const QString name = file.isEmpty() ? titleOf(doc) : QFileInfo(file).fileName();
    const QString suffix = QFileInfo(file).suffix().toLower();
    if (qobject_cast<PythonDoc*>(w) != nullptr) return {tr("Python runs the script, with your rights (%1)").arg(name)};
    if (qobject_cast<TextDoc*>(w) != nullptr) {
        if (suffix == QLatin1String("m") || suffix == QLatin1String("oct")) return {tr("Octave runs the script, with your rights (%1)").arg(name)};
        return {};
    }
    auto* sch = dynamic_cast<Schematic*>(doc);
    if (sch == nullptr) return {};
    if (suffix != QLatin1String("dpl")) return commandsNamed(sch);
    // A data display: its schematic, beside it by its name - open, or as
    // it is there.
    const QString beside = QFileInfo(file).absoluteDir().filePath(QFileInfo(file).completeBaseName() + QStringLiteral(".sch"));
    for (QucsDoc* d : a_app->allDocuments())
        if (auto* s = dynamic_cast<Schematic*>(d); s != nullptr && !d->getDocName().isEmpty() && sameFile(d->getDocName(), beside))
            return commandsNamed(s);
    if (!QFileInfo(beside).isFile()) return {};
    Schematic there(nullptr, beside);
    misc::ErrorCapture quiet;
    return there.loadDocument() ? commandsNamed(&there) : QStringList();
}

QString QucsControl::runRefused(QWidget* w) const
{
    // The calls it may be of: the one under way (its synchronous part, or
    // all of it when it runs alone - a batch, a script) and those that
    // drive the window, until they answer. (The user's own Simulate,
    // clicked meanwhile, is taken as theirs.)
    if (!a_guards.commands || a_ownRun) return {};
    bool any = false;
    Guarded seen;
    if (a_callGuard.on && (a_callDepth > 0 || a_callGuard.kept)) {
        any = true;
        seen.add(a_callGuard.seen);
    }
    for (const CallGuard& g : a_driving) {
        any = true;
        seen.add(g.seen);
    }
    if (!any) return {};
    Guarded now;
    now.commands = runOf(w);
    const Guarded unseen = seen.beyond(now);
    if (unseen.isEmpty()) return {};
    return tr("Not simulated: it runs what the user was not asked about when this call began (%1). Make the call that runs it by "
              "itself - simulate, or trigger_action of Simulation > Simulate -, and the user is asked first.")
        .arg(unseen.commands.join(QStringLiteral("; ")));
}

QStringList QucsControl::commandsNamed(Schematic* sch) const
{
    const QString name = sch->getDocName().isEmpty() ? titleOf(sch) : QFileInfo(sch->getDocName()).fileName();
    QStringList out;
    for (const QString& what : qucs_s::erc::commandsRun(sch)) out << QStringLiteral("%1 (%2)").arg(what, name);
    return out;
}

bool QucsControl::outsideWorkspace(const QString& file, bool written) const
{
    // (A link in the workspace is the user's: where it is named counts, as
    // does where it leads. A folder setting not set - a QDir made empty,
    // its path ".", or any relative one - is none: its absolutePath() is the
    // program's working folder, / when started from the Finder.)
    const auto folder = [](const QDir& d) { return d.path().isEmpty() || QDir::isRelativePath(d.path()) ? QString() : d.absolutePath(); };
    QStringList roots{folder(QucsSettings.qucsWorkspaceDir), folder(QucsSettings.QucsWorkDir), misc::scratchDir(),
                      QucsSettings.S4Qworkdir, misc::cacheDir(), folder(QucsSettings.tempFilesDir)};
    if (!written)
        roots << QucsSettings.LibDir << QucsSettings.SpiceLibDir << QucsSettings.ExamplesDir << QucsSettings.DocDir << QucsSettings.OctaveDir
              << QucsSettings.LangDir;
    const QString named = QDir::cleanPath(QFileInfo(file).absoluteFilePath()), real = resolvedPath(file);
    for (const QString& root : std::as_const(roots)) {
        if (root.trimmed().isEmpty()) continue;
        if (within(named, QDir::cleanPath(QFileInfo(root).absoluteFilePath())) || within(real, resolvedPath(root))) return false;
    }
    return true;
}

void QucsControl::filesOutside(const QString& tool, const QJsonObject& a, Guarded* g) const
{
    const auto text = [&a](const char* key) { return a.value(QLatin1String(key)).toString().trimmed(); };
    const QString action = text("action");
    // The document it acts on ('path''s, else the one in front): a relative
    // name may be taken beside it.
    QString none;
    QucsDoc* doc = document(a, &none);
    const QString beside = doc != nullptr && !doc->getDocName().isEmpty() ? QFileInfo(doc->getDocName()).absolutePath() : QString();
    const auto isOpen = [this](const QString& file) {
        for (QucsDoc* d : a_app->allDocuments())
            if (!d->getDocName().isEmpty() && sameFile(d->getDocName(), file)) return true;
        return false;
    };
    // Read: a file or folder named that is there (not a document open in
    // the window, which the user - or a call asked about - opened).
    const auto read = [&](const QString& value) {
        if (value.isEmpty()) return;
        QString error;
        if (document(QJsonObject{{QStringLiteral("path"), value}}, &error) != nullptr) return;
        QStringList candidates{absolute(value)};
        if (QFileInfo(value).isRelative() && !beside.isEmpty()) candidates << QDir::cleanPath(QDir(beside).filePath(value));
        for (const QString& c : std::as_const(candidates))
            if (QFileInfo::exists(c) && !isOpen(c) && outsideWorkspace(c, false))
                if (const QString r = resolvedPath(c); !g->read.contains(r)) g->read << r;
    };
    const auto write = [&](const QString& file) {
        if (file.isEmpty() || !outsideWorkspace(file, true)) return;
        if (const QString r = resolvedPath(file); !g->written.contains(r)) g->written << r;
    };
    for (const char* key : {"path", "file", "against", "compare", "dataset", "folder", "symbols", "library", "measured"})
        if (!(tool == QLatin1String("fem_model") && action == QLatin1String("new") && qstrcmp(key, "path") == 0)) read(text(key));
    if (tool == QLatin1String("describe_component_type")) read(text("type"));   // (a Verilog-A module's file)
    if (tool == QLatin1String("open_project")) read(text("name"));             // (a project's folder)

    // Written: where each tool writes, as it resolves it.
    const QString saveAs = text("save_as");
    if (tool == QLatin1String("save_document")) {
        write(!text("as").isEmpty() ? absolute(text("as")) : doc != nullptr ? doc->getDocName() : QString());
    } else if (tool == QLatin1String("close_document")) {
        if (text("unsaved") == QLatin1String("save") && doc != nullptr && doc->getDocChanged()) write(doc->getDocName());
    } else if (tool == QLatin1String("export_image") && isSlideExport(a)) {
        // A presentation's slides: pictures numbered after the name when
        // there are several.
        const QStringList files = slideExportFiles(a);
        if (files.isEmpty() && !saveAs.isEmpty()) write(absolute(saveAs));
        for (const QString& f : files) write(f);
    } else if (tool == QLatin1String("export_image") || tool == QLatin1String("export_netlist") || tool == QLatin1String("export_data")
               || tool == QLatin1String("get_netlist") || tool == QLatin1String("import_netlist") || tool.startsWith(QLatin1String("synthesize_"))) {
        if (!saveAs.isEmpty()) write(absolute(saveAs));
    } else if (tool == QLatin1String("create_subcircuit")) {
        // (Beside the schematic, a name or a relative path.)
        if (!saveAs.isEmpty())
            write(QFileInfo(saveAs).isAbsolute() ? QDir::cleanPath(saveAs)
                                                 : QDir::cleanPath(QFileInfo(doc != nullptr ? doc->getDocName() : QString()).absoluteDir().filePath(saveAs)));
    } else if (tool == QLatin1String("copy_document")) {
        // As copyDocument() takes 'to': a folder or project to copy into, a
        // path, or a name beside the schematic.
        const QString from = doc != nullptr ? doc->getDocName() : absolute(text("path"));
        QString to = text("to");
        if (!to.isEmpty() && !from.isEmpty()) {
            const QString asProject = QucsSettings.qucsWorkspaceDir.absoluteFilePath(qucs_s::workspace::folderFor(to));
            if (QFileInfo(absolute(to)).isDir()) to = QDir(absolute(to)).filePath(QFileInfo(from).fileName());
            else if (!to.contains(QLatin1Char('/')) && QFileInfo(asProject).isDir() && !to.endsWith(QLatin1String(".sch")))
                to = QDir(asProject).filePath(QFileInfo(from).fileName());
            else if (to.contains(QLatin1Char('/')) || to.contains(QLatin1Char('\\'))) to = absolute(to);
            else to = QFileInfo(from).absoluteDir().filePath(to);
            write(to);
        }
    } else if ((tool == QLatin1String("edit_component") && !text("rename").isEmpty()) || tool == QLatin1String("rename_net")) {
        // (The traces renamed in the data display beside it, not open:
        // renameEverywhere().)
        if (auto* sch = dynamic_cast<Schematic*>(doc); sch != nullptr && !sch->getDocName().isEmpty() && !sch->getDataDisplay().isEmpty()) {
            const QString dpl = QFileInfo(sch->getDocName()).absoluteDir().filePath(sch->getDataDisplay());
            if (QFileInfo::exists(dpl) && !isOpen(dpl)) write(dpl);
        }
    } else if (tool == QLatin1String("import_data")) {
        // (Its dataset beside the schematic - written, read again, or taken
        // away.)
        if (!beside.isEmpty()) {
            const QString name = !text("name").isEmpty() ? text("name") : QFileInfo(text("file")).completeBaseName();
            write(QDir(beside).filePath((name.isEmpty() ? QStringLiteral("imported") : name) + QStringLiteral(".dat")));
        }
    } else if (tool == QLatin1String("fem_model") || tool == QLatin1String("fem_export")) {
        for (const QString& file : femFilesWritten(tool, a)) write(file);
    }
}

QJsonObject QucsControl::guardsState() const
{
    const quint64 caller = a_callers.isEmpty() ? 0 : a_callers.last();
    const bool asked = caller == 0 || a_asking.contains(caller);
    QStringList what;
    what << (a_guards.commands
                 ? tr("commands: on - the user is asked before a simulation that runs commands besides the simulator (a System "
                      "Command part, ngspice's shell, an Octave or Python script after a run)")
                 : tr("commands: off - such a simulation runs without a question"));
    what << (a_guards.outside ? tr("files outside the workspace: on - the user is asked before a file outside the workspace and the "
                                   "open project is read or written, the file named")
                              : tr("files outside the workspace: off - such a file is read and written without a question"));
    if (!asked)
        what << tr("this conversation is not asked at all (its session runs with permissions bypassed): nothing above is put to the "
                   "user");
    return QJsonObject{{QStringLiteral("commands"), a_guards.commands},
                       {QStringLiteral("files outside the workspace"), a_guards.outside},
                       {QStringLiteral("this conversation is asked"), asked},
                       {QStringLiteral("what they watch"), what.join(QStringLiteral("; "))},
                       {QStringLiteral("note"),
                        tr("The user sets these in the Claude panel's Permissions menu. describe_tool with 'arguments' says what "
                           "they name for one call, before it is made.")}};
}

QJsonObject QucsControl::callGuarded(const QString& tool, const QJsonObject& arguments) const
{
    QJsonObject o;
    if (const QString always = askedEachTime(tool, arguments); !always.isEmpty()) {
        o.insert(QStringLiteral("treated as"), tr("asked about each time"));
        o.insert(QStringLiteral("why"), always);
    } else if (irreversible(tool, arguments)) {
        o.insert(QStringLiteral("treated as"), tr("asked about, unless Claude acts on its own"));
        o.insert(QStringLiteral("why"), tr("It cannot be undone: files are deleted or written over, or unsaved changes discarded. "
                                           "In auto mode it is let through."));
    } else if (readOnlyTools().contains(tool)) {
        o.insert(QStringLiteral("treated as"), tr("let through"));
        o.insert(QStringLiteral("why"), tr("It only looks: the guards name nothing in it."));
    } else {
        o.insert(QStringLiteral("treated as"), tr("let through once the tools are allowed"));
        o.insert(QStringLiteral("why"), tr("It changes something, and the guards name nothing in it: asked once for the tool (or "
                                           "let through where edits are allowed - acceptEdits, auto)."));
    }
    // Inside a call that runs alone (a batch, a script, tune): what was not
    // asked about when that call began is refused outright. (Not this
    // call's own guard, which is describe_tool's and names nothing.)
    if (a_callGuard.kept)
        if (const QString within = unaskedWithin(tool, arguments); !within.isEmpty())
            o.insert(QStringLiteral("refused here"), within);
    return o;
}

QString QucsControl::guardedSaid(const Guarded& g) const
{
    QStringList said;
    if (!g.commands.isEmpty())
        said << tr("Simulated, it runs commands besides the simulator, with your rights: %1.").arg(g.commands.join(QStringLiteral("; ")));
    QStringList files;
    if (!g.read.isEmpty()) files << tr("reads %1").arg(native(g.read).join(QStringLiteral(", ")));
    if (!g.written.isEmpty()) files << tr("writes %1").arg(native(g.written).join(QStringLiteral(", ")));
    if (!files.isEmpty()) said << tr("Outside the workspace and the open project, it %1.").arg(files.join(tr(" and ")));
    if (said.isEmpty()) return {};
    return said.join(QLatin1Char(' ')) + QLatin1Char(' ') + tr("Asked about each time, as the Permissions menu says.");
}

QString QucsControl::unaskedWithin(const QString& tool, const QJsonObject& arguments) const
{
    if (!a_callGuard.on) return {};
    const Guarded unseen = a_callGuard.seen.beyond(guarded(tool, arguments));
    if (unseen.isEmpty()) return {};
    return tr("%1 was not made: %2 That was not asked about when the call it is part of began - make it by itself, and the user is "
              "asked first.")
        .arg(tool, guardedSaid(unseen));
}

QString QucsControl::unaskedCommands(Schematic* sch) const
{
    if (!a_callGuard.on || !a_guards.commands) return {};
    Guarded now;
    now.commands = commandsNamed(sch);
    const Guarded unseen = a_callGuard.seen.beyond(now);
    if (unseen.isEmpty()) return {};
    return tr("%1 was not simulated: it runs commands besides the simulator that the user was not asked about when this call began "
              "(%2). Simulate it with a call of its own, and the user is asked first.")
        .arg(titleOf(sch), unseen.commands.join(QStringLiteral("; ")));
}

QString QucsControl::unaskedWrite(const QString& file) const
{
    if (!a_callGuard.on || !a_guards.outside || a_callDepth == 0 || file.isEmpty() || !outsideWorkspace(file, true)) return {};
    Guarded now;
    now.written << resolvedPath(file);
    if (a_callGuard.seen.beyond(now).isEmpty()) return {};
    return tr("%1 was not written: it is outside the workspace and the open project, and writing it was not asked about when this "
              "call began. Write it with a call of its own that names it, and the user is asked first.")
        .arg(QDir::toNativeSeparators(file));
}

bool QucsControl::irreversible(const QString& tool, const QJsonObject& a) const
{
    const auto exists = [this](const QString& path) { return !path.trimmed().isEmpty() && QFileInfo::exists(absolute(path.trimmed())); };
    if (tool == QLatin1String("clean_scratch") || tool == QLatin1String("trash_file")) return true;
    // A batch: when one of its calls is. (Taken as undoable whatever it
    // held, a trash_file in one went unasked where the host's tools were
    // allowed.)
    if (tool == QLatin1String("batch")) {
        for (const QJsonValue& v : a.value(QLatin1String("calls")).toArray())
            if (irreversible(v.toObject().value(QLatin1String("tool")).toString(), v.toObject().value(QLatin1String("arguments")).toObject()))
                return true;
        return false;
    }
    // A file written over, 'replace' says.
    if ((tool == QLatin1String("fem_model") || tool == QLatin1String("create_subcircuit") || tool == QLatin1String("import_netlist"))
        && a.value(QLatin1String("replace")).toBool())
        return true;
    // Git: changes thrown away, a branch's commits lost, a stash dropped,
    // the work published - asked about each time.
    if (tool == QLatin1String("git_discard")) return true;
    if (tool == QLatin1String("git_commit_action"))
        return a.value(QLatin1String("action")).toString() == QLatin1String("reset") && a.value(QLatin1String("mode")).toString() == QLatin1String("hard");
    if (tool == QLatin1String("git_branch"))
        return a.value(QLatin1String("action")).toString() == QLatin1String("delete") && a.value(QLatin1String("force")).toBool();
    if (tool == QLatin1String("git_stash")) return a.value(QLatin1String("action")).toString() == QLatin1String("drop");
    if (tool == QLatin1String("git_remote")) return a.value(QLatin1String("action")).toString() == QLatin1String("push");
    if (tool == QLatin1String("git_commit")) return a.value(QLatin1String("push")).toBool();
    // A side taken whole: the other's changes, an edit made in the file, gone.
    if (tool == QLatin1String("git_resolve"))
        return a.value(QLatin1String("action")).toString().startsWith(QLatin1String("take_"));
    // Raw input, the last resort: what it clicks or types is anything the
    // window does.
    if (tool == QLatin1String("send_input")) return true;
    // A line typed into a console runs, with the user's rights: asked about
    // each time (only reading it is not).
    if (tool == QLatin1String("console"))
        return !a.value(QLatin1String("input")).toString().isEmpty() || a.value(QLatin1String("interrupt")).toBool();
    // A script that names what cannot be undone.
    if (tool == QLatin1String("run_script")) {
        const QString script = a.value(QLatin1String("script")).toString();
        return script.contains(QLatin1String("clean_scratch")) || script.contains(QLatin1String("trash_file"))
               || script.contains(QLatin1String("discard"))
               || script.contains(QLatin1String("replace")) || script.contains(QLatin1String("save_as"));
    }
    if (tool == QLatin1String("close_document")) {
        if (a.value(QLatin1String("unsaved")).toString() != QLatin1String("discard")) return false;
        QString error;
        QucsDoc* doc = document(a, &error);
        return doc == nullptr || doc->getDocChanged();
    }
    if (tool == QLatin1String("copy_document")) return a.value(QLatin1String("replace")).toBool();
    if (tool == QLatin1String("export_image") && isSlideExport(a)) {
        // (Each picture of a presentation's slides written over.)
        for (const QString& f : slideExportFiles(a))
            if (QFileInfo::exists(f)) return true;
        return false;
    }
    if (tool == QLatin1String("export_image") || tool == QLatin1String("export_netlist") || tool == QLatin1String("export_data"))
        return exists(a.value(QLatin1String("save_as")).toString()) || exists(a.value(QLatin1String("file")).toString());
    if (tool == QLatin1String("get_netlist")) return exists(a.value(QLatin1String("save_as")).toString());
    if (tool == QLatin1String("save_document")) {
        const QString as = a.value(QLatin1String("as")).toString();
        // (Without 'as', 'replace' writes a schematic over its file in
        // conflict: the merge's other side gone from it.)
        return (!as.isEmpty() && exists(as)) || (as.isEmpty() && a.value(QLatin1String("replace")).toBool());
    }
    return false;
}

qint64 QucsControl::monotonicMs()
{
    static const std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
    // (From 1, never 0: 0 is "no wait under way" to the readers.)
    return 1 + std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
}

void QucsControl::setAsker(quint64 caller, Asker asker)
{
    if (asker) a_askers.insert(caller, std::move(asker));
    else a_askers.remove(caller);
}

QJsonObject QucsControl::askUser(const QString& message, const QJsonObject& schema)
{
    const quint64 caller = a_callers.isEmpty() ? 0 : a_callers.last();
    const Asker asker = a_askers.value(caller);
    if (!asker) return QJsonObject{{QStringLiteral("action"), QStringLiteral("cancel")}};
    QJsonObject answer;
    bool answered = false;
    QEventLoop loop;
    asker(message, schema, [&](const QJsonObject& result) {
        answer = result;
        answered = true;
        loop.quit();
    });
    if (!answered) {
        // The user's time, told apart from the call's own: a script's clock
        // does not count it (MCP wishlist N10 - a script that waited on a
        // card was "stopped after 60 s"). When it began is published while
        // it waits, so that the script's watchdog - another thread - sees
        // the wait as it goes and not only once it is over; a card asked
        // for within one is inside its time already.
        if (a_askedUserDepth++ == 0) a_askedUserSince.store(monotonicMs());
        ++a_spinning;
        loop.exec();
        --a_spinning;
        if (--a_askedUserDepth == 0)
            if (const qint64 since = a_askedUserSince.exchange(0); since > 0) a_askedUserMs += monotonicMs() - since;
    }
    return answer;
}

bool QucsControl::confirmed(const QString& question)
{
    const QJsonObject schema{{QStringLiteral("type"), QStringLiteral("object")},
                             {QStringLiteral("properties"),
                              QJsonObject{{QStringLiteral("confirm"), QJsonObject{{QStringLiteral("type"), QStringLiteral("boolean")},
                                                                                  {QStringLiteral("title"), tr("Yes")},
                                                                                  {QStringLiteral("description"), question}}}}},
                             {QStringLiteral("required"), QJsonArray{QStringLiteral("confirm")}}};
    const QJsonObject answer = askUser(question, schema);
    return answer.value(QLatin1String("action")).toString() == QLatin1String("accept")
           && answer.value(QLatin1String("content")).toObject().value(QLatin1String("confirm")).toBool();
}

QString QucsControl::choice(const QString& question, const QStringList& options)
{
    const QJsonObject schema{{QStringLiteral("type"), QStringLiteral("object")},
                             {QStringLiteral("properties"),
                              QJsonObject{{QStringLiteral("choice"), QJsonObject{{QStringLiteral("type"), QStringLiteral("string")},
                                                                                 {QStringLiteral("title"), question},
                                                                                 {QStringLiteral("enum"), QJsonArray::fromStringList(options)}}}}},
                             {QStringLiteral("required"), QJsonArray{QStringLiteral("choice")}}};
    const QJsonObject answer = askUser(question, schema);
    if (answer.value(QLatin1String("action")).toString() != QLatin1String("accept")) return {};
    return answer.value(QLatin1String("content")).toObject().value(QLatin1String("choice")).toString();
}

// ---- preview and diff: what a change would do, what changed

// A .sch file's text as the undo steps have a schematic (snapshot()): its
// components, wires and labels, diagrams and paintings, a section each.
QString QucsControl::stateOfText(const QString& text)
{
    QString state = QStringLiteral("*\n");
    for (const char* name : {"Components", "Wires", "Diagrams", "Paintings"}) {
        const QString open = QStringLiteral("<%1>").arg(QLatin1String(name)), close = QStringLiteral("</%1>").arg(QLatin1String(name));
        const qsizetype from = text.indexOf(open), to = text.indexOf(close);
        if (from >= 0 && to > from)
            for (const QString& line : text.mid(from + open.size(), to - from - open.size()).split(QLatin1Char('\n')))
                if (!line.trimmed().isEmpty()) state += line.trimmed() + QLatin1Char('\n');
        state += QStringLiteral("</>\n");
    }
    return state;
}

QJsonObject QucsControl::preview(const QString& tool, const QJsonObject& args, const Done& done, bool& async)
{
    // The open schematics as they are - elements, symbol, where their undo
    // stacks stand, whether changed, which view - to put back after.
    // (And its revision and latest edits: a preview is no edit - its
    // dataset is not called stale after it, nor is it told as "yours".)
    struct Kept {
        QPointer<Schematic> sch;
        QPair<QString, QString> state;
        Schematic::UndoStacks marks;
        bool changed;
        bool symbolMode;
        quint64 revision;
        QList<QucsDoc::Edit> edits;
        Selected selected;
    };
    auto kept = std::make_shared<QList<Kept>>();
    for (QucsDoc* doc : a_app->allDocuments())
        if (auto* sch = dynamic_cast<Schematic*>(doc))
            kept->append(Kept{sch, sch->snapshotAll(), sch->undoStacks(), sch->getDocChanged(), sch->getSymbolMode(), sch->revision(),
                              sch->recentEdits(), Selected::of(sch)});
    // What a subscriber reads meanwhile (a batch previewed runs over
    // several turns of the event loop): the revisions as they were.
    if (a_previewing == 0)
        for (const Kept& k : std::as_const(*kept)) a_revisionsKept.insert(k.sch.data(), {k.revision, k.changed});
    QPointer<QWidget> front = a_app->DocumentTab->currentWidget();
    // The documents open: one the call opens - a data display it makes for
    // add_diagram's 'document' - is closed after, and its file, made for it,
    // taken away.
    auto openBefore = std::make_shared<QSet<QucsDoc*>>();
    for (QucsDoc* doc : a_app->allDocuments()) openBefore->insert(doc);
    // A batch: of calls that change schematics or look alone - not one that
    // writes a file, runs a simulation or opens a document.
    if (tool == QLatin1String("batch"))
        for (const QJsonValue& v : args.value(QLatin1String("calls")).toArray()) {
            const QString inner = v.toObject().value(QLatin1String("tool")).toString();
            if (!previewTools().contains(inner) && !readOnlyTools().contains(inner))
                return errorResult(tr("A batch previewed holds only calls that change schematics or only look: %1 does more (files, "
                                      "simulations, documents). Nothing was run.")
                                       .arg(inner));
        }
    // What it did, told; all put back.
    ++a_previewing;
    const auto conclude = [this, kept, front, openBefore](const QJsonObject& answer) {
        // Files written: as they were.
        QJsonArray files;
        if (--a_previewing == 0) {
            for (const auto& [file, before] : std::as_const(a_previewFiles)) {
                files.append(QDir::toNativeSeparators(file));
                if (!before) {
                    QFile::remove(file);
                    continue;
                }
                QFile out(file);
                if (out.open(QIODevice::WriteOnly | QIODevice::Truncate)) out.write(*before);
            }
            a_previewFiles.clear();
        }
        QJsonArray changes;
        for (const Kept& k : std::as_const(*kept)) {
            if (!k.sch) continue;
            const QPair<QString, QString> after = k.sch->snapshotAll();
            if (after != k.state) {
                QJsonArray lines = QJsonArray::fromStringList(describeChanges(k.state.first, after.first, 60));
                if (after.second.mid(1) != k.state.second.mid(1)) lines.append(tr("its symbol's paintings change"));
                if (!lines.isEmpty()) changes.append(QJsonObject{{QStringLiteral("document"), titleOf(k.sch)}, {QStringLiteral("changes"), lines}});
                k.sch->restoreAll(k.state, false);
                k.sch->setUndoStacks(k.marks);
                k.selected.apply(k.sch);
                k.sch->viewport()->update();
            }
            if (k.sch->getSymbolMode() != k.symbolMode) {
                a_app->showDocument(k.sch);
                QMetaObject::invokeMethod(a_app, "slotSymbolEdit", Qt::DirectConnection);
            }
            k.sch->setChanged(k.changed, false);
            k.sch->rewind(k.revision, k.edits);
        }
        // What the call opened: said, and closed as it was not (its file,
        // if made for it, is gone with the files above).
        if (a_previewing == 0)
            for (QucsDoc* doc : a_app->allDocuments()) {
                if (openBefore->contains(doc)) continue;
                QJsonArray lines;
                const bool made = !doc->getDocName().isEmpty() && !QFileInfo::exists(doc->getDocName());
                lines.append(made ? tr("made (it had none), and opened") : tr("opened"));
                if (auto* sch = dynamic_cast<Schematic*>(doc))
                    for (const QString& line : describeChanges(stateOfText(QString()), sch->snapshotAll().first, 60)) lines.append(line);
                changes.append(QJsonObject{{QStringLiteral("document"), titleOf(doc)}, {QStringLiteral("changes"), lines}});
                if (auto* sch = dynamic_cast<Schematic*>(doc)) sch->setChanged(false);
                doc->setDocChanged(false);
                QWidget* w = QucsApp::documentWidget(doc);
                a_app->showDocument(w);
                a_app->slotFileClose(a_app->DocumentTab->indexOf(w));
            }
        if (a_previewing == 0) a_revisionsKept.clear();
        if (front && a_app->DocumentTab->indexOf(front) >= 0) a_app->showDocument(front);
        a_callNotes.clear();   // (what the tool would have said beside it: part of the preview's answer)
        QJsonObject result{{QStringLiteral("preview"), true},
                           {QStringLiteral("changed"), false},
                           {QStringLiteral("would change"), changes},
                           {QStringLiteral("note"), changes.isEmpty() ? tr("It would change nothing on the schematics (or it failed: see its answer).")
                                                                      : tr("Nothing was changed: call it again without 'preview' to make the change.")}};
        const QString said = textOf(answer);
        const QJsonDocument parsed = QJsonDocument::fromJson(said.toUtf8());
        result.insert(QStringLiteral("its answer"), parsed.isObject() ? QJsonValue(parsed.object()) : QJsonValue(said));
        if (answer.value(QLatin1String("isError")).toBool()) result.insert(QStringLiteral("it would fail"), true);
        if (!files.isEmpty()) result.insert(QStringLiteral("files it would write"), files);
        return jsonResult(result);
    };
    QJsonObject stripped = args;
    stripped.remove(QStringLiteral("preview"));
    bool innerAsync = false;
    const QJsonObject answer = call(tool, stripped, [done, conclude](const QJsonObject& r) { done(conclude(r)); }, innerAsync);
    if (innerAsync) {   // (batch: told when it is over)
        async = true;
        return {};
    }
    return conclude(answer);
}

QString QucsControl::aboutToWrite(const QString& file)
{
    if (const QString why = qucs_s::projectlibraries::notToWrite(file); !why.isEmpty()) return why;
    if (a_previewing > 0 || a_callDepth == 0 || file.isEmpty()) return {};
    if (const QString no = unaskedWrite(file); !no.isEmpty()) return no;
    const QString path = QFileInfo(file).absoluteFilePath();
    for (const auto& kept : std::as_const(a_openStep.before))
        if (kept.first == path) return {};   // (as it was when the call began)
    std::optional<QByteArray> held;
    if (QFile f(path); f.exists() && f.open(QIODevice::ReadOnly)) held = f.readAll();
    a_openStep.before.append({path, held});
    return {};
}

void QucsControl::movedFile(const QString& from, const QString& to)
{
    if (a_previewing > 0 || a_callDepth == 0 || from.isEmpty() || to.isEmpty()) return;
    a_openStep.moved.append({QFileInfo(from).absoluteFilePath(), QFileInfo(to).absoluteFilePath()});
}

void QucsControl::openFileStep(const QString& tool)
{
    a_openStep = FileStep{tool, QDateTime::currentDateTime(), {}, {}, {}};
}

void QucsControl::closeFileStep()
{
    FileStep step = std::exchange(a_openStep, FileStep{});
    // Each file as the call left it; one it left as it was is no step.
    QList<QPair<QString, std::optional<QByteArray>>> changed;
    for (const auto& [file, before] : std::as_const(step.before)) {
        QFile f(file);
        const bool there = f.exists() && f.open(QIODevice::ReadOnly);
        const QByteArray now = there ? f.readAll() : QByteArray();
        if ((!there && !before) || (there && before && *before == now)) continue;
        changed.append({file, before});
        step.after.insert(file, there ? QCryptographicHash::hash(now, QCryptographicHash::Sha1) : QByteArray());
    }
    if (changed.isEmpty() && step.moved.isEmpty()) return;
    step.before = changed;
    a_fileSteps.append(step);
    while (a_fileSteps.size() > 50) a_fileSteps.removeFirst();
}

QJsonObject QucsControl::undoFiles(int steps)
{
    if (a_fileSteps.isEmpty())
        return errorResult(tr("No file written by a tool is kept to put back (save_document, create_subcircuit, copy_document, "
                              "import_netlist, import_data, export_data, export_netlist, export_image, rename_net's data display, "
                              "rename_file and trash_file are)."));
    QStringList restored, removed, skipped, reload, movedBack;
    QJsonArray undone;
    for (int n = 0; n < steps && !a_fileSteps.isEmpty(); ++n) {
        const FileStep step = a_fileSteps.takeLast();
        QJsonArray files;
        // Moved (renamed, to the trash): moved back, the last first - not
        // over one there now, nor what is no longer where it went.
        for (auto it = step.moved.crbegin(); it != step.moved.crend(); ++it) {
            const auto& [from, to] = *it;
            const QString shown = QDir::toNativeSeparators(from);
            if (QFileInfo::exists(from) || QFileInfo(from).isSymLink()) {
                skipped << tr("%1 (there is one there again)").arg(shown);
                continue;
            }
            if (!QFileInfo::exists(to) && !QFileInfo(to).isSymLink()) {
                skipped << tr("%1 (no longer at %2)").arg(shown, QDir::toNativeSeparators(to));
                continue;
            }
            if (!QFileInfo(QFileInfo(from).absolutePath()).isDir() || !QDir().rename(to, from)) {
                skipped << (step.tool == QLatin1String("trash_file")
                                ? tr("%1 (it could not be taken out of the trash, at %2: Finder's Put Back puts it back)")
                                      .arg(shown, QDir::toNativeSeparators(to))
                                : tr("%1 (it could not be moved back from %2)").arg(shown, QDir::toNativeSeparators(to)));
                continue;
            }
            // The documents open from it follow it back.
            a_app->documentsMoved({to}, {from});
            movedBack << tr("%1 (from %2)").arg(shown, QDir::toNativeSeparators(to));
            files.append(shown);
        }
        for (const auto& [file, before] : step.before) {
            // Not over what was done to it since (by hand, or another call).
            QFile f(file);
            const bool there = f.exists() && f.open(QIODevice::ReadOnly);
            const QByteArray now = there ? QCryptographicHash::hash(f.readAll(), QCryptographicHash::Sha1) : QByteArray();
            f.close();
            if (now != step.after.value(file)) {
                skipped << tr("%1 (changed since %2)").arg(QDir::toNativeSeparators(file), step.tool);
                continue;
            }
            if (!before) {
                if (QFile::remove(file)) removed << QDir::toNativeSeparators(file);
                else skipped << tr("%1 (could not be removed)").arg(QDir::toNativeSeparators(file));
            } else {
                QFile out(file);
                if (out.open(QIODevice::WriteOnly | QIODevice::Truncate) && out.write(*before) == before->size()) {
                    restored << QDir::toNativeSeparators(file);
                    reload << file;
                } else {
                    skipped << tr("%1 (could not be written)").arg(QDir::toNativeSeparators(file));
                }
            }
            files.append(QDir::toNativeSeparators(file));
        }
        undone.append(QJsonObject{{QStringLiteral("tool"), step.tool}, {QStringLiteral("at"), step.when.toString(Qt::ISODate)},
                                  {QStringLiteral("files"), files}});
    }
    // The documents open on them, without unsaved changes: loaded again.
    if (!reload.isEmpty()) a_app->reloadChangedFiles(reload);
    QJsonObject result{{QStringLiteral("undone"), undone}};
    if (!restored.isEmpty()) result.insert(QStringLiteral("put back"), QJsonArray::fromStringList(restored));
    if (!movedBack.isEmpty()) {
        result.insert(QStringLiteral("moved back"), QJsonArray::fromStringList(movedBack));
        if (a_app->projectView() != nullptr) a_app->projectView()->refresh();
    }
    if (!removed.isEmpty()) result.insert(QStringLiteral("removed (the call made them)"), QJsonArray::fromStringList(removed));
    if (!skipped.isEmpty()) result.insert(QStringLiteral("not put back"), QJsonArray::fromStringList(skipped));
    result.insert(QStringLiteral("note"), tr("Files are not redone. A document open on a file put back is loaded again, unless it has "
                                            "unsaved changes; one trash_file closed is not opened again (open_document opens it). A "
                                            "schematic's own changes are undone with undo without 'files'."));
    return jsonResult(result);
}

void QucsControl::written(const QString& file, const std::optional<QByteArray>& before)
{
    if (a_previewing == 0) return;
    for (const auto& kept : std::as_const(a_previewFiles))
        if (kept.first == file) return;   // (as it was first)
    a_previewFiles.append({file, before});
}

QJsonObject QucsControl::diffTool(const QJsonObject& args)
{
    QString error;
    Schematic* sch = schematic(args, &error, false);
    if (sch == nullptr) return errorResult(error);
    const QString now = sch->snapshot();
    QString before, against;
    if (args.contains(QLatin1String("steps"))) {
        const int steps = args.value(QLatin1String("steps")).toInt();
        // (steps checked first: undoIndex() - INT_MIN overflows)
        // (None back: said so - "1 to 0" read as a range.)
        if (sch->undoIndex() <= 0)
            return errorResult(tr("%1 has no step to go back to: it is at the start of its undo history (redo steps come after). "
                                  "diff without 'steps' compares it with its file.").arg(titleOf(sch)));
        if (steps < 1 || steps > sch->undoIndex() || sch->undoIndex() - steps >= sch->undoCount())
            return errorResult(tr("'steps' is how many steps back in its undo history: 1 to %1.").arg(sch->undoIndex()));
        // (The one state compared, unpacked: all of them took 0.4 s at
        // 10,000 parts.)
        before = sch->undoState(sch->undoIndex() - steps);
        against = tr("%1 step(s) back").arg(steps);
    } else {
        // Another file (or document), or its own file as saved.
        const QString other = args.value(QLatin1String("against")).toString().trimmed();
        if (!other.isEmpty()) {
            QString openError;
            QucsDoc* doc = document(QJsonObject{{QStringLiteral("path"), other}}, &openError);
            // (Its own file: as saved, not itself.)
            if (auto* otherSch = dynamic_cast<Schematic*>(doc); otherSch != nullptr && otherSch != sch) {
                before = otherSch->snapshot();
                against = titleOf(otherSch);
            } else {
                QFile f(absolute(other));
                if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) return errorResult(tr("There is no file %1.").arg(QDir::toNativeSeparators(absolute(other))));
                const QString text = QString::fromUtf8(f.readAll());
                // A schematic's file (a symbol's too), not any file read as
                // an empty schematic with every part "added".
                if (!text.startsWith(QLatin1String("<Qucs Schematic")) && !text.startsWith(QLatin1String("<Qucs Symbol")))
                    return errorResult(tr("%1 is not a schematic of Qucs-S.").arg(QDir::toNativeSeparators(absolute(other))));
                before = stateOfText(text);
                against = QDir::toNativeSeparators(absolute(other));
            }
        } else {
            if (sch->getDocName().isEmpty()) return errorResult(tr("%1 has no file: give 'steps' or 'against'.").arg(titleOf(sch)));
            QFile f(sch->getDocName());
            if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) return errorResult(tr("%1 cannot be read.").arg(QDir::toNativeSeparators(sch->getDocName())));
            before = stateOfText(QString::fromUtf8(f.readAll()));
            against = tr("its file as saved");
        }
    }
    const QStringList changes = describeChanges(before, now, 200);
    return jsonResult(QJsonObject{{QStringLiteral("document"), titleOf(sch)},
                                  {QStringLiteral("against"), against},
                                  {QStringLiteral("changes"), QJsonArray::fromStringList(changes)},
                                  {QStringLiteral("same"), changes.isEmpty()}});
}
