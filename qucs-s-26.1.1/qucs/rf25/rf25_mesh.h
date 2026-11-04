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

#include <QHash>
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
    /// The most cells the mesh is cut into before fewer are put across a
    /// conductor; 0: what the solver it is for holds - kMostCharges for the
    /// quasi-static extraction's dense charge solve, kMostCompressedCells
    /// for a full-wave solve held compressed.
    int most = 0;
    /// Whether it is for a full-wave solve held compressed (the study's
    /// Solver and Matrix): its cost said so, and its cells not held to the
    /// charge solve's bound. Not read from the node: the tab sets it.
    bool compressed = false;
    /// Whether a side that faces another conductor across a gap narrower
    /// than its outermost cells has them halved until they are no wider
    /// than a quarter of the gap, where the charge and the current crowd on
    /// both edges. The tab sets it for both studies, and a full-wave one's
    /// calibration strips are meshed so too; not read from the node.
    bool gaps = false;
    /// The most cells, as the solver has it.
    int mostCells() const;
};
MeshSettings meshSettingsOf(const Node& node, const QList<fem::Model::Parameter>& parameters, QStringList* problems);

/// How many cells the charge solve of a static extraction will set up a
/// dense matrix of: 8000 is half a gigabyte a matrix and some thirty
/// seconds a solve on a desktop of 2026.
constexpr int kMostCharges = 8000;
/// How many cells a mesh for a full-wave solve held compressed is cut into
/// before fewer are put across a conductor: some 120 000 unknowns, two to
/// three gigabytes and ten seconds a frequency on a desktop of 2026 (the
/// solve itself takes what the machine's memory allows).
constexpr int kMostCompressedCells = 40000;

/// What a mesh would cost a solver to hold.
struct MeshCost {
    /// The edges two cells share: where current flows - and, for a mesh for
    /// a full-wave solve held compressed, its cells too: that solve's
    /// unknowns.
    int unknowns = 0;
    double matrixBytes = 0;   ///< a dense complex matrix of that order, or the compressed one's
    bool tooBig = false;      ///< beyond what this build will hold
    QString said;             ///< the count and the memory, for the panel
};

struct Mesh {
    QList<MeshCell> cells;
    /// The charges a quasi-static solve of it has: its cells, a metal solved
    /// as its two faces counting each twice (the walls carry no charge of
    /// their own).
    int charges = 0;
    MeshCost cost;
    QStringList problems;
    QStringList lines;        ///< what it did, as the processing reports its own
    double cellSize = 0;      ///< the size it aimed at, µm
    /// How many conductors' sides it graded where they face another across a
    /// narrow gap (MeshSettings::gaps).
    int facing = 0;
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
/// will hold it: a dense matrix of complex doubles, both triangles - or,
/// \a compressed, held compressed, some 20 kB an unknown as measured
/// (rf25_fast.h).
MeshCost costOf(int unknowns, bool compressed = false);

/// What the charge solve of \a cells cells costs, said: one unknown a
/// cell, a dense real matrix of them, and whether it is more than this
/// build solves (kMostCharges). The mesh has two counts - its cells, the
/// charge solve's, and the edges between them, a full-wave solve's - and
/// they were said side by side with the second's matrix (the phase B
/// check of 24 October).
QString chargeCostOf(int cells);

/// The highest frequency of the model's study, Hz; 0 when it says none.
double topFrequency(const Model& model);

/*!
 * \a stack, \a conductors and \a mesh with every metal solved as its two
 * faces (Layer::twoSheets) made into what the solvers take, which know
 * sheets and vias and nothing of a metal's thickness: its bottom face the
 * metal itself, its top a metal layer of its own at its thickness over it,
 * and its walls a via layer joining the two along its edges - each cell
 * with a side on its conductor's edge joined to the same cell of its top.
 * The room between the two is what is over the metal, a dielectric or the
 * air, so that what is beside the walls is what fills the gap there; the
 * layers above it are then as high as the stack draws them, and a via up
 * from it leaves its top. The rest is as it was.
 *
 * The mesh's own cells keep their places; the top faces' cells and the
 * walls' come after them. A metal that is to be two sheets and has no
 * thickness is one.
 */
struct Thickened {
    Stack stack;
    QList<Conductor> conductors;
    Mesh mesh;
    bool any = false;              ///< whether any metal is two sheets
    int cells = 0;                 ///< the mesh's own cells, which come first
    int walls = 0;                 ///< the walls' cells, which come last
    QHash<int, int> topOf;         ///< a bottom face's cell, and the same cell of its top
    QHash<int, int> bottomOf;      ///< and back
    QStringList said;              ///< what was done, in words
    /// Whether \a layer of the stack is a metal's top face or its walls:
    /// what the solvers made and the caller did not.
    bool made(const QString& layer) const;
};
Thickened thickened(const Stack& stack, const QList<Conductor>& conductors, const Mesh& mesh);

} // namespace qucs_s::rf25

#endif
