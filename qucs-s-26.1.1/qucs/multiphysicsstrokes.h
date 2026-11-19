/*
 * multiphysicsstrokes.h - the pen's strokes on a 2D multiphysics model's
 *                         Graphics view read against the geometry, for
 *                         Claude: the domains, boundaries and points each
 *                         stroke is round, on, points at or follows, and
 *                         the value the results show where it points
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#ifndef QUCS_MULTIPHYSICSSTROKES_H
#define QUCS_MULTIPHYSICSSTROKES_H

#include "markup.h"

#include <QJsonArray>
#include <QPointF>
#include <QString>

#include <functional>
#include <optional>

namespace qucs_s::fem {

struct Topology;

/// What the Graphics view's results show, read at a point: none where it
/// shows no color map (\a at null).
struct ShownValues {
    QString expression;                                   ///< "T", "es.normE"
    std::function<std::optional<double>(QPointF)> at;     ///< in SI; none outside the mesh
    std::function<QString(double)> said;                  ///< "312.4 K", in the plot's unit
    explicit operator bool() const { return static_cast<bool>(at); }
};

/// \a strokes, drawn on a model's geometry \a topology (null: none built) in
/// the geometry's unit, y up, in that order: a reading each, as
/// markup::readingOf() begins it, and by its gesture what it marks - each
/// a domain ("domain 2 (rect1)", by the number the selection lists give and
/// the object that made it), a boundary ("boundary 5 (between domains 1
/// and 2)") or a point ("point 3 at (0, 1)"):
/// - a ring: "around" (the domains, boundaries and points inside it, a
///   domain at least half inside), "around part of" (domains partly);
/// - a dot, a mark: "on" (the point it is on, else the boundary, else the
///   domain);
/// - a corner: "at", what its point is on;
/// - a line, an arrow: "from" and "points at" (what its ends are on: an
///   arrow's tail, its end farther from its tip, and its tip); "along" (the
///   boundaries it follows); an arrow's "tip" and "tail";
/// - a highlighter's line or mark: "highlights" alone (the boundaries it
///   follows, else the domains it lies on); its arrow that and an arrow's.
/// With \a values: the value where it points ("value at", a dot's, a mark's
/// middle, a corner's point, an arrow's tip; a pen's line's "value at its start"
/// and "value at its end"), and a ring's "least inside" and "greatest
/// inside" with where. \a points: its points simplified.
QJsonArray readModelStrokes(const Topology* topology, const QList<markup::Stroke>& strokes, bool points, const ShownValues& values = {});

/// Readings a sentence each, a line each, for a prompt: "the model, stroke
/// 2: a red pen ring around domain 3 (rect1) ... (its box 0, 0 to 1, 1 µm)"
/// - the latest \a most of them, the earlier ones counted; those marked
/// addressed left out, and named. \a unit: the geometry's.
QString modelSentences(const QJsonArray& readings, const QString& unit, int most = 30);

} // namespace qucs_s::fem

#endif
