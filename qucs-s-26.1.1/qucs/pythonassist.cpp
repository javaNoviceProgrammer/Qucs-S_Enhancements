/*
 * pythonassist.cpp - the Python editor's help with a script (pythondoc.h):
 *                    the call's signature above it, what a name is, where
 *                    it is defined, the outline, the cells, Format Document
 *                    and Fix Problems, the breakpoints and the debugger's
 *                    line
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#include "pythondoc.h"

#include "pythonviews.h"
#include "qucs.h"
#include "settings.h"

#include <QAbstractItemView>
#include <QApplication>
#include <QComboBox>
#include <QDateTime>
#include <QCompleter>
#include <QDir>
#include <QFileInfo>
#include <QHelpEvent>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QLabel>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPixmap>
#include <QProcess>
#include <QScreen>
#include <QSet>
#include <QTextBlock>
#include <QTimer>
#include <QToolTip>

#include <algorithm>

namespace {

QString escaped(const QString& text) { return text.toHtmlEscaped(); }

// The outline's letters: their size, and how far a level sets one in.
constexpr int kOutlineBadge = 16;
constexpr int kOutlineStep = 14;

// The colours of the debugger's marks.
const QColor kBreakpoint(0xd0, 0x38, 0x2b);
const QColor kStopped(0xf2, 0xb7, 0x05);    // the line it stopped at
const QColor kLookedAt(0x3f, 0xa3, 0x4d);   // a frame further out, looked at

} // namespace

// ----------------------------------------------------------------------
// The call's signature

bool PythonDoc::signatureShown() const
{
    return a_signatureTip != nullptr && a_signatureTip->isVisible();
}

void PythonDoc::showSignature(bool asked)
{
    a_signatureDelay->stop();
    const QTextCursor cursor = textCursor();
    if (!asked && !signatureShown()) {
        // As one types: not in a string or a comment.
        const QString before = cursor.block().text().left(cursor.positionInBlock());
        if (qucs_s::python::inStringOrComment(before)) return;
    }
    const int id = ask(QStringLiteral("signature"), cursor.blockNumber() + 1, cursor.positionInBlock());
    if (id == 0) return;
    a_signatureRequest = id;
    a_signatureRevision = document()->revision();
}

void PythonDoc::answerSignature(const qucs_s::python::Answer& answer)
{
    if (!answer.engine.isEmpty()) a_completionEngine = answer.engine;
    a_signature = answer.signature;
    // Shown while the text is as it was asked about, the keyboard here.
    const QWidget* focus = QApplication::focusWidget();
    const bool elsewhere = !isVisible() || (focus != nullptr && focus != this && focus != a_completer->popup());
    // (Shown though the text changed since: asked again as the cursor moves.)
    if (!a_signature.valid || a_signature.name.isEmpty() || elsewhere) {
        a_signatureTip->hide();
        emit signatureAnswered();
        return;
    }
    QStringList params;
    for (int k = 0; k < a_signature.params.size(); ++k) {
        const QString p = escaped(a_signature.params.at(k));
        params << (k == a_signature.index ? QStringLiteral("<b><u>%1</u></b>").arg(p) : p);
    }
    QString html = QStringLiteral("<code>%1(%2)</code>").arg(escaped(a_signature.name), params.join(QStringLiteral(", ")));
    if (!a_signature.doc.isEmpty())
        html += QStringLiteral("<br><span style='white-space:pre-wrap'>%1</span>").arg(escaped(a_signature.doc));
    a_signatureTip->setText(html);
    a_signatureTip->adjustSize();
    placeSignature();
    a_signatureTip->show();
    a_signatureTip->raise();
    emit signatureAnswered();
}

void PythonDoc::placeSignature()
{
    // Above the call's bracket (below its line when there is no room).
    QTextCursor at = textCursor();
    const QTextBlock block = document()->findBlockByNumber(a_signature.openLine - 1);
    if (block.isValid())
        at.setPosition(block.position() + std::clamp(a_signature.openColumn, 0, std::max(0, block.length() - 1)));
    const QRect rect = cursorRect(at);
    QPoint place = viewport()->mapToGlobal(QPoint(rect.left(), rect.top() - a_signatureTip->height() - 2));
    const QScreen* screen = this->screen();
    if (screen != nullptr && place.y() < screen->availableGeometry().top())
        place = viewport()->mapToGlobal(QPoint(rect.left(), rect.bottom() + 2));
    a_signatureTip->move(place);
}

void PythonDoc::focusOutEvent(QFocusEvent* event)
{
    if (QApplication::focusWidget() != a_completer->popup()) a_signatureTip->hide();
    TextDoc::focusOutEvent(event);
}

// ----------------------------------------------------------------------
// What a name is

bool PythonDoc::showHelpAt(const QPoint& at)
{
    const QTextCursor cursor = cursorForPosition(at);
    const QTextBlock block = cursor.block();
    const QString text = block.text();
    int start = cursor.positionInBlock(), end = start;
    const auto nameCharacter = [](QChar c) { return c.isLetterOrNumber() || c == QLatin1Char('_'); };
    while (start > 0 && nameCharacter(text.at(start - 1))) --start;
    while (end < text.size() && nameCharacter(text.at(end))) ++end;
    if (start == end || text.at(start).isDigit() || qucs_s::python::inStringOrComment(text.left(start))) return false;
    // Not past the line's text (the mouse on the paper after it).
    QTextCursor last(block);
    last.setPosition(block.position() + end);
    if (at.x() > cursorRect(last).right() + 2) return false;
    const int id = ask(QStringLiteral("help"), block.blockNumber() + 1, start);
    if (id == 0) return false;
    a_helpRequest = id;
    a_helpAt = viewport()->mapToGlobal(at);
    QTextCursor first(block);
    first.setPosition(block.position() + start);
    a_helpRect = cursorRect(first).united(cursorRect(last));
    a_helpDiagnostics = diagnosticsAtY(at.y());
    return true;
}

void PythonDoc::answerHelp(const qucs_s::python::Answer& answer)
{
    if (!answer.engine.isEmpty()) a_completionEngine = answer.engine;
    if (answer.help.valid) {
        a_lastHelp = answer.help.text.isEmpty() ? answer.help.title : answer.help.title + QLatin1Char('\n') + answer.help.text;
        QString html;
        if (!a_helpDiagnostics.isEmpty())
            html += QStringLiteral("<p style='white-space:pre-wrap'>%1</p><hr>").arg(escaped(a_helpDiagnostics));
        html += QStringLiteral("<p style='white-space:pre-wrap'><b>%1</b>").arg(escaped(answer.help.title));
        if (!answer.help.type.isEmpty()) html += QStringLiteral(" &nbsp;<i>%1</i>").arg(escaped(answer.help.type));
        html += QStringLiteral("</p>");
        if (!answer.help.text.isEmpty())
            html += QStringLiteral("<p style='white-space:pre-wrap'>%1</p>").arg(escaped(answer.help.text));
        if (isVisible()) QToolTip::showText(a_helpAt, html, viewport(), a_helpRect);
    }
    emit helpAnswered();
}

bool PythonDoc::viewportEvent(QEvent* event)
{
    // The mouse resting on a name: what it is, the line's errors and
    // warnings with it - those at once, the rest when the completer says.
    // While the debugger is stopped: its value.
    if (event->type() == QEvent::ToolTip) {
        const auto* help = static_cast<QHelpEvent*>(event);
        if (a_valueLookup) {
            const QTextCursor cursor = cursorForPosition(help->pos());
            const QTextBlock block = cursor.block();
            const auto [expression, start] = qucs_s::python::dottedNameAt(block.text(), cursor.positionInBlock());
            // (A keyword has no value: what it is, as ever.)
            static const QSet<QString> keywords{
                QStringLiteral("False"), QStringLiteral("None"), QStringLiteral("True"), QStringLiteral("and"), QStringLiteral("as"),
                QStringLiteral("assert"), QStringLiteral("async"), QStringLiteral("await"), QStringLiteral("break"),
                QStringLiteral("class"), QStringLiteral("continue"), QStringLiteral("def"), QStringLiteral("del"),
                QStringLiteral("elif"), QStringLiteral("else"), QStringLiteral("except"), QStringLiteral("finally"),
                QStringLiteral("for"), QStringLiteral("from"), QStringLiteral("global"), QStringLiteral("if"),
                QStringLiteral("import"), QStringLiteral("in"), QStringLiteral("is"), QStringLiteral("lambda"),
                QStringLiteral("nonlocal"), QStringLiteral("not"), QStringLiteral("or"), QStringLiteral("pass"),
                QStringLiteral("raise"), QStringLiteral("return"), QStringLiteral("try"), QStringLiteral("while"),
                QStringLiteral("with"), QStringLiteral("yield")};
            if (!expression.isEmpty() && !keywords.contains(expression) && a_valueLookup(expression)) {
                a_valueExpression = expression;
                a_valueAt = help->pos();
                a_helpAt = help->globalPos();
                QTextCursor first(block), last(block);
                first.setPosition(block.position() + start);
                last.setPosition(block.position() + start + int(expression.size()));
                a_helpRect = cursorRect(first).united(cursorRect(last));
                return true;
            }
        }
        if (showHelpAt(help->pos())) {
            if (!a_helpDiagnostics.isEmpty()) QToolTip::showText(help->globalPos(), a_helpDiagnostics, viewport());
            return true;
        }
    }
    return TextDoc::viewportEvent(event);
}

// ----------------------------------------------------------------------
// Where a name is defined

void PythonDoc::goToDefinition()
{
    const QTextCursor cursor = textCursor();
    a_definition = {};
    const int id = ask(QStringLiteral("definition"), cursor.blockNumber() + 1, cursor.positionInBlock());
    if (id == 0) {
        emit definitionAnswered();   // (no completer: nothing found)
        return;
    }
    a_definitionRequest = id;
}

void PythonDoc::mousePressEvent(QMouseEvent* event)
{
    // Ctrl+click (Command+click on a Mac): to the name's definition.
    if (event->button() == Qt::LeftButton && (event->modifiers() & Qt::ControlModifier)) {
        setTextCursor(cursorForPosition(event->position().toPoint()));
        goToDefinition();
        event->accept();
        return;
    }
    TextDoc::mousePressEvent(event);
}

void PythonDoc::setLibraryFile(bool on)
{
    if (a_library == on) return;
    a_library = on;
    setReadOnly(on);
    if (on) {
        stopCheck();
        setDiagnostics({});
        if (a_libraryNote == nullptr) {
            a_libraryNote = new QLabel(viewport());
            a_libraryNote->setObjectName(QStringLiteral("pythonLibraryNote"));
            a_libraryNote->setAutoFillBackground(true);
            a_libraryNote->setBackgroundRole(QPalette::ToolTipBase);
            a_libraryNote->setForegroundRole(QPalette::ToolTipText);
            a_libraryNote->setMargin(4);
        }
        a_libraryNote->setText(tr("Python's library - read-only"));
        a_libraryNote->setToolTip(tr("%1 is Python's own, or a package installed for it: shown, not edited here.")
                                      .arg(QDir::toNativeSeparators(getDocName())));
        a_libraryNote->adjustSize();
        placeLibraryNote();
        a_libraryNote->show();
    } else {
        if (a_libraryNote != nullptr) a_libraryNote->hide();
        checkNow();
    }
}

void PythonDoc::placeLibraryNote()
{
    if (a_libraryNote != nullptr) a_libraryNote->move(std::max(0, viewport()->width() - a_libraryNote->width() - 8), 4);
}

// ----------------------------------------------------------------------
// The cells

std::pair<int, int> PythonDoc::currentCell() const
{
    return qucs_s::python::cellAround(toPlainText(), textCursor().blockNumber() + 1);
}

void PythonDoc::paintEvent(QPaintEvent* event)
{
    TextDoc::paintEvent(event);
    // A rule above each cell's first line (# %%).
    QPainter painter(viewport());
    painter.setPen(QPen(QColor(0x8a, 0x94, 0xa6), 1));
    for (QTextBlock block = firstVisibleBlock(); block.isValid(); block = block.next()) {
        const QRectF box = blockBoundingGeometry(block).translated(contentOffset());
        if (box.top() > event->rect().bottom()) break;
        if (block.isVisible() && qucs_s::python::isCellMarker(block.text()))
            painter.drawLine(QPointF(0, box.top() + 0.5), QPointF(viewport()->width(), box.top() + 0.5));
    }
    // A line folded: a box with three dots after its text.
    if (a_folds.isEmpty()) return;
    painter.setRenderHint(QPainter::Antialiasing);
    for (QTextBlock block = firstVisibleBlock(); block.isValid(); block = block.next()) {
        const QRectF box = blockBoundingGeometry(block).translated(contentOffset());
        if (box.top() > event->rect().bottom()) break;
        if (!block.isVisible() || block.layout() == nullptr || block.layout()->lineCount() == 0 || !isFolded(block.blockNumber() + 1))
            continue;
        const QTextLine last = block.layout()->lineAt(block.layout()->lineCount() - 1);
        const qreal x = box.left() + last.x() + last.naturalTextWidth() + fontMetrics().horizontalAdvance(QLatin1Char(' '));
        const QRectF dots(x, box.top() + last.y() + 2, fontMetrics().horizontalAdvance(QStringLiteral(" ... ")), last.height() - 4);
        painter.setPen(QPen(palette().color(QPalette::PlaceholderText), 1));
        painter.setBrush(palette().color(QPalette::AlternateBase));
        painter.drawRoundedRect(dots, 3, 3);
        painter.drawText(dots, Qt::AlignCenter, QStringLiteral("..."));
    }
}

// ----------------------------------------------------------------------
// Format Document, Fix Problems

void PythonDoc::format() { runFormatter(QStringLiteral("format")); }

void PythonDoc::fixProblems() { runFormatter(QStringLiteral("fix")); }

void PythonDoc::runFormatter(const QString& mode)
{
    if (a_formatProcess != nullptr || isReadOnly()) return;
    a_formatMode = mode;
    a_formatRevision = document()->revision();
    a_formatProcess = new QProcess(this);
    QProcessEnvironment environment = qucs_s::python::scriptEnvironment();
    environment.insert(QStringLiteral("PYTHONDONTWRITEBYTECODE"), QStringLiteral("1"));
    a_formatProcess->setProcessEnvironment(environment);
    // (In a folder of its own, as the check: the tools run in the script's,
    // for its project's settings.)
    a_formatProcess->setWorkingDirectory(qucs_s::python::neutralFolder());
    connect(a_formatProcess, &QProcess::finished, this, &PythonDoc::finishFormat);
    connect(a_formatProcess, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart) finishFormat();
    });
    const QString name = getDocName().isEmpty() ? QStringLiteral("script.py") : QFileInfo(getDocName()).absoluteFilePath();
    a_formatProcess->start(interpreter(), {QStringLiteral("-c"), qucs_s::python::formatterProgram(), mode, name});
    a_formatProcess->write(toPlainText().toUtf8());
    a_formatProcess->closeWriteChannel();
}

void PythonDoc::finishFormat()
{
    QProcess* process = a_formatProcess;
    if (process == nullptr) return;
    a_formatProcess = nullptr;
    disconnect(process, nullptr, this, nullptr);
    process->deleteLater();
    const bool fix = a_formatMode == QLatin1String("fix");
    QString said;
    if (process->error() == QProcess::FailedToStart) {
        said = tr("No Python at %1 to format it with.").arg(interpreter());
    } else {
        const QJsonObject o = QJsonDocument::fromJson(process->readAllStandardOutput()).object();
        const QString failure = o.value(QStringLiteral("failure")).toString();
        if (o.isEmpty()) said = tr("The formatter gave no answer.");
        else if (!failure.isEmpty()) said = failure;
        else if (a_formatRevision != document()->revision()) said = tr("The script changed while it was being formatted: nothing done.");
        else {
            const QString text = o.value(QStringLiteral("text")).toString();
            const QString tool = o.value(QStringLiteral("tool")).toString();
            if (text == toPlainText()) {
                said = fix ? tr("Nothing for %1 to fix.").arg(tool) : tr("Already formatted as %1 has it.").arg(tool);
            } else {
                replaceText(text);
                said = fix ? tr("Fixed by %1 (Undo takes it back).").arg(tool) : tr("Formatted by %1 (Undo takes it back).").arg(tool);
            }
        }
    }
    emit formatted(said);
}

void PythonDoc::replaceText(const QString& text)
{
    // What differs alone: the cursor and the view stay where they were
    // outside it. One edit, undone at once.
    const QString old = toPlainText();
    qsizetype head = 0;
    while (head < old.size() && head < text.size() && old.at(head) == text.at(head)) ++head;
    qsizetype tail = 0;
    while (tail < old.size() - head && tail < text.size() - head && old.at(old.size() - 1 - tail) == text.at(text.size() - 1 - tail)) ++tail;
    QTextCursor change(document());
    change.setPosition(int(head));
    change.setPosition(int(old.size() - tail), QTextCursor::KeepAnchor);
    change.beginEditBlock();
    change.insertText(text.mid(head, text.size() - head - tail));
    change.endEditBlock();
}

// ----------------------------------------------------------------------
// The outline

QMargins PythonDoc::extraMargins() const
{
    return a_outlineBar != nullptr ? QMargins(0, a_outlineBar->sizeHint().height(), 0, 0) : QMargins();
}

void PythonDoc::resizeEvent(QResizeEvent* event)
{
    TextDoc::resizeEvent(event);
    placeOutline();
    placeLibraryNote();
}

void PythonDoc::placeOutline()
{
    if (a_outlineBar == nullptr) return;
    const QRect cr = contentsRect();
    a_outlineBar->setGeometry(cr.left(), cr.top(), cr.width(), a_outlineBar->sizeHint().height());
    a_outlineBar->raise();
}

void PythonDoc::updateOutline()
{
    a_outlineDelay->stop();
    a_outlineEntries = qucs_s::python::outlineOf(toPlainText());
    // Each with its letter, as its completion has it (C a class, f a
    // function; m the module, the top level), set in by how deep it is.
    int deepest = 0;
    for (const qucs_s::python::OutlineEntry& e : std::as_const(a_outlineEntries)) deepest = std::max(deepest, e.depth);
    a_outline->setIconSize(QSize(kOutlineBadge + deepest * kOutlineStep, kOutlineBadge));
    const auto icon = [deepest](const QString& kind, int depth) {
        QIcon set;
        for (const int scale : {1, 2}) {
            QPixmap pixmap((kOutlineBadge + deepest * kOutlineStep) * scale, kOutlineBadge * scale);
            pixmap.setDevicePixelRatio(scale);
            pixmap.fill(Qt::transparent);
            QPainter painter(&pixmap);
            painter.drawPixmap(depth * kOutlineStep, 0,
                               qucs_s::python::completionIcon(kind).pixmap(QSize(kOutlineBadge, kOutlineBadge), scale));
            painter.end();
            set.addPixmap(pixmap);
        }
        return set;
    };
    {
        const QSignalBlocker block(a_outline);
        a_outline->clear();
        a_outline->addItem(icon(QStringLiteral("module"), 0), tr("(top level)"), 0);
        a_outline->setItemData(0, QStringLiteral("module"), kOutlineKindRole);
        for (const qucs_s::python::OutlineEntry& e : std::as_const(a_outlineEntries)) {
            const QString kind = e.kind == QLatin1String("class") ? QStringLiteral("class") : QStringLiteral("function");
            a_outline->addItem(icon(kind, e.depth), e.name, e.line);
            const int row = a_outline->count() - 1;
            a_outline->setItemData(row, kind, kOutlineKindRole);
            a_outline->setItemData(row, tr("%1 %2, line %3").arg(e.kind, e.name).arg(e.line), Qt::ToolTipRole);
        }
    }
    chooseOutlineEntry();
}

void PythonDoc::chooseOutlineEntry()
{
    // The innermost function or class whose lines the cursor is on.
    const int line = textCursor().blockNumber() + 1;
    int chosen = 0;
    for (int k = 0; k < a_outlineEntries.size(); ++k) {
        const qucs_s::python::OutlineEntry& e = a_outlineEntries.at(k);
        if (e.line <= line && line <= e.lastLine) chosen = k + 1;
    }
    const QSignalBlocker block(a_outline);
    a_outline->setCurrentIndex(chosen);
}

// ----------------------------------------------------------------------
// The breakpoints and the debugger's line

QList<int> PythonDoc::breakpoints() const
{
    QList<int> lines;
    for (const BreakpointMark& b : a_breakpoints)
        if (!b.at.isNull()) lines << b.at.blockNumber() + 1;
    std::sort(lines.begin(), lines.end());
    lines.erase(std::unique(lines.begin(), lines.end()), lines.end());
    return lines;
}

QList<qucs_s::python::Breakpoint> PythonDoc::breakpointList() const
{
    QList<qucs_s::python::Breakpoint> list;
    QSet<int> seen;
    for (const BreakpointMark& b : a_breakpoints) {
        if (b.at.isNull()) continue;
        qucs_s::python::Breakpoint spec = b.spec;
        spec.line = b.at.blockNumber() + 1;
        if (seen.contains(spec.line)) continue;   // (two that the text brought together: the first)
        seen.insert(spec.line);
        list.append(spec);
    }
    std::sort(list.begin(), list.end(), [](const auto& a, const auto& b) { return a.line < b.line; });
    return list;
}

qucs_s::python::Breakpoint PythonDoc::breakpointAt(int line) const
{
    for (const qucs_s::python::Breakpoint& b : breakpointList())
        if (b.line == line) return b;
    return {};
}

void PythonDoc::setBreakpoints(const QList<int>& lines)
{
    // (Those of the lines that stay keep what they were.)
    const QList<qucs_s::python::Breakpoint> had = breakpointList();
    a_breakpoints.clear();
    for (const int line : lines) {
        const QTextBlock block = document()->findBlockByNumber(line - 1);
        if (!block.isValid()) continue;
        qucs_s::python::Breakpoint spec;
        for (const auto& b : had)
            if (b.line == line) spec = b;
        spec.line = line;
        a_breakpoints.append({QTextCursor(block), spec});
    }
    refreshMarks();
    emit breakpointsChanged();
}

void PythonDoc::setBreakpoint(const qucs_s::python::Breakpoint& b)
{
    const QTextBlock block = document()->findBlockByNumber(b.line - 1);
    if (!block.isValid()) return;
    a_breakpoints.removeIf([&](const BreakpointMark& m) { return m.at.blockNumber() == block.blockNumber(); });
    a_breakpoints.append({QTextCursor(block), b});
    refreshMarks();
    emit breakpointsChanged();
}

void PythonDoc::removeBreakpoint(int line)
{
    const qsizetype had = a_breakpoints.size();
    a_breakpoints.removeIf([&](const BreakpointMark& m) { return m.at.blockNumber() + 1 == line; });
    if (a_breakpoints.size() == had) return;
    refreshMarks();
    emit breakpointsChanged();
}

void PythonDoc::toggleBreakpoint(int line)
{
    const QTextBlock block = document()->findBlockByNumber(line - 1);
    if (!block.isValid()) return;
    const qsizetype had = a_breakpoints.size();
    a_breakpoints.removeIf([&](const BreakpointMark& m) { return m.at.blockNumber() == block.blockNumber(); });
    if (a_breakpoints.size() == had) {
        qucs_s::python::Breakpoint spec;
        spec.line = line;
        a_breakpoints.append({QTextCursor(block), spec});
    }
    refreshMarks();
    emit breakpointsChanged();
}

QMenu* PythonDoc::marginMenuAt(int line)
{
    auto* menu = new QMenu(this);
    menu->setAttribute(Qt::WA_DeleteOnClose);
    const qucs_s::python::Breakpoint b = breakpointAt(line);
    if (b.line == 0) {
        menu->addAction(tr("Add Breakpoint"), this, [this, line] { toggleBreakpoint(line); })->setObjectName(QStringLiteral("addBreakpoint"));
        menu->addAction(tr("Add Conditional Breakpoint..."), this, [this, line] { editBreakpoint(line, 0); })
            ->setObjectName(QStringLiteral("addConditionalBreakpoint"));
        menu->addAction(tr("Add Logpoint..."), this, [this, line] { editBreakpoint(line, 2); })->setObjectName(QStringLiteral("addLogpoint"));
    } else {
        menu->addAction(tr("Edit Breakpoint..."), this, [this, line] { editBreakpoint(line, 0); })->setObjectName(QStringLiteral("editBreakpoint"));
        menu->addAction(b.enabled ? tr("Disable Breakpoint") : tr("Enable Breakpoint"), this, [this, b] {
                qucs_s::python::Breakpoint changed = b;
                changed.enabled = !b.enabled;
                setBreakpoint(changed);
            })->setObjectName(QStringLiteral("enableBreakpoint"));
        menu->addAction(tr("Remove Breakpoint"), this, [this, line] { removeBreakpoint(line); })->setObjectName(QStringLiteral("removeBreakpoint"));
    }
    if (isFolded(line) || isFoldable(line)) {
        menu->addSeparator();
        menu->addAction(isFolded(line) ? tr("Unfold") : tr("Fold"), this, [this, line] { toggleFold(line); })
            ->setObjectName(QStringLiteral("foldHere"));
    }
    return menu;
}

void PythonDoc::marginMenu(const QTextBlock& block, const QPoint& global)
{
    marginMenuAt(block.blockNumber() + 1)->popup(global);
}

void PythonDoc::editBreakpoint(int line, int field)
{
    qucs_s::python::Breakpoint b = breakpointAt(line);
    const bool had = b.line != 0;
    b.line = line;
    if (!qucs_s::python::editBreakpointDialog(this, &b, field, had)) return;
    setBreakpoint(b);
}

void PythonDoc::setExecutionLine(int line, bool top)
{
    const QTextBlock block = line > 0 ? document()->findBlockByNumber(line - 1) : QTextBlock();
    a_execution = block.isValid() ? QTextCursor(block) : QTextCursor();
    a_executionTop = top;
    refreshMarks();
}

int PythonDoc::executionLine() const
{
    return a_execution.isNull() ? 0 : a_execution.blockNumber() + 1;
}

int PythonDoc::markRoom() const
{
    return std::clamp(fontMetrics().height(), 12, 18) + 2;
}

void PythonDoc::marginPressed(const QTextBlock& block)
{
    toggleBreakpoint(block.blockNumber() + 1);
}

void PythonDoc::paintMark(QPainter& painter, const QTextBlock& block, const QRect& box)
{
    painter.setRenderHint(QPainter::Antialiasing);
    const qreal size = std::min(box.width(), box.height()) - 4;
    const QRectF square(box.left() + (box.width() - size) / 2.0, box.top() + (box.height() - size) / 2.0, size, size);
    const qucs_s::python::Breakpoint b = breakpointAt(block.blockNumber() + 1);
    if (b.line > 0) {
        // A dot; a logpoint a diamond; with a condition or hits, a bar
        // across; off, hollow and grey.
        const QColor colour = b.enabled ? kBreakpoint : QColor(0x8a, 0x94, 0xa6);
        painter.setPen(QPen(colour.darker(130), 1.2));
        painter.setBrush(b.enabled ? QBrush(colour) : QBrush(Qt::NoBrush));
        if (!b.log.isEmpty()) {
            QPolygonF diamond;
            diamond << QPointF(square.center().x(), square.top()) << QPointF(square.right(), square.center().y())
                    << QPointF(square.center().x(), square.bottom()) << QPointF(square.left(), square.center().y());
            painter.drawPolygon(diamond);
        } else {
            painter.drawEllipse(square);
        }
        if (!b.condition.trimmed().isEmpty() || !b.hit.trimmed().isEmpty()) {
            painter.setPen(QPen(b.enabled ? QColor(Qt::white) : colour, std::max(1.5, size / 7.0), Qt::SolidLine, Qt::RoundCap));
            const qreal y = square.center().y();
            painter.drawLine(QPointF(square.left() + size * 0.28, y - size * 0.12), QPointF(square.right() - size * 0.28, y - size * 0.12));
            painter.drawLine(QPointF(square.left() + size * 0.28, y + size * 0.14), QPointF(square.right() - size * 0.28, y + size * 0.14));
        }
    }
    if (!a_execution.isNull() && a_execution.blockNumber() == block.blockNumber()) {
        // An arrow, pointing at the line.
        const QColor colour = a_executionTop ? kStopped : kLookedAt;
        QPolygonF arrow;
        arrow << QPointF(square.left(), square.top() + size * 0.3) << QPointF(square.left() + size * 0.5, square.top() + size * 0.3)
              << QPointF(square.left() + size * 0.5, square.top()) << QPointF(square.right(), square.center().y())
              << QPointF(square.left() + size * 0.5, square.bottom()) << QPointF(square.left() + size * 0.5, square.top() + size * 0.7)
              << QPointF(square.left(), square.top() + size * 0.7);
        painter.setPen(QPen(colour.darker(160), 1));
        painter.setBrush(colour);
        painter.drawPolygon(arrow);
    }
}

QList<QTextEdit::ExtraSelection> PythonDoc::moreSelections() const
{
    QList<QTextEdit::ExtraSelection> marks;
    if (!a_execution.isNull()) {
        QTextEdit::ExtraSelection line;
        const QColor colour = a_executionTop ? kStopped : kLookedAt;
        line.format.setBackground(QColor(colour.red(), colour.green(), colour.blue(), 70));
        line.format.setProperty(QTextFormat::FullWidthSelection, true);
        line.cursor = a_execution;
        line.cursor.clearSelection();
        marks.append(line);
    }
    marks.append(bracketSelections());
    return marks;
}

// ----------------------------------------------------------------------
// Where a name is used; renamed

QString PythonDoc::nameAtCursor() const
{
    const QTextCursor cursor = textCursor();
    const QString text = cursor.block().text();
    int start = cursor.positionInBlock(), end = start;
    const auto nameCharacter = [](QChar c) { return c.isLetterOrNumber() || c == QLatin1Char('_'); };
    while (start > 0 && nameCharacter(text.at(start - 1))) --start;
    while (end < text.size() && nameCharacter(text.at(end))) ++end;
    if (start == end || text.at(start).isDigit() || qucs_s::python::inStringOrComment(text.left(start))) return {};
    return text.mid(start, end - start);
}

void PythonDoc::findReferences()
{
    const QTextCursor cursor = textCursor();
    a_references = {};
    const int id = ask(QStringLiteral("references"), cursor.blockNumber() + 1, cursor.positionInBlock());
    if (id == 0) {
        emit referencesAnswered();   // (no completer: none found)
        return;
    }
    a_referencesRequest = id;
}

void PythonDoc::renameSymbol(const QString& name)
{
    const QTextCursor cursor = textCursor();
    a_rename = {};
    if (isReadOnly() || !startCompleter()) {
        a_rename.refusal = isReadOnly() ? tr("The script is read-only.") : tr("No Python to rename with.");
        emit renameAnswered();
        return;
    }
    const int id = ++a_questions;
    const QJsonObject question{{QStringLiteral("id"), id},
                               {QStringLiteral("kind"), QStringLiteral("rename")},
                               {QStringLiteral("source"), toPlainText()},
                               {QStringLiteral("line"), cursor.blockNumber() + 1},
                               {QStringLiteral("column"), cursor.positionInBlock()},
                               {QStringLiteral("new_name"), name},
                               {QStringLiteral("path"), getDocName().isEmpty() ? QString() : QFileInfo(getDocName()).absoluteFilePath()}};
    a_completerProcess->write(QJsonDocument(question).toJson(QJsonDocument::Compact) + '\n');
    a_renameRequest = id;
    a_renameRevision = document()->revision();
}

// ----------------------------------------------------------------------
// A value, while the debugger is stopped

void PythonDoc::showValue(const QString& expression, const QString& said)
{
    if (expression != a_valueExpression) return;   // (another was asked for since)
    a_valueExpression.clear();
    a_lastValue = said;
    if (said.isEmpty()) {   // (no value there - a function of a module, a name of another frame): what it is
        if (isVisible()) showHelpAt(a_valueAt);
    } else if (isVisible()) {
        QToolTip::showText(a_helpAt, QStringLiteral("<p style='white-space:pre-wrap'><code>%1</code></p>").arg(escaped(said)), viewport(), a_helpRect);
    }
    emit valueShown();
}

// ----------------------------------------------------------------------
// The type check

namespace {
// The interpreters with no type checker (of a kind) installed, and when
// that was found: not asked again for a minute (a check is made after each
// edit), then again - one may have been installed since.
QHash<QString, QDateTime>& withoutTypeChecker()
{
    static QHash<QString, QDateTime> found;
    return found;
}
} // namespace

QString PythonDoc::typeChecker() { return _settings::Get().item<QString>("PythonTypeChecker"); }

void PythonDoc::setTypeChecker(const QString& checker)
{
    _settings::Get().setItem<QString>("PythonTypeChecker", checker);
    withoutTypeChecker().clear();   // (chosen again: looked for again)
    for (PythonDoc* script : openDocuments()) {
        script->a_typeRevision = -1;
        script->a_typeCheck = {};
        if (checker == QLatin1String("off")) {   // its findings gone
            QList<Diagnostic> basic;
            for (const Diagnostic& d : script->diagnostics())
                if (d.source.isEmpty()) basic.append(d);
            script->showProblems(basic, {});
        } else {
            script->scheduleTypeCheck();
        }
    }
}

void PythonDoc::scheduleTypeCheck()
{
    if (typeChecker() == QLatin1String("off") || a_library) return;
    a_typeDelay->start();
}

void PythonDoc::startTypeCheck()
{
    // One at a time: the next when it is done, if the text changed since.
    if (a_typeProcess != nullptr) return;
    const QString checker = typeChecker();
    if (checker == QLatin1String("off") || a_library) return;
    if (a_typeRevision == document()->revision() && a_typeRunningWith == checker) return;   // (checked as it is)
    const QString key = interpreter() + QLatin1Char('\n') + checker;
    if (const auto none = withoutTypeChecker().constFind(key);
        none != withoutTypeChecker().constEnd() && none->secsTo(QDateTime::currentDateTimeUtc()) < 60) {
        a_typeCheck = {};
        a_typeRevision = document()->revision();
        a_typeRunningWith = checker;
        return;
    }
    a_typeProcess = new QProcess(this);
    a_typeRunning = document()->revision();
    a_typeRunningWith = checker;
    QProcessEnvironment environment = qucs_s::python::scriptEnvironment();
    environment.insert(QStringLiteral("PYTHONDONTWRITEBYTECODE"), QStringLiteral("1"));
    a_typeProcess->setProcessEnvironment(environment);
    a_typeProcess->setWorkingDirectory(qucs_s::python::neutralFolder());
    connect(a_typeProcess, &QProcess::finished, this, &PythonDoc::finishTypeCheck);
    connect(a_typeProcess, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart) finishTypeCheck();
    });
    const QString name = getDocName().isEmpty() ? QString() : QFileInfo(getDocName()).absoluteFilePath();
    const QString cache = QDir(qucs_s::python::neutralFolder()).filePath(QStringLiteral("typecheck-cache"));
    a_typeProcess->start(interpreter(), {QStringLiteral("-c"), qucs_s::python::typeCheckProgram(), checker, name, cache});
    a_typeProcess->write(toPlainText().toUtf8());
    a_typeProcess->closeWriteChannel();
    // (A type check that hangs: ended, the next one when the text changes.)
    QTimer::singleShot(120000, a_typeProcess, [process = a_typeProcess] { process->kill(); });
}

void PythonDoc::finishTypeCheck()
{
    QProcess* process = a_typeProcess;
    if (process == nullptr) return;
    a_typeProcess = nullptr;
    disconnect(process, nullptr, this, nullptr);
    process->deleteLater();
    if (a_typeRunning != document()->revision() || a_typeRunningWith != typeChecker()) {
        scheduleTypeCheck();   // (of a text no longer there: again)
        return;
    }
    qucs_s::python::TypeCheck check;
    if (process->error() == QProcess::FailedToStart) check.failure = tr("No Python at %1 to check its types with.").arg(interpreter());
    else if (process->exitStatus() != QProcess::NormalExit) check.failure = tr("The type check ended before it answered.");
    else check = qucs_s::python::readTypeCheck(process->readAllStandardOutput());
    a_typeCheck = check;
    a_typeRevision = a_typeRunning;
    const QString key = interpreter() + QLatin1Char('\n') + a_typeRunningWith;
    if (check.tool.isEmpty() && check.failure.isEmpty()) withoutTypeChecker().insert(key, QDateTime::currentDateTimeUtc());
    else withoutTypeChecker().remove(key);
    // The check's findings as they are, and these.
    QList<Diagnostic> basic, typed;
    for (const Diagnostic& d : diagnostics())
        if (d.source.isEmpty()) basic.append(d);
    const QString tool = check.tool.section(QLatin1Char(' '), 0, 0);
    for (const qucs_s::python::Problem& p : std::as_const(check.problems)) {
        Diagnostic d;
        d.line = p.line;
        d.column = p.column;
        d.endLine = p.endLine;
        d.endColumn = p.endColumn;
        d.error = false;   // (it runs all the same)
        d.message = p.code.isEmpty() ? QStringLiteral("%1 (%2)").arg(p.message, tool) : QStringLiteral("%1 (%2: %3)").arg(p.message, tool, p.code);
        d.source = tool.isEmpty() ? QStringLiteral("types") : tool;
        typed.append(d);
    }
    showProblems(basic, typed);
    emit typeCheckFinished();
}
