/*
 * biaslabels.h - where the DC bias labels of a schematic go: beside their
 * node, clear of the symbols, texts, wires and of one another
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#ifndef QUCS_BIASLABELS_H
#define QUCS_BIASLABELS_H

#include <QLine>
#include <QList>
#include <QPoint>
#include <QRect>
#include <QSize>

namespace qucs_s::bias {

/// A DC bias label to place: the value of a node (a voltage) or of the
/// probe or source at it (a current).
struct Label {
    QPoint anchor;          ///< the node, in model coordinates
    QSize size;             ///< the label's box
    bool current = false;   ///< a current: it prefers the right-hand side, a voltage the left
};

/// What a label had better not cover.
struct Obstacles {
    QList<QRect> boxes;      ///< symbols, their texts, node dots, wire labels, diagrams, paintings
    QList<QLine> wires;
};

/// Where a label goes.
struct Placement {
    QRect box;
    bool leader = false;   ///< set apart from its node: a line joins them
    double cost = 0;       ///< what it still covers (0: nothing)
};

/// The candidate boxes for a label, in the order they are preferred: the
/// four corners beside the anchor (upper left first for a voltage, upper
/// right for a current - where the fixed offsets used to put them), then
/// the four sides, then the same eight a step further out.
QList<QRect> candidates(const Label& label);

/// How much of \a box is covered: by labels already placed (weighed most),
/// by boxes (area) and by wires (length times the box's height).
double cost(const QRect& box, const Obstacles& obstacles, const QList<QRect>& placed);

/// Places every label where it covers the least of the labels placed
/// before it and, among those, the least of the rest, preferring the
/// nearer and the earlier candidates when that is equal; the labels with
/// the fewest free places go first.
/// When nothing near the node is free the label may go a step further
/// out, with a leader line; when nothing is free at all, it goes where it
/// covers the least. The result is in the order of \a labels.
QList<Placement> place(const QList<Label>& labels, const Obstacles& obstacles);

} // namespace qucs_s::bias

#endif
