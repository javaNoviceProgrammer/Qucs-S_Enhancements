/*
 * projectstatedelegate.h - the dot a panel draws for a project or a
 *                          workspace: green for the one open, grey for the
 *                          rest
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#ifndef QUCS_PROJECTSTATEDELEGATE_H
#define QUCS_PROJECTSTATEDELEGATE_H

#include "apptheme.h"
#include "qucs.h"

#include <QApplication>
#include <QColor>
#include <QPainter>
#include <QStyle>
#include <QStyleOptionViewItem>
#include <QStyledItemDelegate>

#include <algorithm>

/// The rows of the Projects and Workspaces panels: at the right end of a
/// project's or a workspace's row, a dot - green for the one open now,
/// grey for the others; a row that is neither has none, and a name is cut
/// short before the dot rather than under it. A row says which it is
/// through QucsFileSystemModel::ProjectStateRole, so a plain list can use
/// this by setting that role on its items.
class ProjectStateDelegate : public QStyledItemDelegate
{
public:
  using QStyledItemDelegate::QStyledItemDelegate;

  static int stateOf(const QModelIndex &index)
  {
    return index.data(QucsFileSystemModel::ProjectStateRole).toInt();
  }
  // The dot's size and its room at the right end, in a row \a rect.
  static int dotSize(const QRect &rect) { return std::clamp(rect.height() * 2 / 5, 7, 12); }
  static int dotRoom(const QRect &rect) { return dotSize(rect) + 12; }
  static QRect dotRect(const QRect &rect)
  {
    const int d = dotSize(rect);
    return QRect(rect.right() - dotRoom(rect) + (dotRoom(rect) - d) / 2, rect.center().y() - d / 2 + 1, d, d);
  }
  static QColor openColour() { return QColor(0x34, 0xc7, 0x59); }

  void paint(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const override
  {
    QStyledItemDelegate::paint(painter, option, index);
    const int state = stateOf(index);
    if (state == QucsFileSystemModel::NoProject) return;
    // (A neutral grey on a highlighted row too: of the list's colours, not
    // the highlight's, which would tint it.)
    const QColor colour = state == QucsFileSystemModel::OpenProject
                              ? openColour()
                              : qucs_s::apptheme::mix(option.palette.color(QPalette::Base), option.palette.color(QPalette::Text), 0.45);
    painter->save();
    painter->setRenderHint(QPainter::Antialiasing);
    painter->setPen(Qt::NoPen);
    painter->setBrush(colour);
    painter->drawEllipse(dotRect(option.rect));
    painter->restore();
  }

protected:
  void initStyleOption(QStyleOptionViewItem *option, const QModelIndex &index) const override
  {
    QStyledItemDelegate::initStyleOption(option, index);
    if (stateOf(index) == QucsFileSystemModel::NoProject) return;
    // The name cut short before the dot rather than under it.
    const QStyle *style = option->widget != nullptr ? option->widget->style() : QApplication::style();
    const int margin = style->pixelMetric(QStyle::PM_FocusFrameHMargin, nullptr, option->widget) + 1;
    const int room = option->rect.width() - option->decorationSize.width() - 4 * margin - dotRoom(option->rect);
    option->text = option->fontMetrics.elidedText(option->text, option->textElideMode, std::max(room, 0));
  }
};

#endif
