/*
 * speclimits.h - a diagram's spec limits: upper and lower lines, levels and
 * piecewise masks its traces must keep within; where they do not, and the
 * verdict
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#ifndef QUCS_DIAGRAM_LIMITS_H
#define QUCS_DIAGRAM_LIMITS_H

#include <QList>
#include <QPointF>
#include <QString>
#include <QVector>

class Diagram;
class Graph;
struct Axis;

namespace qucs_s::limits {

/// A limit a diagram's traces keep within: below an upper one, above a
/// lower one. Its points run straight from one to the next, x rising, and
/// it holds between the first x and the last; two points of one x are a
/// step (a mask's edge), where the stricter holds. One point alone is a
/// level over every x. Its y is in the units its axis shows (dB on an axis
/// of dB).
struct Limit {
    enum Side { Upper = 0, Lower = 1 };
    Side side = Upper;
    QVector<QPointF> points;
    QString label;
    int axis = 0;   ///< 0 the left y axis, 1 the right
    int pane = 0;   ///< a stacked diagram's pane (0 the top one)

    /// The limit at \a x: NaN outside its x range. Straight between two
    /// points as they are drawn: in the logarithm of x on a logarithmic x
    /// axis (\a logX), of y on a logarithmic y axis (\a logY).
    double at(double x, bool logX = false, bool logY = false) const;
    /// The x range it holds over (all of it for a level).
    double from() const;
    double to() const;
    bool isLevel() const { return points.size() == 1; }

    /// As a diagram's file keeps it, in a line of its own:
    /// <Limit 0 0 0 "label" x,y;x,y>.
    QString save() const;
    static bool load(const QString& line, Limit* limit);
    bool operator==(const Limit&) const = default;
};

/// Where a trace is beyond a limit: from, to (its x), how far at worst,
/// and where.
struct Violation {
    int graph = -1;   ///< its index among the diagram's graphs
    int limit = -1;   ///< ... and among its limits
    double from = 0, to = 0;
    double worst = 0, worstAt = 0;
};

/// The values \a g shows of its curve \a curve, as its axis \a axis shows
/// them: x, and y (a complex value's magnitude, in dB on an axis of dB).
void curveOf(const Graph* g, int curve, const Axis* axis, QVector<double>* x, QVector<double>* y);

/// Every place a graph of \a d is beyond a limit of its axis (and pane),
/// its curves' together.
QList<Violation> check(const Diagram* d);

/// The limits of \a d no trace with data is drawn against (on their axis
/// and pane): they check nothing.
QList<int> unchecked(const Diagram* d);

/// "upper" or "lower".
QString sideName(Limit::Side side);

} // namespace qucs_s::limits

#endif
