/*
 * layoutstrokes.h - the pen's strokes on a layout's cell read against what
 *                   is there, for Claude: what each stroke marks (the
 *                   shapes by layer, the texts, the cells placed), and what
 *                   it is about - the shapes much larger than it set apart,
 *                   the part of a large shape a ring holds, the feature an
 *                   arrow's tip is nearest, how a line follows a shape and
 *                   leaves it, and a freehand stroke as a polyline snapped
 *                   to what is near it
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#ifndef QUCS_LAYOUTSTROKES_H
#define QUCS_LAYOUTSTROKES_H

#include "layout.h"
#include "markup.h"

#include <QJsonArray>

namespace qucs_s::layout {

/// \a strokes, drawn on \a base.cell in that order, read against what
/// \a base finds there (its layers, depth and texts: what is seen): a
/// reading each, as markup::readingOf() begins it, with "cell" its name
/// and, by its gesture, what it marks - "around" (a ring), "on" (a dot or
/// a mark), "highlights", "from" and "points at" (a line or an arrow), "at"
/// (a corner's point) - each the shapes by layer, the texts and the cells
/// placed, those much larger than the stroke, under all of it, apart in
/// "over". And what it is about: "holds part of" (a ring round part of a
/// large shape: its vertices or segments inside), "nearest its tip",
/// "tip", "tail" and "comes from" (an arrow), "follows" (a line or a mark
/// along a path or an edge), "polyline" (a line, a corner, a mark or a
/// ring that is a few straight runs); \a points, its points simplified.
QJsonArray readStrokes(const Layout& layout, const Query& base, const QList<markup::Stroke>& strokes, bool points);

/// The most shapes and texts a reading of one stroke looks at.
constexpr int MostRead = 200000;

} // namespace qucs_s::layout

#endif
