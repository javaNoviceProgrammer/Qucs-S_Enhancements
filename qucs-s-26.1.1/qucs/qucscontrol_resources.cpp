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
#include "qucscontrol_p.h"

#include "qucs.h"
#include "qucsdoc.h"
#include "schematic.h"

#include <QDir>
#include <QFile>
#include <QPointer>
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

} // namespace

QJsonArray QucsControl::resources() const
{
    QJsonArray list{QJsonObject{{QStringLiteral("uri"), kPrefix + QStringLiteral("state")},
                                {QStringLiteral("name"), QStringLiteral("state")},
                                {QStringLiteral("title"), tr("The Qucs-S window")},
                                {QStringLiteral("description"), tr("What get_state tells: the documents open, their revisions, the panes, the simulator")},
                                {QStringLiteral("mimeType"), QStringLiteral("application/json")}}};
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
    if (uri == kPrefix + QStringLiteral("state")) {
        *kind = QStringLiteral("state");
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
    if (kind == QLatin1String("state")) {
        QString v;
        for (QucsDoc* d : a_app->allDocuments())
            v += QStringLiteral("%1:%2:%3;").arg(seenKey(d)).arg(d->revision()).arg(d->getDocChanged() ? 1 : 0);
        return v;
    }
    if (kind == QLatin1String("dataset")) return datasetWritten(doc).toString(Qt::ISODateWithMs);
    return QString::number(doc->revision());
}

bool QucsControl::irreversible(const QString& tool, const QJsonObject& a) const
{
    const auto exists = [this](const QString& path) { return !path.trimmed().isEmpty() && QFileInfo::exists(absolute(path.trimmed())); };
    if (tool == QLatin1String("clean_scratch")) return true;
    // A script that names what cannot be undone.
    if (tool == QLatin1String("run_script")) {
        const QString script = a.value(QLatin1String("script")).toString();
        return script.contains(QLatin1String("clean_scratch")) || script.contains(QLatin1String("discard"))
               || script.contains(QLatin1String("replace")) || script.contains(QLatin1String("save_as"));
    }
    if (tool == QLatin1String("close_document")) {
        if (a.value(QLatin1String("unsaved")).toString() != QLatin1String("discard")) return false;
        QString error;
        QucsDoc* doc = document(a, &error);
        return doc == nullptr || doc->getDocChanged();
    }
    if (tool == QLatin1String("copy_document")) return a.value(QLatin1String("replace")).toBool();
    if (tool == QLatin1String("export_image") || tool == QLatin1String("export_netlist"))
        return exists(a.value(QLatin1String("save_as")).toString()) || exists(a.value(QLatin1String("file")).toString());
    if (tool == QLatin1String("get_netlist")) return exists(a.value(QLatin1String("save_as")).toString());
    if (tool == QLatin1String("save_document")) {
        const QString as = a.value(QLatin1String("as")).toString();
        return !as.isEmpty() && exists(as);
    }
    return false;
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
    if (!answered) loop.exec();
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
    struct Kept {
        QPointer<Schematic> sch;
        QPair<QString, QString> state;
        QPair<int, int> marks;
        bool changed;
        bool symbolMode;
    };
    auto kept = std::make_shared<QList<Kept>>();
    for (QucsDoc* doc : a_app->allDocuments())
        if (auto* sch = dynamic_cast<Schematic*>(doc))
            kept->append(Kept{sch, sch->snapshotAll(), sch->undoMarks(), sch->getDocChanged(), sch->getSymbolMode()});
    QPointer<QWidget> front = a_app->DocumentTab->currentWidget();
    // What it did, told; all put back.
    const auto conclude = [this, kept, front](const QJsonObject& answer) {
        QJsonArray changes;
        for (const Kept& k : std::as_const(*kept)) {
            if (!k.sch) continue;
            const QPair<QString, QString> after = k.sch->snapshotAll();
            if (after != k.state) {
                QJsonArray lines = QJsonArray::fromStringList(describeChanges(k.state.first, after.first, 60));
                if (after.second.mid(1) != k.state.second.mid(1)) lines.append(tr("its symbol's paintings change"));
                if (!lines.isEmpty()) changes.append(QJsonObject{{QStringLiteral("document"), titleOf(k.sch)}, {QStringLiteral("changes"), lines}});
                k.sch->restoreAll(k.state, false);
                k.sch->forgetUndoAfter(k.marks);
            }
            if (k.sch->getSymbolMode() != k.symbolMode) {
                a_app->showDocument(k.sch);
                QMetaObject::invokeMethod(a_app, "slotSymbolEdit", Qt::DirectConnection);
            }
            k.sch->setChanged(k.changed, false);
        }
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

QJsonObject QucsControl::diffTool(const QJsonObject& args)
{
    QString error;
    Schematic* sch = schematic(args, &error, false);
    if (sch == nullptr) return errorResult(error);
    const QString now = sch->snapshot();
    QString before, against;
    if (args.contains(QLatin1String("steps"))) {
        const int steps = args.value(QLatin1String("steps")).toInt();
        const QStringList states = sch->undoStates();
        const int at = sch->undoIndex() - steps;
        if (steps < 1 || at < 0 || at >= states.size())
            return errorResult(tr("'steps' is how many steps back in its undo history: 1 to %1.").arg(std::max(0, sch->undoIndex())));
        before = states.at(at);
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
                before = stateOfText(QString::fromUtf8(f.readAll()));
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
