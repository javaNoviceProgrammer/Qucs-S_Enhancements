/*
 * strokeshape.h - a freehand stroke read as the polyline it was meant to
 *                 be: its straight runs and their ways, the corners between
 *                 them (rounded, and how much), each run's place snapped to
 *                 what is near it or to a round value, and whether it is
 *                 near symmetric - for Claude, who is asked to "make it look
 *                 like this"
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#ifndef QUCS_STROKESHAPE_H
#define QUCS_STROKESHAPE_H

#include <QJsonObject>
#include <QList>
#include <QPolygonF>
#include <QString>

#include <optional>

namespace qucs_s::markup {

/// A place a run, an end or an axis can be taken to: a value of x (or of
/// y) and what is there ("the 2/0 path's centre line").
struct Snap {
    double value = 0;
    QString what;
    int rank = 0;   ///< within reach, the lowest rank first, then the nearest: a centre line before an edge
};

/// What a stroke's runs may snap to: the places of what is near it, across
/// (\a xs: values of x) and up (\a ys); else a round value no finer than
/// \a finest (a layout's database unit).
struct Snaps {
    QList<Snap> xs, ys;
    double finest = 0.001;
    /// Lines in the layout a sketch may be centred on or not - a path's
    /// middle, midway between two placements, the cell's centre -: upright
    /// (values of x) and level (values of y), the lowest rank first.
    QList<Snap> uprights, levels;
};

/// A stroke read as a polyline (polylineOf()).
struct Polyline {
    enum class Way { Horizontal, Vertical, Diagonal, Slanted };
    struct Run {
        Way way = Way::Slanted;
        double angle = 0;          ///< degrees, the way it was drawn: 0 to +x, counterclockwise
        double at = 0;             ///< a horizontal run's y, a vertical one's x, as drawn
        double spread = 0;         ///< the furthest its points stray from it
        bool snapped = false;      ///< (horizontal and vertical runs alone)
        double snappedAt = 0;
        QString snappedTo;         ///< what it snapped to: what is there, or "a 1 µm grid"
    };
    bool closed = false;
    QList<Run> runs;
    QPolygonF drawn;               ///< its corners as drawn, its ends first and last (closed: the first not again)
    QPolygonF snapped;             ///< the same, its runs and ends snapped
    QStringList endsSnappedTo;     ///< what each end snapped to (empty: a round value)
    QList<double> angles;          ///< each corner's, degrees between its runs (drawn's interior points in turn)
    QList<double> radii;           ///< each corner's radius as drawn, 0 sharp
    /// Its corners' own mirror line (an open one's ends apart): x = axisAt
    /// ("x") or y = axisAt ("y"), within so much. Said only against a line
    /// of the layout (Snaps::uprights, levels): a level-bottomed step is
    /// always symmetric about its own middle, so what tells is whether that
    /// middle is the layout's - centred on it, or so far off it.
    QString axis;
    double axisAt = 0;
    double within = 0;
    bool referenced = false;
    double reference = 0;
    QString referenceIs;
    bool centred = false;
};

/// \a points, drawn \a width broad, read as a polyline - \a closed, a ring's
/// - when they are one: two to a dozen straight runs (a closed one up to
/// six), none of them curved, the whole no rounder than a circle; else
/// none. Its runs are snapped to \a snaps.
std::optional<Polyline> polylineOf(const QPolygonF& points, double width, bool closed, const Snaps& snaps);

/// A polyline as a reading has it, and in words for a prompt.
QJsonObject polylineJson(const Polyline& polyline);
QString polylineSaid(const QJsonObject& polyline);

/// \a value on the coarsest round grid (100, 50, 10, 5, 1, 0.5 ... µm, no
/// finer than \a finest) within \a tolerance of it; that grid's step in
/// \a step.
double onRoundGrid(double value, double tolerance, double finest, double* step = nullptr);

/// A number as the readings write it: to a thousandth, no more digits than
/// it has.
QString numberSaid(double value);
QString pointSaid(QPointF point);

} // namespace qucs_s::markup

#endif
