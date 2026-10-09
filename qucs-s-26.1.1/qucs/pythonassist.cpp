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

#include "qucs.h"

#include <QAbstractItemView>
#include <QApplication>
#include <QComboBox>
#include <QCompleter>
#include <QDir>
#include <QFileInfo>
#include <QHelpEvent>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QProcess>
#include <QScreen>
#include <QTextBlock>
#include <QTimer>
#include <QToolTip>

#include <algorithm>

namespace {

QString escaped(const QString& text) { return text.toHtmlEscaped(); }

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
    if (event->type() == QEvent::ToolTip) {
        const auto* help = static_cast<QHelpEvent*>(event);
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
    {
        const QSignalBlocker block(a_outline);
        a_outline->clear();
        a_outline->addItem(tr("(top level)"), 0);
        for (const qucs_s::python::OutlineEntry& e : std::as_const(a_outlineEntries)) {
            const QString text = QString(e.depth * 4, QLatin1Char(' ')) + (e.kind == QLatin1String("class") ? QStringLiteral("class ") : QStringLiteral("def ")) + e.name;
            a_outline->addItem(text, e.line);
            a_outline->setItemData(a_outline->count() - 1, tr("Line %1").arg(e.line), Qt::ToolTipRole);
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
    for (const QTextCursor& at : a_breakpoints)
        if (!at.isNull()) lines << at.blockNumber() + 1;
    std::sort(lines.begin(), lines.end());
    lines.erase(std::unique(lines.begin(), lines.end()), lines.end());
    return lines;
}

void PythonDoc::setBreakpoints(const QList<int>& lines)
{
    a_breakpoints.clear();
    for (const int line : lines) {
        const QTextBlock block = document()->findBlockByNumber(line - 1);
        if (block.isValid()) a_breakpoints.append(QTextCursor(block));
    }
    refreshMarks();
    emit breakpointsChanged();
}

void PythonDoc::toggleBreakpoint(int line)
{
    const QTextBlock block = document()->findBlockByNumber(line - 1);
    if (!block.isValid()) return;
    const qsizetype had = a_breakpoints.size();
    a_breakpoints.removeIf([&](const QTextCursor& at) { return at.blockNumber() == block.blockNumber(); });
    if (a_breakpoints.size() == had) a_breakpoints.append(QTextCursor(block));
    refreshMarks();
    emit breakpointsChanged();
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
    const bool breakpoint = std::any_of(a_breakpoints.cbegin(), a_breakpoints.cend(),
                                        [&](const QTextCursor& at) { return at.blockNumber() == block.blockNumber(); });
    if (breakpoint) {
        painter.setPen(QPen(kBreakpoint.darker(130), 1));
        painter.setBrush(kBreakpoint);
        painter.drawEllipse(square);
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
    if (a_execution.isNull()) return {};
    QTextEdit::ExtraSelection line;
    const QColor colour = a_executionTop ? kStopped : kLookedAt;
    line.format.setBackground(QColor(colour.red(), colour.green(), colour.blue(), 70));
    line.format.setProperty(QTextFormat::FullWidthSelection, true);
    line.cursor = a_execution;
    line.cursor.clearSelection();
    return {line};
}
