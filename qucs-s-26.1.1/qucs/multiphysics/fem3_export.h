/*
 * fem3_export.h - a 3D model's exports: its fields for ParaView (VTU, a PVD
 *                 collection of them over output times and a sweep's points),
 *                 and Field on a Grid in 3D (CSV, NumPy, columns)
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#ifndef QUCS_FEM3_EXPORT_H
#define QUCS_FEM3_EXPORT_H

#include "fem3_results.h"
#include "fem_export.h"

#include <utility>
#include <vector>

namespace qucs_s::fem {

/// A 3D model's export, its files: its own (written where its File says,
/// or beside the model) and the parts beside it, each by the end of its name
/// after the export's base name (a PVD collection's VTUs: _0000.vtu...).
struct Export3 {
    ExportText main;
    std::vector<std::pair<QString, QByteArray>> parts;
};

/// Runs a 3D model's export node - Field on a Grid, VTU - on \a set, its
/// study's solutions; \a base the base name its file is written under (a
/// collection names its parts by it).
Export3 export3(const Solutions3& set, const Node& node, const QString& base);

/// A solution's VTU (VTK's XML unstructured grid, ASCII): its prisms as
/// wedges, or (\a quadratic) as quadratic wedges of 15 points - the corners,
/// then each edge's middle, read in its prism -, each expression a point
/// array (SI), each prism's domain (from 1) a cell array "dom", its points in
/// the geometry's unit. Empty, and why in \a error, where an expression does
/// not read.
QByteArray vtu3(const Solution3& solution, const std::vector<NamedExpression>& items, bool quadratic, QString* error = nullptr);

} // namespace qucs_s::fem

#endif // QUCS_FEM3_EXPORT_H
