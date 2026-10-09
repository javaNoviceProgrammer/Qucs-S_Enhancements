/*
 * pythonedit.cpp - the Python editor's help with typing (pythondoc.h):
 *                  brackets and quotes closed as they are typed, the
 *                  bracket at the cursor matched, the text folded
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#include "pythondoc.h"

#include "settings.h"

#include <QAbstractTextDocumentLayout>
#include <QKeyEvent>
#include <QPainter>
#include <QPainterPath>
#include <QRegularExpression>
#include <QTextBlock>
#include <QTimer>

#include <algorithm>

namespace {

bool isOpener(QChar c) { return c == QLatin1Char('(') || c == QLatin1Char('[') || c == QLatin1Char('{'); }
bool isCloser(QChar c) { return c == QLatin1Char(')') || c == QLatin1Char(']') || c == QLatin1Char('}'); }
bool isQuote(QChar c) { return c == QLatin1Char('\'') || c == QLatin1Char('"'); }

QChar closerOf(QChar c)
{
    if (c == QLatin1Char('(')) return QLatin1Char(')');
    if (c == QLatin1Char('[')) return QLatin1Char(']');
    if (c == QLatin1Char('{')) return QLatin1Char('}');
    return c;   // (a quote)
}

// What may follow a bracket or a quote closed as it is typed: the line's
// end, a space, a closing bracket, punctuation - not a word (typed before).
bool closesBefore(QChar next)
{
    return next.isNull() || next.isSpace() || isCloser(next) || QStringLiteral(",:;.=").contains(next);
}

// The prefix of a string (f, r, b, u, rb, br, fr, rf: any case) ending
// \a before - a quote after it opens a string, not a word's.
bool endsInStringPrefix(const QString& before)
{
    qsizetype k = before.size();
    while (k > 0 && before.at(k - 1).isLetterOrNumber()) --k;
    const QString word = before.mid(k).toLower();
    if (k > 0 && (before.at(k - 1) == QLatin1Char('_') || before.at(k - 1) == QLatin1Char('.'))) return false;
    static const QStringList prefixes{QStringLiteral("f"),  QStringLiteral("r"),  QStringLiteral("b"),  QStringLiteral("u"),
                                      QStringLiteral("rb"), QStringLiteral("br"), QStringLiteral("fr"), QStringLiteral("rf"),
                                      QStringLiteral("t"),  QStringLiteral("rt"), QStringLiteral("tr")};
    return prefixes.contains(word);
}

// The colours of the marks of brackets.
QColor matchedColour(const QPalette& palette)
{
    QColor c = palette.color(QPalette::Highlight);
    c.setAlpha(70);
    return c;
}

} // namespace

// ----------------------------------------------------------------------
// Brackets and quotes closed

bool PythonDoc::autoClose() { return _settings::Get().item<bool>("PythonAutoClose"); }

void PythonDoc::setAutoClose(bool on) { _settings::Get().setItem<bool>("PythonAutoClose", on); }

bool PythonDoc::closePair(QKeyEvent* event)
{
    const Qt::KeyboardModifiers modifiers = event->modifiers() & ~(Qt::ShiftModifier | Qt::KeypadModifier);
    if (modifiers != Qt::NoModifier) return false;
    QTextCursor cursor = textCursor();
    const QString line = cursor.block().text();
    const int at = cursor.positionInBlock();
    const QString before = line.left(at);
    const QChar next = at < line.size() ? line.at(at) : QChar();

    // Backspace between a pair made empty: both go.
    if (event->key() == Qt::Key_Backspace) {
        if (cursor.hasSelection() || at == 0 || next.isNull()) return false;
        const QChar previous = line.at(at - 1);
        if (!((isOpener(previous) && next == closerOf(previous)) || (isQuote(previous) && next == previous))) return false;
        if (isQuote(previous) && qucs_s::python::inStringOrComment(line.left(at - 1))) return false;
        cursor.beginEditBlock();
        cursor.movePosition(QTextCursor::Left);
        cursor.movePosition(QTextCursor::Right, QTextCursor::KeepAnchor, 2);
        cursor.removeSelectedText();
        cursor.endEditBlock();
        setTextCursor(cursor);
        return true;
    }

    const QString typed = event->text();
    if (typed.size() != 1) return false;
    const QChar c = typed.at(0);
    if (!isOpener(c) && !isCloser(c) && !isQuote(c)) return false;

    // A selection wrapped in the pair (the selection kept, inside it).
    if (cursor.hasSelection() && (isOpener(c) || isQuote(c))) {
        const int start = cursor.selectionStart(), end = cursor.selectionEnd();
        cursor.beginEditBlock();
        cursor.setPosition(end);
        cursor.insertText(QString(closerOf(c)));
        cursor.setPosition(start);
        cursor.insertText(QString(c));
        cursor.endEditBlock();
        cursor.setPosition(start + 1);
        cursor.setPosition(end + 1, QTextCursor::KeepAnchor);
        setTextCursor(cursor);
        return true;
    }
    if (cursor.hasSelection()) return false;
    const bool inside = qucs_s::python::inStringOrComment(before);

    // A closing bracket or quote typed where one is already: over it.
    if ((isCloser(c) || isQuote(c)) && next == c) {
        if (isCloser(c) && inside) return false;
        if (isQuote(c) && !inside) return false;   // (a quote that opens one: typed)
        cursor.movePosition(QTextCursor::Right);
        setTextCursor(cursor);
        return true;
    }
    if (isCloser(c)) return false;

    if (isOpener(c)) {
        if (inside || !closesBefore(next)) return false;
        cursor.beginEditBlock();
        cursor.insertText(QString(c) + closerOf(c));
        cursor.endEditBlock();
        cursor.movePosition(QTextCursor::Left);
        setTextCursor(cursor);
        return true;
    }

    // A quote: a third makes a triple quote of the two before ("""|""").
    if (before.endsWith(QString(2, c)) && !before.endsWith(QString(3, c)) && next != c
        && !qucs_s::python::inStringOrComment(before.chopped(2))) {
        cursor.beginEditBlock();
        cursor.insertText(QString(4, c));
        cursor.endEditBlock();
        cursor.movePosition(QTextCursor::Left, QTextCursor::MoveAnchor, 3);
        setTextCursor(cursor);
        return true;
    }
    if (inside || !closesBefore(next)) return false;
    const QChar previous = before.isEmpty() ? QChar() : before.back();
    if ((previous.isLetterOrNumber() || previous == QLatin1Char('_')) && !endsInStringPrefix(before)) return false;
    cursor.beginEditBlock();
    cursor.insertText(QString(2, c));
    cursor.endEditBlock();
    cursor.movePosition(QTextCursor::Left);
    setTextCursor(cursor);
    return true;
}

// ----------------------------------------------------------------------
// The bracket at the cursor and its partner

void PythonDoc::markBrackets()
{
    const std::pair<int, int> had = a_bracketMarks;
    a_bracketMarks = {-1, -1};
    if (document()->revision() != a_bracketRevision) {
        a_brackets = qucs_s::python::bracketPairs(toPlainText());
        a_bracketRevision = document()->revision();
    }
    const QTextCursor cursor = textCursor();
    if (!cursor.hasSelection()) {
        // The bracket after the cursor, else the one before it.
        const int at = cursor.position();
        for (const int k : {at, at - 1}) {
            const auto found = a_brackets.constFind(k);
            if (k < 0 || found == a_brackets.constEnd()) continue;
            a_bracketMarks = {k, *found};
            break;
        }
    }
    if (had != a_bracketMarks) refreshMarks();
}

// ----------------------------------------------------------------------
// Folding

namespace {
QStringList linesOf(const QTextDocument* document)
{
    return document->toPlainText().split(QLatin1Char('\n'));
}
} // namespace

bool PythonDoc::isFoldable(int line) const
{
    // (Quickly, for the margin: a line below it indented more - or a cell.)
    const QTextBlock block = document()->findBlockByNumber(line - 1);
    if (!block.isValid()) return false;
    const QString head = block.text();
    if (qucs_s::python::isCellMarker(head)) {
        for (QTextBlock b = block.next(); b.isValid() && !qucs_s::python::isCellMarker(b.text()); b = b.next())
            if (!b.text().trimmed().isEmpty()) return true;
        return false;
    }
    if (head.trimmed().isEmpty()) return false;
    const auto indent = [](const QString& text) {
        int n = 0;
        for (const QChar c : text) {
            if (c == QLatin1Char(' ')) ++n;
            else if (c == QLatin1Char('\t')) n += 8 - n % 8;
            else break;
        }
        return n;
    };
    for (QTextBlock b = block.next(); b.isValid(); b = b.next()) {
        const QString text = b.text();
        if (text.trimmed().isEmpty()) continue;
        return indent(text) > indent(head) && !qucs_s::python::isCellMarker(text);
    }
    return false;
}

bool PythonDoc::isFolded(int line) const
{
    return std::any_of(a_folds.cbegin(), a_folds.cend(), [&](const Fold& f) { return f.at.blockNumber() + 1 == line; });
}

QList<int> PythonDoc::foldedLines() const
{
    QList<int> lines;
    for (const Fold& f : a_folds) lines << f.at.blockNumber() + 1;
    std::sort(lines.begin(), lines.end());
    lines.erase(std::unique(lines.begin(), lines.end()), lines.end());
    return lines;
}

void PythonDoc::fold(int line)
{
    if (isFolded(line) || !isFoldable(line)) return;
    const QTextBlock block = document()->findBlockByNumber(line - 1);
    a_folds.append({QTextCursor(block), block.text()});
    // The cursor in what goes: on the line folded.
    const auto [first, last] = qucs_s::python::foldRange(linesOf(document()), line);
    const int at = textCursor().blockNumber() + 1;
    if (first <= at && at <= last) {
        QTextCursor cursor(block);
        cursor.movePosition(QTextCursor::EndOfBlock);
        a_applyingFolds = true;
        setTextCursor(cursor);
        a_applyingFolds = false;
    }
    applyFolds();
}

void PythonDoc::unfold(int line)
{
    const qsizetype had = a_folds.size();
    a_folds.removeIf([&](const Fold& f) { return f.at.blockNumber() + 1 == line; });
    if (a_folds.size() != had) applyFolds();
}

void PythonDoc::toggleFold(int line)
{
    if (isFolded(line)) unfold(line);
    else fold(line);
}

void PythonDoc::foldAll()
{
    // Each function, class and cell (the blocks of loops and ifs left).
    static const QRegularExpression head(QStringLiteral("^\\s*(async\\s+def|def|class)\\b"));
    for (QTextBlock b = document()->begin(); b.isValid(); b = b.next()) {
        const int line = b.blockNumber() + 1;
        if ((head.match(b.text()).hasMatch() || qucs_s::python::isCellMarker(b.text())) && isFoldable(line) && !isFolded(line))
            a_folds.append({QTextCursor(b), b.text()});
    }
    const int at = textCursor().blockNumber() + 1;
    applyFolds();
    if (!document()->findBlockByNumber(at - 1).isVisible()) {
        // (The cursor on the line folded that hides it.)
        const QStringList lines = linesOf(document());
        for (const Fold& f : std::as_const(a_folds)) {
            const auto [first, last] = qucs_s::python::foldRange(lines, f.at.blockNumber() + 1);
            if (first <= at && at <= last && f.at.block().isVisible()) {
                QTextCursor cursor(f.at.block());
                cursor.movePosition(QTextCursor::EndOfBlock);
                a_applyingFolds = true;
                setTextCursor(cursor);
                a_applyingFolds = false;
                break;
            }
        }
    }
}

void PythonDoc::unfoldAll()
{
    if (a_folds.isEmpty()) return;
    a_folds.clear();
    applyFolds();
}

int PythonDoc::foldAround(int line, bool folding) const
{
    // (The cursor's line is never hidden: a fold it is in is its own.)
    if (!folding) return isFolded(line) ? line : 0;
    if (isFoldable(line) && !isFolded(line)) return line;
    // The innermost block the line is in (not folded: the line is shown).
    const QStringList lines = linesOf(document());
    for (int k = line - 1; k >= 1; --k) {
        const auto [first, last] = qucs_s::python::foldRange(lines, k);
        if (first > 0 && first <= line && line <= last) return k;
    }
    return 0;
}

void PythonDoc::applyFolds()
{
    if (a_applyingFolds) return;
    a_applyingFolds = true;
    const QStringList lines = linesOf(document());
    QList<bool> hidden(lines.size() + 1, false);
    // A fold whose line changed (it went, it was another), or that no
    // longer hides anything, opened.
    a_folds.removeIf([&](const Fold& f) {
        const int line = f.at.blockNumber() + 1;
        if (f.at.isNull() || f.at.block().text() != f.header) return true;
        return qucs_s::python::foldRange(lines, line).first == 0;
    });
    for (const Fold& f : std::as_const(a_folds)) {
        const auto [first, last] = qucs_s::python::foldRange(lines, f.at.blockNumber() + 1);
        for (int k = first; k <= last && k < hidden.size(); ++k) hidden[k] = true;
    }
    bool changed = false;
    for (QTextBlock b = document()->begin(); b.isValid(); b = b.next()) {
        const bool visible = !hidden.value(b.blockNumber() + 1, false);
        if (b.isVisible() != visible) {
            b.setVisible(visible);
            changed = true;
        }
    }
    if (changed) {
        document()->markContentsDirty(0, document()->characterCount());
        if (auto* layout = document()->documentLayout()) emit layout->documentSizeChanged(layout->documentSize());
        viewport()->update();
        ensureCursorVisible();
    }
    refreshMarks();
    a_applyingFolds = false;
}

int PythonDoc::foldRoom() const
{
    return std::clamp(fontMetrics().height() * 3 / 4, 9, 14) + 2;
}

void PythonDoc::paintFold(QPainter& painter, const QTextBlock& block, const QRect& box)
{
    const int line = block.blockNumber() + 1;
    const bool folded = isFolded(line);
    if (!folded && !isFoldable(line)) return;
    // A triangle: pointing down, open; to the right, folded.
    painter.setRenderHint(QPainter::Antialiasing);
    const qreal size = std::min(box.width(), box.height()) * 0.55;
    const QPointF centre = QRectF(box).center();
    QPainterPath path;
    if (folded) {
        path.moveTo(centre.x() - size * 0.35, centre.y() - size * 0.5);
        path.lineTo(centre.x() + size * 0.45, centre.y());
        path.lineTo(centre.x() - size * 0.35, centre.y() + size * 0.5);
    } else {
        path.moveTo(centre.x() - size * 0.5, centre.y() - size * 0.3);
        path.lineTo(centre.x() + size * 0.5, centre.y() - size * 0.3);
        path.lineTo(centre.x(), centre.y() + size * 0.45);
    }
    path.closeSubpath();
    QColor ink = palette().color(QPalette::PlaceholderText);
    if (folded) ink = palette().color(QPalette::Text);
    painter.setPen(Qt::NoPen);
    painter.setBrush(ink);
    painter.drawPath(path);
}

void PythonDoc::foldPressed(const QTextBlock& block)
{
    const int line = block.blockNumber() + 1;
    if (isFolded(line) || isFoldable(line)) toggleFold(line);
}

QList<QTextEdit::ExtraSelection> PythonDoc::bracketSelections() const
{
    QList<QTextEdit::ExtraSelection> marks;
    const auto [at, partner] = a_bracketMarks;
    if (at < 0) return marks;
    const auto mark = [&](int position, bool matched) {
        QTextEdit::ExtraSelection s;
        s.cursor = QTextCursor(document());
        s.cursor.setPosition(position);
        s.cursor.setPosition(position + 1, QTextCursor::KeepAnchor);
        if (matched) {
            s.format.setBackground(matchedColour(palette()));
            s.format.setFontWeight(QFont::Bold);
        } else {
            s.format.setForeground(QColor(0xe0, 0x35, 0x2b));
            s.format.setBackground(QColor(0xe0, 0x35, 0x2b, 40));
        }
        marks.append(s);
    };
    mark(at, partner >= 0);
    if (partner >= 0) mark(partner, true);
    return marks;
}
