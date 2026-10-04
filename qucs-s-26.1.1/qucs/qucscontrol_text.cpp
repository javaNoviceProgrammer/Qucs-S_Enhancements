/*
 * qucscontrol_text.cpp - Claude's tools for text documents (.cir, .va,
 *                        .m, .py, .txt, ...): a tab's text read with its
 *                        unsaved edits, edited as one undo step, a line
 *                        shown
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
#include "textdoc.h"

#include <QJsonArray>
#include <QTextBlock>
#include <QTextCursor>
#include <QTextDocument>

#include <algorithm>
#include <cmath>

using namespace qucs_s::control;

namespace {

// A place in the text as a line and a column, both from 1.
QJsonObject placeOf(const QTextDocument* document, int position)
{
    const QTextBlock block = document->findBlock(position);
    return {{QStringLiteral("line"), block.blockNumber() + 1}, {QStringLiteral("column"), position - block.position() + 1}};
}

// Where line \a line (from 1) begins in \a text; text.size() past its end.
qsizetype lineStart(const QString& text, int line)
{
    qsizetype at = 0;
    for (int n = 1; n < line; ++n) {
        const qsizetype next = text.indexOf(QLatin1Char('\n'), at);
        if (next < 0) return text.size();
        at = next + 1;
    }
    return at;
}

// Where line \a line ends, before its newline.
qsizetype lineEnd(const QString& text, int line)
{
    const qsizetype start = lineStart(text, line);
    const qsizetype next = text.indexOf(QLatin1Char('\n'), start);
    return next < 0 ? text.size() : next;
}

} // namespace

TextDoc* QucsControl::textDocument(const QJsonObject& args, QString* error) const
{
    QucsDoc* doc = document(args, error);
    if (doc == nullptr) return nullptr;
    auto* text = dynamic_cast<TextDoc*>(doc);
    if (text == nullptr) *error = tr("%1 is no text document (get_schematic reads a schematic).").arg(titleOf(doc));
    return text;
}

QJsonObject QucsControl::getText(const QJsonObject& args)
{
    QString error;
    TextDoc* text = textDocument(args, &error);
    if (text == nullptr) return errorResult(error);
    const QStringList lines = text->toPlainText().split(QLatin1Char('\n'));
    const int count = int(lines.size());
    const int from = args.contains(QLatin1String("from_line")) ? args.value(QLatin1String("from_line")).toInt() : 1;
    int to = args.contains(QLatin1String("to_line")) ? args.value(QLatin1String("to_line")).toInt() : count;
    if (from < 1 || from > count)
        return errorResult(tr("'from_line' %1 is not a line of %2, which has %3 (from 1).").arg(from).arg(titleOf(text)).arg(count));
    if (to < from) return errorResult(tr("'to_line' %1 is before 'from_line' %2.").arg(to).arg(from));
    to = std::min(to, count);
    // Each line with its number, as an editor's margin shows it.
    const int width = int(QString::number(to).size());
    QStringList shown;
    for (int n = from; n <= to; ++n) shown << QStringLiteral("%1| %2").arg(n, width).arg(lines.at(n - 1));
    QJsonObject result{{QStringLiteral("path"), QDir::toNativeSeparators(text->getDocName())},
                       {QStringLiteral("lines"), count},
                       {QStringLiteral("revision"), double(text->revision())},
                       {QStringLiteral("unsaved"), text->getDocChanged()},
                       {QStringLiteral("text"), shown.join(QLatin1Char('\n'))}};
    if (from > 1 || to < count) result.insert(QStringLiteral("shown"), tr("lines %1 to %2 of %3").arg(from).arg(to).arg(count));
    if (text->isReadOnly()) result.insert(QStringLiteral("read only"), true);
    const QTextCursor cursor = text->textCursor();
    result.insert(QStringLiteral("cursor"), placeOf(text->document(), cursor.position()));
    if (cursor.hasSelection()) {
        QString selected = cursor.selectedText();
        selected.replace(QChar(QChar::ParagraphSeparator), QLatin1Char('\n'));
        result.insert(QStringLiteral("selection"), QJsonObject{{QStringLiteral("from"), placeOf(text->document(), cursor.selectionStart())},
                                                               {QStringLiteral("to"), placeOf(text->document(), cursor.selectionEnd())},
                                                               {QStringLiteral("text"), selected}});
    }
    // What is marked in it (build_verilog_a's errors and warnings).
    QJsonArray marks;
    for (const TextDoc::Diagnostic& d : text->diagnostics())
        marks.append(QJsonObject{{QStringLiteral("line"), d.line}, {QStringLiteral("column"), d.column},
                                 {QStringLiteral("severity"), d.error ? QStringLiteral("error") : QStringLiteral("warning")},
                                 {QStringLiteral("message"), d.message}});
    if (!marks.isEmpty()) result.insert(QStringLiteral("marks"), marks);
    return jsonResult(result);
}

QJsonObject QucsControl::editText(const QJsonObject& args)
{
    QString error;
    TextDoc* text = textDocument(args, &error);
    if (text == nullptr) return errorResult(error);
    if (const QString library = text->libraryOrigin(); !library.isEmpty())
        return errorResult(tr("%1 is the Verilog-A of a device of the library %2, linked into the project read-only: it is "
                              "changed in the library, which every project using it shares (its tooltip in the Content "
                              "panel names the file). Nothing was changed.").arg(titleOf(text), library));
    if (text->isReadOnly()) return errorResult(tr("%1 is read-only.").arg(titleOf(text)));
    if (args.contains(QLatin1String("revision"))) {
        // (A whole number from 0, as get_text gives it - -1 or 1e308 made
        // into an unsigned one was undefined.)
        const QJsonValue value = args.value(QLatin1String("revision"));
        if (!value.isDouble() || value.toDouble() < 0 || value.toDouble() > 9007199254740992.0 || value.toDouble() != std::floor(value.toDouble()))
            return errorResult(tr("'revision' is the revision get_text gave, a whole number from 0. Nothing was changed."));
        const auto given = quint64(value.toDouble());
        if (given != text->revision())
            return errorResult(tr("%1 is at revision %2, not %3: it was edited since (the user typed, or another call). Nothing was "
                                  "changed - get_text reads it as it is now.")
                                   .arg(titleOf(text)).arg(text->revision()).arg(given));
    }
    const QJsonArray edits = args.value(QLatin1String("edits")).toArray();
    if (edits.isEmpty())
        return errorResult(tr("'edits' lists the changes: [{\"find\": ..., \"replace\": ...}] or [{\"lines\": [a, b], \"text\": ...}]."));
    // Worked out on a copy first - each edit on the text the ones before it
    // left -, so that all of them are made or none.
    struct Change {
        qsizetype at;
        qsizetype length;
        QString with;
    };
    QList<Change> changes;
    QString work = text->toPlainText();
    for (qsizetype i = 0; i < edits.size(); ++i) {
        const QJsonObject e = edits.at(i).toObject();
        const QString which = tr("edit %1").arg(i + 1);
        if (e.contains(QLatin1String("find"))) {
            const QString find = e.value(QLatin1String("find")).toString();
            if (find.isEmpty()) return errorResult(tr("%1: 'find' is empty. Nothing was changed.").arg(which));
            if (!e.value(QLatin1String("replace")).isString())
                return errorResult(tr("%1: 'replace' is the text put in its place (\"\" takes it away). Nothing was changed.").arg(which));
            const QString replace = e.value(QLatin1String("replace")).toString();
            QList<qsizetype> found;
            for (qsizetype at = work.indexOf(find); at >= 0; at = work.indexOf(find, at + find.size())) found << at;
            if (found.isEmpty())
                return errorResult(tr("%1: \"%2\" is not in the text (as the edits before it left it). Nothing was changed.")
                                       .arg(which, find.left(80)));
            if (found.size() > 1 && !e.value(QLatin1String("all")).toBool())
                return errorResult(tr("%1: \"%2\" is there %3 times: give more of the text around it, or 'all': true. Nothing was "
                                      "changed.").arg(which, find.left(80)).arg(found.size()));
            // From the last: the places before it stay where they are.
            for (auto it = found.crbegin(); it != found.crend(); ++it) {
                changes.append({*it, find.size(), replace});
                work.replace(*it, find.size(), replace);
            }
        } else if (e.contains(QLatin1String("lines"))) {
            const QJsonArray range = e.value(QLatin1String("lines")).toArray();
            if (range.size() != 2 || !range.at(0).isDouble() || !range.at(1).isDouble() || !e.value(QLatin1String("text")).isString())
                return errorResult(tr("%1: 'lines' is [first, last] and 'text' the lines put in their place (\"\" takes them away; "
                                      "[n, n - 1] puts 'text' before line n). Nothing was changed.").arg(which));
            const int a = range.at(0).toInt(), b = range.at(1).toInt();
            const int count = int(work.count(QLatin1Char('\n'))) + 1;
            QString with = e.value(QLatin1String("text")).toString();
            if (with.endsWith(QLatin1Char('\n'))) with.chop(1);   // (the lines, not a line more)
            if (a < 1 || a > count + 1 || b < a - 1 || b > count)
                return errorResult(tr("%1: lines [%2, %3] are not lines of the text, which has %4 (from 1). Nothing was changed.")
                                       .arg(which).arg(a).arg(b).arg(count));
            qsizetype at = 0, length = 0;
            if (b == a - 1) {
                // Put before line a (after the last line: at the end).
                if (a == count + 1) {
                    at = work.size();
                    with.prepend(QLatin1Char('\n'));
                } else {
                    at = lineStart(work, a);
                    with.append(QLatin1Char('\n'));
                }
            } else if (with.isEmpty()) {
                // Taken away, with a newline: no empty line stays.
                at = lineStart(work, a);
                qsizetype end = lineEnd(work, b);
                if (end < work.size()) ++end;   // the newline after line b
                else if (at > 0) --at;          // the last line's: the one before it
                length = end - at;
            } else {
                at = lineStart(work, a);
                length = lineEnd(work, b) - at;
            }
            changes.append({at, length, with});
            work.replace(at, length, with);
        } else {
            return errorResult(tr("%1: give 'find' and 'replace', or 'lines' and 'text'. Nothing was changed.").arg(which));
        }
    }
    // Made: one undo step in the tab's own history.
    QTextCursor cursor(text->document());
    cursor.beginEditBlock();
    for (const Change& c : std::as_const(changes)) {
        cursor.setPosition(int(c.at));
        cursor.setPosition(int(c.at + c.length), QTextCursor::KeepAnchor);
        cursor.insertText(c.with);
    }
    cursor.endEditBlock();
    if (text->toPlainText() != work) {
        text->document()->undo();
        return errorResult(tr("The edits came out otherwise in %1 than worked out, and were taken back.").arg(titleOf(text)));
    }
    return jsonResult(QJsonObject{{QStringLiteral("path"), QDir::toNativeSeparators(text->getDocName())},
                                  {QStringLiteral("changed"), int(changes.size())},
                                  {QStringLiteral("revision"), double(text->revision())},
                                  {QStringLiteral("lines"), int(work.count(QLatin1Char('\n'))) + 1},
                                  {QStringLiteral("unsaved"), text->getDocChanged()},
                                  {QStringLiteral("note"), tr("One undo step in the tab (Edit > Undo takes it back); save_document saves "
                                                              "it.")}});
}

QJsonObject QucsControl::gotoLine(const QJsonObject& args)
{
    QString error;
    TextDoc* text = textDocument(args, &error);
    if (text == nullptr) return errorResult(error);
    const int line = args.value(QLatin1String("line")).toInt();
    const int count = text->document()->blockCount();
    if (line < 1 || line > count) return errorResult(tr("Line %1 is not in %2, which has %3 (from 1).").arg(line).arg(titleOf(text)).arg(count));
    const QTextBlock block = text->document()->findBlockByNumber(line - 1);
    const int column = std::clamp(args.value(QLatin1String("column")).toInt(1), 1, block.length());
    // In front, the line in the middle - the keyboard left where it is.
    a_app->showDocument(QucsApp::documentWidget(text));
    QTextCursor cursor(block);
    cursor.setPosition(block.position() + column - 1);
    text->setTextCursor(cursor);
    text->centerCursor();
    return jsonResult(QJsonObject{{QStringLiteral("path"), QDir::toNativeSeparators(text->getDocName())},
                                  {QStringLiteral("line"), line},
                                  {QStringLiteral("column"), column},
                                  {QStringLiteral("text"), block.text()}});
}
