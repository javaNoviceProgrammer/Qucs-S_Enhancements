/*
 * fem_import.h - drawings read into a geometry: a DXF's entities, an SVG's
 *                shapes, as the curves of paths - open or closed - in the
 *                file's own coordinates
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#ifndef QUCS_FEM_IMPORT_H
#define QUCS_FEM_IMPORT_H

#include "fem_geometry.h"

#include <QByteArray>
#include <QStringList>

#include <vector>

namespace qucs_s::fem {

/// A path of a drawing: its curves one after another (each from its t0 to
/// its t1, the next starting where it ends), closed or not.
struct ImportedPath {
    std::vector<Curve> curves;
    bool closed = false;
};

/// A drawing read: its paths, in its own coordinates (y up), and its unit
/// when it says one (metres in a unit of it; 0: it does not say).
struct ImportedDrawing {
    std::vector<ImportedPath> paths;
    double unit = 0;
    QStringList warnings;
    QString error;
};

/// An ASCII DXF: lines, arcs, circles, ellipses, (lightweight) polylines
/// with their bulges, splines, points ignored, a block's entities where it is
/// inserted; those on \a layers only (empty: all). Pieces that meet are
/// joined into paths.
ImportedDrawing readDxf(const QByteArray& data, const QStringList& layers = {});
/// An SVG: its paths (lines, Béziers, arcs), rectangles (rounded too),
/// circles, ellipses, lines, polylines and polygons, through their
/// transforms; y turned up.
ImportedDrawing readSvg(const QByteArray& data);

} // namespace qucs_s::fem

#endif // QUCS_FEM_IMPORT_H
