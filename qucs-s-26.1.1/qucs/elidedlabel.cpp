/*
 * elidedlabel.cpp - a label of one line, and a tool button's name, that give
 * way to their layout: what does not fit is cut with an ellipsis
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#include "elidedlabel.h"

#include <QHelpEvent>
#include <QPainter>
#include <QStyle>
#include <QStyleOptionToolButton>
#include <QStylePainter>
#include <QTextDocument>
#include <QTextDocumentFragment>
#include <QTextLayout>
#include <QToolTip>

#include <algorithm>
#include <cmath>

ElidedLabel::ElidedLabel(QWidget* parent, Qt::TextElideMode mode) : QLabel(parent), a_mode(mode) {}

ElidedLabel::ElidedLabel(const QString& text, QWidget* parent, Qt::TextElideMode mode)
    : QLabel(text, parent), a_mode(mode)
{
}

bool ElidedLabel::rich() const
{
    return textFormat() == Qt::RichText || (textFormat() == Qt::AutoText && Qt::mightBeRichText(text()));
}

QString ElidedLabel::plain() const
{
    QString out = rich() ? QTextDocumentFragment::fromHtml(text()).toPlainText() : text();
    for (QChar& c : out)
        if (c == QLatin1Char('\n') || c == QChar::LineSeparator || c == QChar::ParagraphSeparator) c = QLatin1Char(' ');
    return out;
}

QRect ElidedLabel::room() const
{
    // As QLabel lays its text out: the margin all round, and an indent on
    // the side it is aligned to (half an x where a frame and no indent).
    QRect r = contentsRect().adjusted(margin(), margin(), -margin(), -margin());
    int in = indent();
    if (in < 0 && frameWidth() > 0) in = fontMetrics().horizontalAdvance(QLatin1Char('x')) / 2 - margin();
    if (in > 0) {
        const Qt::Alignment align = QStyle::visualAlignment(layoutDirection(), alignment());
        if (align & Qt::AlignLeft) r.setLeft(r.left() + in);
        if (align & Qt::AlignRight) r.setRight(r.right() - in);
        if (align & Qt::AlignTop) r.setTop(r.top() + in);
        if (align & Qt::AlignBottom) r.setBottom(r.bottom() - in);
    }
    return r;
}

bool ElidedLabel::elided() const
{
    // QLabel's own size hint is its whole text's, laid out as it draws it.
    return !wordWrap() && !text().isEmpty() && QLabel::sizeHint().width() > width();
}

QString ElidedLabel::shown() const
{
    return elided() ? fontMetrics().elidedText(plain(), a_mode, std::max(0, room().width())) : text();
}

QSize ElidedLabel::minimumSizeHint() const
{
    const QSize whole = QLabel::minimumSizeHint();
    if (wordWrap()) return whole;
    // Room for a few characters, and the margins, frame and indent around
    // it: never more than the whole text asks.
    const int least = width() - room().width() + fontMetrics().averageCharWidth() * 4;
    return QSize(std::min(whole.width(), least), whole.height());
}

bool ElidedLabel::event(QEvent* event)
{
    if (event->type() == QEvent::ToolTip && elided()) {
        const auto* help = static_cast<QHelpEvent*>(event);
        QString tip = rich() ? text() : Qt::convertFromPlainText(text(), Qt::WhiteSpaceNormal);
        if (!toolTip().isEmpty())
            tip += QStringLiteral("<hr>")
                   + (Qt::mightBeRichText(toolTip()) ? toolTip() : Qt::convertFromPlainText(toolTip(), Qt::WhiteSpaceNormal));
        QToolTip::showText(help->globalPos(), tip, this, QRect(), toolTipDuration());
        return true;
    }
    return QLabel::event(event);
}

void ElidedLabel::paintEvent(QPaintEvent* event)
{
    if (!elided()) {
        QLabel::paintEvent(event);
        return;
    }
    QPainter painter(this);
    drawFrame(&painter);
    const Qt::Alignment align = QStyle::visualAlignment(layoutDirection(), alignment());
    style()->drawItemText(&painter, room(), int(align) | Qt::TextSingleLine, palette(), isEnabled(), shown(), foregroundRole());
}

// ----------------------------------------------------------------------

WrappedLabel::WrappedLabel(QWidget* parent) : WrappedLabel(QString(), parent) {}

WrappedLabel::WrappedLabel(const QString& text, QWidget* parent) : QLabel(text, parent)
{
    setTextFormat(Qt::PlainText);
    setWordWrap(true);
}

QSize WrappedLabel::chrome() const
{
    // (Whatever the label's size now: its width less its contents' is the
    // frame and the contents' margins.)
    return QSize(width() - contentsRect().width(), height() - contentsRect().height()) + QSize(2 * margin(), 2 * margin());
}

bool WrappedLabel::broken() const
{
    // QLabel's least width wrapped is its longest word's, as it draws it.
    return !text().isEmpty() && QLabel::minimumSizeHint().width() > width();
}

qreal WrappedLabel::linesHeight(qreal width, QTextLayout* into) const
{
    QString text = this->text();
    text.replace(QLatin1Char('\n'), QChar::LineSeparator);
    QTextLayout own;
    QTextLayout& layout = into != nullptr ? *into : own;
    layout.setText(text);
    layout.setFont(font());
    QTextOption option(QStyle::visualAlignment(layoutDirection(), alignment()) & Qt::AlignHorizontal_Mask);
    option.setWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
    layout.setTextOption(option);
    const qreal leading = QFontMetricsF(font()).leading();
    qreal y = 0;
    layout.beginLayout();
    for (QTextLine line = layout.createLine(); line.isValid(); line = layout.createLine()) {
        line.setLineWidth(std::max<qreal>(1, width));
        line.setPosition(QPointF(0, y));
        y += line.height() + leading;
    }
    layout.endLayout();
    return y;
}

int WrappedLabel::heightForWidth(int width) const
{
    if (QLabel::minimumSizeHint().width() <= width) return QLabel::heightForWidth(width);
    return int(std::ceil(linesHeight(width - chrome().width()))) + chrome().height();
}

QSize WrappedLabel::minimumSizeHint() const
{
    // Room for a few characters, and what is around them: never more than
    // the whole text asks.
    const QSize whole = QLabel::minimumSizeHint();
    return QSize(std::min(whole.width(), chrome().width() + fontMetrics().averageCharWidth() * 8), whole.height());
}

QSize WrappedLabel::sizeHint() const
{
    // (QLabel's is as wide as the longest word too: a dock never sized
    // takes its contents' size hints.)
    const QSize whole = QLabel::sizeHint();
    const int most = chrome().width() + fontMetrics().averageCharWidth() * 40;
    if (whole.width() <= most) return whole;
    return QSize(most, heightForWidth(most));
}

void WrappedLabel::paintEvent(QPaintEvent* event)
{
    if (!broken()) {
        QLabel::paintEvent(event);
        return;
    }
    QPainter painter(this);
    drawFrame(&painter);
    const QRect room = contentsRect().adjusted(margin(), margin(), -margin(), -margin());
    QTextLayout layout;
    linesHeight(room.width(), &layout);
    // (Its selection, by mouse, drawn as QLabel draws it.)
    QList<QTextLayout::FormatRange> selections;
    if (hasSelectedText()) {
        QTextLayout::FormatRange selected;
        selected.start = selectionStart();
        selected.length = int(selectedText().size());
        selected.format.setBackground(palette().brush(QPalette::Highlight));
        selected.format.setForeground(palette().brush(QPalette::HighlightedText));
        selections << selected;
    }
    painter.setPen(palette().color(isEnabled() ? QPalette::Active : QPalette::Disabled, foregroundRole()));
    layout.draw(&painter, room.topLeft(), selections, room);
}

// ----------------------------------------------------------------------

ElidedToolButton::ElidedToolButton(QWidget* parent, Qt::TextElideMode mode) : QToolButton(parent), a_mode(mode)
{
    // A tool button's width is fixed, and a layout holds a fixed width to
    // the whole text: this one shrinks to its least, and grows no wider than
    // its text.
    QSizePolicy policy = sizePolicy();
    policy.setHorizontalPolicy(QSizePolicy::Maximum);
    setSizePolicy(policy);
}

int ElidedToolButton::textWidth() const
{
    // As QToolButton measures it, an & of a mnemonic taken out.
    return text().isEmpty() || toolButtonStyle() == Qt::ToolButtonIconOnly
               ? 0
               : fontMetrics().size(Qt::TextShowMnemonic, text()).width();
}

bool ElidedToolButton::elided() const
{
    return textWidth() > 0 && QToolButton::sizeHint().width() > width();
}

QString ElidedToolButton::shown() const
{
    if (!elided()) return text();
    const int room = width() - (QToolButton::sizeHint().width() - textWidth());
    return fontMetrics().elidedText(text(), a_mode, std::max(0, room), Qt::TextShowMnemonic);
}

QSize ElidedToolButton::minimumSizeHint() const
{
    // The button's own sizes, its text but a few characters: never more
    // than the whole text asks.
    const QSize whole = QToolButton::minimumSizeHint();
    const int words = textWidth();
    if (words == 0) return whole;
    const int least = QToolButton::sizeHint().width() - words + std::min(words, fontMetrics().averageCharWidth() * 4);
    return QSize(std::min(whole.width(), least), whole.height());
}

void ElidedToolButton::paintEvent(QPaintEvent* event)
{
    if (!elided()) {
        QToolButton::paintEvent(event);
        return;
    }
    QStylePainter painter(this);
    QStyleOptionToolButton option;
    initStyleOption(&option);
    option.text = shown();
    painter.drawComplexControl(QStyle::CC_ToolButton, option);
}
