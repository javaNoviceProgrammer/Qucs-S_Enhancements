/*
 * ink.h - the colours a schematic is drawn in, fitted to its paper
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#ifndef QUCS_INK_H
#define QUCS_INK_H

#include <QBrush>
#include <QColor>
#include <QPen>

/*!
 * \brief The colours of a schematic on dark paper.
 *
 * Symbols, wires and texts carry fixed colours meant for light paper
 * (dark blue above all), which vanish on a dark one. Whatever draws onto
 * the paper passes its colours through on(): on light paper - prints,
 * exports, and the canvas in its usual colour - they come back as they
 * are; on dark paper a colour without enough contrast to it gets the
 * lightness it lacks and keeps its hue (dark blue turns light blue, dark
 * red pink, black light grey), and one that shows already is left alone.
 *
 * The paper is set for the time of a paint with a Paper object; nothing
 * set means light paper.
 */
namespace qucs_s::ink {

/// The paper drawn on while this object lives (they nest).
class Paper
{
public:
    explicit Paper(const QColor& paper);
    ~Paper();
    Paper(const Paper&) = delete;
    Paper& operator=(const Paper&) = delete;

private:
    QColor m_previous;
};

/// The paper in use (white when none is set).
QColor paper();
/// Whether the paper in use is dark.
bool darkPaper();
/// Whether a colour is dark, as paper.
bool isDark(const QColor& paper);

/// The WCAG contrast ratio of two colours: 1 (the same) to 21.
double contrast(const QColor& a, const QColor& b);

/// A colour to draw with on the paper in use.
QColor on(const QColor& colour);
inline QColor on(Qt::GlobalColor colour) { return on(QColor(colour)); }
/// A pen whose colour is on() of its own.
QPen on(QPen pen);
/// A plain brush whose colour is on() of its own; others as they are.
QBrush on(QBrush brush);

/// The paper of the canvas in the dark theme, when the schematic is to
/// follow the theme.
inline QColor darkPaperColour() { return QColor(0x22, 0x24, 0x27); }

} // namespace qucs_s::ink

#endif
