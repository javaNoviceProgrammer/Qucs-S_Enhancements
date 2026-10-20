/*
 * fem_process_p.h - a process flow's insides: Clipper's integer polygons
 *                   (a process grid's 64th each) and what is done to them
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#ifndef QUCS_FEM_PROCESS_P_H
#define QUCS_FEM_PROCESS_P_H

#include <clipper/clipper.hpp>

#include <QString>

#include <utility>
#include <vector>

namespace qucs_s::fem::process {

using cInt = ClipperLib::cInt;
using Path = ClipperLib::Path;
using Paths = ClipperLib::Paths;

/// Their union (nonzero), outlines counterclockwise, holes clockwise.
Paths unite(const Paths& a);
/// a op b: union, difference, intersection.
Paths clip(ClipperLib::ClipType type, const Paths& a, const Paths& b);
/// Grown by \a by (shrunk: negative), round, arcs within \a arcTolerance.
Paths offset(const Paths& a, double by, double arcTolerance);
Paths rectangle(cInt x0, cInt y0, cInt x1, cInt y1);
/// What \a a sweeps moving by (dx, dy): it, it moved and between - holes
/// kept as they are swept.
Paths sweep(const Paths& a, cInt dx, cInt dy);
double area(const Paths& a);
/// Its lowest and highest y.
std::pair<cInt, cInt> yRange(const Paths& a);
/// Each point on a multiple of \a step, made clean again.
void snap(Paths& a, cInt step);
/// Its connected pieces, each an outline and its holes.
std::vector<Paths> components(const Paths& a);

/// A region as the flow works on it: a step's material, integer polygons.
struct Region {
    QString name, material;
    bool crystal = false;
    Paths p;
};

} // namespace qucs_s::fem::process

#endif // QUCS_FEM_PROCESS_P_H
