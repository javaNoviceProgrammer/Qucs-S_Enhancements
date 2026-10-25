/*
 * rf25_mesh.h - the cells an RF 2.5D model's conductors are divided into,
 *               and what they would cost to solve
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * The last piece of phase A of
 * docs/proposals/2026-10-23-rf-2p5d-moment-method.md. It divides the
 * processed shapes into cells and counts what a solver would have to hold -
 * "the panel should show the unknown count before solving and refuse what
 * it cannot hold" - but it computes no fields: the basis functions, the
 * Green's functions and the matrix are phases B and C.
 *
 * The cells are axis-aligned squares cut against each conductor, which is
 * the rooftop mesh a planar layout wants: exact on the rectangles a layout
 * is mostly drawn with, and cut to the outline elsewhere. Coordinates stay
 * the layout's micrometres.
 */
#ifndef QUCS_RF25_MESH_H
#define QUCS_RF25_MESH_H

#include "rf25_layout.h"

#include <QList>
#include <QMap>
#include <QPolygonF>
#include <QString>
#include <QStringList>

namespace qucs_s::rf25 {

/// One cell of the mesh.
struct MeshCell {
    QString layer;       ///< the stack layer it is on
    QPolygonF outline;   ///< µm
    QPointF centre;      ///< µm
    double area = 0;     ///< µm²
    bool onEdge = false; ///< it touches the conductor's boundary
};

/// The rf_mesh node read.
struct MeshSettings {
    int perWavelength = 20;   ///< cells across a wavelength
    double maximum = 0;       ///< the largest cell, m (0: the wavelength alone decides)
    bool edges = true;        ///< smaller cells where the current crowds
    int skin = 0;             ///< layers through the skin depth (0: a surface impedance)
    /// At least this many cells across each conductor's narrowest part (0:
    /// none - the wavelength and the largest cell alone). The charge
    /// crowds at a conductor's edges, so the accuracy of its capacitance
    /// is in how finely its width is cut, not its length: a cell set by
    /// the wavelength alone was wider than a strip (the phase B check of
    /// 24 October: three cells for 1000 by 100 µm, Z0 12 % out).
    int cellsAcross = 10;
    /// How many times longer than they are wide a straight strip's cells
    /// may be along it: its charge hardly changes along a line, so the
    /// unknowns are better spent across it.
    double stretch = 4;
};
MeshSettings meshSettingsOf(const Node& node, const QList<fem::Model::Parameter>& parameters, QStringList* problems);

/// How many cells the charge solve of a static extraction will set up a
/// dense matrix of: 8000 is half a gigabyte a matrix and some thirty
/// seconds a solve on a desktop of 2026.
constexpr int kMostCharges = 8000;

/// What a mesh would cost a solver to hold.
struct MeshCost {
    int unknowns = 0;         ///< the edges two cells share: where current flows
    double matrixBytes = 0;   ///< a dense complex matrix of that order
    bool tooBig = false;      ///< beyond what this build will hold
    QString said;             ///< the count and the memory, for the panel
};

struct Mesh {
    QList<MeshCell> cells;
    MeshCost cost;
    QStringList problems;
    QStringList lines;        ///< what it did, as the processing reports its own
    double cellSize = 0;      ///< the size it aimed at, µm
    bool ok() const { return problems.isEmpty(); }
    QMap<QString, int> byLayer() const;
};

/*!
 * \a conductors divided into cells. \a top is the highest frequency to be
 * solved (Hz) and \a stack the layers, which together give the wavelength
 * the cell size comes from; \a micrometre is how many metres one unit of
 * the polygons is.
 *
 * A conductor smaller than one cell is one cell. Where \a how.edges is on,
 * the cells that touch a conductor's boundary are divided once more, which
 * is where the current crowds.
 */
Mesh meshOf(const QList<Conductor>& conductors, const MeshSettings& how, const Stack& stack, double top,
            double micrometre = 1e-6);

/// What a mesh of that many unknowns would cost, and whether this build
/// will hold it (a dense matrix of complex doubles, both triangles).
MeshCost costOf(int unknowns);

/// What the charge solve of \a cells cells costs, said: one unknown a
/// cell, a dense real matrix of them, and whether it is more than this
/// build solves (kMostCharges). The mesh has two counts - its cells, the
/// charge solve's, and the edges between them, a full-wave solve's - and
/// they were said side by side with the second's matrix (the phase B
/// check of 24 October).
QString chargeCostOf(int cells);

/// The highest frequency of the model's study, Hz; 0 when it says none.
double topFrequency(const Model& model);

} // namespace qucs_s::rf25

#endif
