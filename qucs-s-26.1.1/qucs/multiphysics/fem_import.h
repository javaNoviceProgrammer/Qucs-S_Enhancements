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
#include <QMutex>
#include <QRectF>
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

/// A layer of a layout and its datatype: "1/0".
struct LayoutLayer {
    quint32 layer = 0, datatype = 0;
    bool operator==(const LayoutLayer& o) const { return layer == o.layer && datatype == o.datatype; }
    bool operator<(const LayoutLayer& o) const { return layer != o.layer ? layer < o.layer : datatype < o.datatype; }
    QString text() const { return QStringLiteral("%1/%2").arg(layer).arg(datatype); }
};
/// "1/0", "1" (datatype 0), "1/*" (any datatype: -1 in \a any); false when
/// it is not one.
bool parseLayoutLayer(const QString& text, LayoutLayer* layer, bool* anyDatatype = nullptr);

/*!
 * A layout's cell as shapes on layers: GDSII (gzipped too) or OASIS, read by
 * gdstk, the cell flattened (its placements and arrays, paths as their
 * outlines), each layer's polygons made one (their union: outlines with
 * their holes, holes clockwise). In µm.
 */
struct LayoutShapes {
    struct Layer {
        LayoutLayer key;
        std::vector<std::vector<QPointF>> loops;   ///< outlines counterclockwise, holes clockwise
    };
    std::vector<Layer> layers;                     ///< by layer and datatype
    QString cell;                                  ///< the cell read
    QStringList cells;                             ///< every cell of the file
    QRectF bounds;
    QStringList warnings;
    QString error;
    const Layer* layer(const LayoutLayer& key) const;
};
/// Reads cell \a cell (empty: its largest top cell) of the layout at
/// \a path, the layers \a layers ("1/0, 2/0", "1/*"; empty: all), what
/// meets \a window only (µm; empty: all).
LayoutShapes readLayoutShapes(const QString& path, const QString& cell = {}, const QString& layers = {}, const QRectF& window = {});
/// Whether \a path is a layout by its suffix (.gds, .gds2, .gdsii, .gds.gz,
/// .oas, .oasis).
bool isLayoutPath(const QString& path);
/// Where the cut from \a a to \a b crosses \a loops' insides: intervals of
/// the distance along it (from \a a, in its unit), in order.
std::vector<std::pair<double, double>> cutIntervals(const std::vector<std::vector<QPointF>>& loops, QPointF a, QPointF b);

/// gdstk tells what it reads to one global log: one reading at a time (the
/// layout viewer's reads too).
QMutex& gdstkLock();

} // namespace qucs_s::fem

#endif // QUCS_FEM_IMPORT_H
