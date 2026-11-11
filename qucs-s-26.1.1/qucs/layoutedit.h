/*
 * layoutedit.h - a GDSII or OASIS layout changed in place for Claude
 *                (edit_layout): shapes, paths, texts and placements added,
 *                deleted, replaced or moved in a cell, all or none, read
 *                and written with gdstk as the router writes its routes
 *                (layoutrouter/router_gds.h); and what changed between two
 *                reads of a layout, by cell and layer
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#ifndef QUCS_LAYOUTEDIT_H
#define QUCS_LAYOUTEDIT_H

#include "layout.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QString>

namespace qucs_s::layout {

struct EditResult {
    bool ok = false;
    QString error;
    QString cell;        ///< the cell changed
    QJsonArray done;     ///< what each edit did, in words
};

/*!
 * \a edits applied in turn to \a cell (empty: the largest top cell) of the
 * layout \a file, all of them or none, and the file written back whole in
 * its place (a temporary file renamed over it). Each edit is one of:
 *
 *   {"add": "polygon", "layer": "2/0", "points": [[x, y], ...]}
 *   {"add": "path", "layer": "2/0", "points": [...], "width": µm, "ends": "flush"|"round"|"extended"}
 *   {"add": "text", "layer": "10/0", "text": "OUT", "at": [x, y]}
 *   {"add": "placement", "of": "PAD", "at": [x, y], "rotation": degrees, "mirrored": bool, "magnification": m}
 *   {"delete": kind, ...what}             what is picked, every one picked
 *   {"replace": kind, ...what, ...new}    the one picked: "points", "width", "ends", "layer_to", "text_to", "at_to", "rotation", "mirrored", "magnification"
 *   {"move": kind, ...what, "by": [dx, dy]}
 *
 * where kind is polygon, path, shape (either), text or placement, and
 * what picks it in the cell's own elements: "layer" (a shape's or a
 * text's) with "at": [x, y] - the smallest there (a path: within half its
 * width of its spine; a text: within a µm of its place; a placement: its
 * box) - or "box": [x0, y0, x1, y1], those wholly inside, or (a deletion)
 * "box_is": [x0, y0, x1, y1], the one whose own box it is; a text's
 * "text", a placement's "of". All in µm, the cell's own coordinates.
 */
EditResult editLayout(const QString& file, const QString& cell, const QJsonArray& edits);

/// A layout with nothing in it - one cell, \a cell ("TOP" where empty), its
/// unit the µm and its grid the nm - written to \a file, a .gds or an .oas;
/// false, and why, when it cannot be. (RF 2.5D's drawing begins one beside
/// a model that names no layout.)
bool newLayout(const QString& file, const QString& cell, QString* error = nullptr);

/// What changed from \a before to \a after, two reads of a layout: the
/// cells added and taken away; in each cell there in both, by layer, the
/// shapes added and taken away (a changed one is both), with the boxes of
/// the first \a most; the texts and the placements likewise. Empty when
/// nothing did.
QJsonObject differences(const Layout& before, const Layout& after, int most = 5);

} // namespace qucs_s::layout

#endif
