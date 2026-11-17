/***************************************************************************
                                rf25_palace.h
                               ---------------
 *  An RF 2.5D model written as a project of Palace, the open-source 3D
 *  finite-element electromagnetic solver: its mesh (Gmsh's MSH 2.2, of
 *  tetrahedra, in µm) and its configuration (JSON) for a driven solve over
 *  the study's sweep, every port driven in turn - ready to run as it is:
 *  "palace -np 4 <name>.json" in the folder it is written to. No solve is
 *  needed for it, and no Palace here.
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 ***************************************************************************/

#ifndef QUCS_RF25_PALACE_H
#define QUCS_RF25_PALACE_H

#include "rf25_layout.h"
#include "rf25_model.h"

#include <QByteArray>
#include <QList>
#include <QRectF>
#include <QString>
#include <QStringList>

namespace qucs_s::rf25 {

/// How a model is written for Palace. Lengths in metres; nought or below
/// nought: chosen.
struct PalaceOptions {
    /// How far the stack reaches beyond the shapes on every side, where an
    /// absorbing boundary closes it (chosen: five heights of the stack). A
    /// lid's side walls give the box instead.
    double margin = -1;
    /// The box's sides where the Cover has no walls: "perfect", perfect
    /// conductors (chosen: five heights of the stack out), or "absorbing", a
    /// first-order absorbing boundary (a quarter wave out). An absorbing side
    /// near a line takes its field along it as a resistive wall would.
    QString sides = QStringLiteral("perfect");
    /// The air over the stack up to the absorbing boundary above it (chosen:
    /// a quarter wave at the top of the sweep). A metal lid closes it at its
    /// height instead; an open ground has as much air under the stack.
    double air = -1;
    /// The largest element (chosen: a tenth of the wavelength in the
    /// slowest dielectric at the top of the sweep).
    double largest = 0;
    /// The elements along the metals' edges and the ports (chosen: a quarter
    /// of the narrowest conductor, as twice its area over its perimeter says).
    double edges = 0;
    /// Each metal as a sheet at its bottom, as RF 2.5D solves a sheet - its
    /// thickness in its loss alone (Palace's conductivity boundary of that
    /// thickness) -, rather than cut out of the mesh as thick as it is.
    bool sheets = false;
    /// The finite elements' order (Palace's Solver Order: 1 to 4).
    int order = 2;
    /// Palace's linear solver: "default", Palace's own choice - a sparse
    /// direct solver where it was built with one, under its p-multigrid -,
    /// or "ams", auxiliary-space multigrid alone. On a microstrip the default took a third of the time in the same memory.
    QString solver = QStringLiteral("default");
    /// The adaptive sweep's tolerance (Palace's AdaptiveTol): a few
    /// frequencies solved and the rest from a reduced model through them.
    /// Nought: every frequency solved. A sweep of two points or fewer is
    /// solved whole.
    double adaptiveTolerance = 1e-3;
};

/// What is written, and what a reader must know.
struct PalaceExport {
    QString name;             ///< the files' base name
    QByteArray mesh;          ///< <name>.msh: Gmsh MSH 2.2 (ASCII), µm
    QByteArray config;        ///< <name>.json
    QStringList notes;        ///< where it is not what RF 2.5D solves
    QStringList problems;     ///< why nothing can be written
    QStringList portSaid;     ///< each port as written: "P1: lumped port 1, ..."
    int nodes = 0, tetrahedra = 0, triangles = 0;
    int ports = 0;            ///< the measuring ports, driven in turn
    int lumped = 0;           ///< the lumped elements, written as undriven RLC ports
    int materials = 0;
    /// µm: what was used.
    double marginUsed = 0, airUsed = 0, largestUsed = 0, edgesUsed = 0;
    double volume = 0;        ///< µm³: its tetrahedra's, the conductors cut out
    double floor = 0, ceiling = 0;   ///< µm: the box's in z
    QRectF box;               ///< µm: the box's in the plan
    bool ok() const { return problems.isEmpty(); }
};

/// More tetrahedra than this is refused, not written: larger elements.
constexpr int kPalaceMostTetrahedra = 6000000;

/*!
 * \a model and its shapes, \a conductors as the processing leaves them (µm,
 * each on a metal or a via of the stack), written for Palace's driven
 * solve. \a name is the files' base name (empty: "rf25"), \a madeBy what
 * the configuration says wrote it.
 *
 * The stack is laid across a box round the shapes (\a options' margin, or a
 * lid's side walls), each slab the material it is - a metal's room the
 * dielectric over it - as RF 2.5D has the stack without end across the
 * plane: closed at the box's sides by an absorbing boundary. A metal and
 * a via are cut out of the mesh, each surface a perfect conductor or of its
 * conductivity - or, \a options' sheets, a metal is a sheet at its bottom.
 * The ground is the box's floor: perfect, of its conductivity, or of its
 * surface impedance; open, air under the stack to an absorbing floor. A
 * metal lid is its ceiling, else air to an absorbing one.
 *
 * The mesh is the plan meshed in triangles - every shape's outline and
 * every port's edge in it, finer along them - swept through the stack's
 * slabs into prisms, each split into three tetrahedra (along diagonals of
 * the lowest-numbered node, so that neighbours share their faces).
 *
 * Each measuring port is a lumped port, driven in turn ("Excitation" its
 * own index: the whole S matrix): an edge port against the ground a
 * rectangle under its face down to the ground - or the metal under it -,
 * against the conductors beside it two rectangles across its gaps; an
 * internal port a rectangle up from its From metal (or the ground) to its
 * To metal, along x across its width (across y where its height is the
 * larger; an element's size where it has neither). A lumped element is an
 * undriven lumped port of its R, L and C (in parallel, or one alone).
 */
PalaceExport palaceExportOf(const Model& model, const QList<Conductor>& conductors, const PalaceOptions& options = {},
                            const QString& name = {}, const QString& madeBy = {});

/// \a written's two files, <name>.json at \a configPath and the mesh beside
/// it (its name as the configuration says it: <name>.msh). The paths written
/// in \a files (the configuration first), or false and why in \a error.
bool writePalaceExport(const PalaceExport& written, const QString& configPath, QStringList* files, QString* error);

} // namespace qucs_s::rf25

#endif
