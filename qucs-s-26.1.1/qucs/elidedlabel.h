/*
 * elidedlabel.h - a label of one line, and a tool button's name, that give
 * way to their layout: what does not fit is cut with an ellipsis
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#ifndef QUCS_ELIDEDLABEL_H
#define QUCS_ELIDEDLABEL_H

#include <QLabel>
#include <QToolButton>

class QTextLayout;

/// A label of one line that asks its layout for the width of a few
/// characters, not of its whole text. A plain QLabel asks for all of it, and
/// its layouts pass that on to the window: a tab's status of some hundred
/// characters made the main window wider than the screen, the Claude Code
/// dock pushed off its edge (2 November 2026).
///
/// What does not fit is drawn cut, with an ellipsis, and the whole text is
/// the label's tool tip, before its own. text() stays whole for what reads
/// it (the tests, the control's tools), and setText() through a QLabel*
/// does as it did: the text is cut when it is drawn. Rich text, cut, is
/// drawn as its plain text; its links still answer a click.
class ElidedLabel : public QLabel
{
public:
    explicit ElidedLabel(QWidget* parent = nullptr, Qt::TextElideMode mode = Qt::ElideRight);
    explicit ElidedLabel(const QString& text, QWidget* parent = nullptr, Qt::TextElideMode mode = Qt::ElideRight);

    /// Whether the text is cut at the width the label has.
    bool elided() const;
    /// What is drawn: the text, or cut to the width with an ellipsis.
    QString shown() const;

    QSize minimumSizeHint() const override;

protected:
    bool event(QEvent* event) override;
    void paintEvent(QPaintEvent* event) override;

private:
    Qt::TextElideMode a_mode;

    bool rich() const;
    /// The text as one line of plain text.
    QString plain() const;
    /// Where the text is drawn: the contents less the margin and indent.
    QRect room() const;
};

/// A label of wrapped lines whose words may be longer than a line: a path, a
/// URL, a command, a run of letters with no break. QLabel's word wrap breaks
/// lines between words only and asks its layout for its longest word's
/// width, which the layouts pass on to the window: a permission for a file
/// of a long path made the Claude Code dock a thousand pixels wide and more,
/// a question with a long URL in it too (the panel's fuzz, 3 November 2026).
///
/// This one asks for the width of a few characters, and a word too long for
/// its line is broken where it must be. A text whose words fit is drawn as
/// QLabel draws it, its selection with it. Plain text; text() stays whole.
class WrappedLabel : public QLabel
{
public:
    explicit WrappedLabel(QWidget* parent = nullptr);
    explicit WrappedLabel(const QString& text, QWidget* parent = nullptr);

    /// Whether a word of the text is longer than a line at the width the
    /// label has: drawn broken.
    bool broken() const;

    QSize minimumSizeHint() const override;
    QSize sizeHint() const override;
    int heightForWidth(int width) const override;

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    /// What is around the text: the frame, the contents' margins, the margin.
    QSize chrome() const;
    /// The height of the text in lines of \a width, a word too long for a
    /// line broken (laid out in \a layout, when given).
    qreal linesHeight(qreal width, QTextLayout* layout = nullptr) const;
};

/// A tool button whose text, a file's name, gives way as ElidedLabel's does:
/// it asks for its icon and a few characters, and what does not fit is drawn
/// cut, in the middle by default so that a name keeps its kind. The Claude
/// Code panel's document chips (2 November 2026): a long name widened the
/// dock and the window. text() stays whole; the tool tip is the button's
/// own, which names the file.
class ElidedToolButton : public QToolButton
{
public:
    explicit ElidedToolButton(QWidget* parent = nullptr, Qt::TextElideMode mode = Qt::ElideMiddle);

    /// Whether the text is cut at the width the button has.
    bool elided() const;
    /// What is drawn: the text, or cut to the width with an ellipsis.
    QString shown() const;

    QSize minimumSizeHint() const override;

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    Qt::TextElideMode a_mode;

    /// The width of the text as the button lays it out.
    int textWidth() const;
};

#endif
