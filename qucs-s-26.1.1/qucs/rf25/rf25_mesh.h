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
 * an earlier note It divides the
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
#include <QRectF>
#include <QString>
#include <QStringList>

#include <cmath>

namespace qucs_s::rf25 {

/// One cell of the mesh.
struct MeshCell {
    QString layer;       ///< the stack layer it is on
    QPolygonF outline;   ///< µm
    QPointF centre;      ///< µm
    double area = 0;     ///< µm²
    bool onEdge = false; ///< it touches the conductor's boundary
    double share = 1;    ///< how much of it is metal: its conductor's (Conductor::share)
    /// The rectangle its charge is spread over in the solvers' closed
    /// forms: its box where it is one; a cell clipped by a slanted side, a
    /// rectangle of its own area at its own centroid, as its box is in
    /// shape. Spread over its box - up to twice its area, its middle off
    /// its centroid -, a square turned 45 degrees read its C 8, 3.5 and
    /// 1.9 % over the square's at cells of a fifth, a tenth and a
    /// twentieth of its side.
    /// A strip's cell (Conductor::spine) lies in the frame of its segment of
    /// the centreline: \a angle the way that segment runs (radians from x),
    /// \a frame its cell's size along it and across it, as a rectangle of
    /// its own area. Nought and empty: a cell of the layout's own axes.
    double angle = 0;
    QSizeF frame;
    bool turned() const { return frame.isValid() && angle != 0; }
    /// \a p (µm, as the outline) in the cell's frame: turned about its centre
    /// by -angle, so that charged() is a rectangle of that frame's axes
    /// about it. As it is for a cell of the layout's own axes.
    QPointF intoFrame(QPointF p) const { return turnedAbout(p, centre, -angle); }
    /// \a p turned about \a about by \a by (radians): what a turned cell's
    /// frame takes a point to, in any units.
    static QPointF turnedAbout(QPointF p, QPointF about, double by)
    {
        if (by == 0) return p;
        const double c = std::cos(by), s = std::sin(by), x = p.x() - about.x(), y = p.y() - about.y();
        return QPointF(about.x() + c * x - s * y, about.y() + s * x + c * y);
    }
    /// The rectangle its charge is spread over, in its own frame (see
    /// above): a turned cell's, of its frame's size about its centre - the
    /// solvers' closed forms taken at a point turned into that frame.
    QRectF charged() const
    {
        if (frame.isValid()) return QRectF(centre - QPointF(frame.width() / 2, frame.height() / 2), frame);
        const QRectF box = outline.boundingRect();
        const double whole = box.width() * box.height();
        if (!(area > 0) || !(whole > 0) || area >= (1 - 1e-9) * whole) return box;
        const double k = std::sqrt(area / whole);
        const QSizeF size(box.width() * k, box.height() * k);
        return QRectF(centre - QPointF(size.width() / 2, size.height() / 2), size);
    }
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
    /// the wavelength alone was wider than a strip (three cells for 1000 by 100 µm, Z0 12 % out).
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
    /// Whether it is for a quasi-static solve, whose charge is held
    /// compressed past the dense limit (kMostCharges) to kMostCompressedCells:
    /// its cells not refused at the dense limit, and said so. Not read from
    /// the node: the tab sets it.
    bool chargeCompressed = false;
    /// Whether a side that faces another conductor across a gap narrower
    /// than its outermost cells has them halved until they are no wider
    /// than a quarter of the gap, where the charge and the current crowd on
    /// both edges. The tab sets it for both studies, and a full-wave one's
    /// calibration strips are meshed so too; not read from the node.
    bool gaps = false;
    /// How wide, as a share of the gap, the outermost cells facing it are
    /// halved down to (\a gaps): the Mesh node's Gap share, which a
    /// convergence check's meshes refine with the rest (refinedBy()).
    double gapShare = 0.25;
    /// The layout's plane of symmetry (Processing), whose half the
    /// conductors are: 1 x = symmetryAt, 2 y = it (µm, the conductors'
    /// units); 0 none. A side lying on it is no edge - the conductor's mirror
    /// is beyond it -, and a piece cut across by it is as wide as it and its
    /// mirror together. Not read from the node: the tab sets it.
    int symmetryAxis = 0;
    double symmetryAt = 0;
    /// The most cells, as the solver has it.
    int mostCells() const;
};
MeshSettings meshSettingsOf(const Node& node, const QList<fem::Model::Parameter>& parameters, QStringList* problems);
/// \a how with every size it sets divided by \a factor - the cells across
/// a conductor and per wavelength times it (at least one), the largest cell
/// and the share of a gap its facing cells are graded to over it -, the
/// edges' halving and the stretch along a line as they were: the same mesh,
/// \a factor times finer (a convergence check's meshes).
MeshSettings refinedBy(const MeshSettings& how, double factor);

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
    /// What makes a solve on it not to be trusted - said among the lines
    /// too: conductors that touch and that it does not join.
    QStringList warnings;
    double cellSize = 0;      ///< the size it aimed at, µm
    /// How many conductors' sides it graded where they face another across a
    /// narrow gap (MeshSettings::gaps).
    int facing = 0;
    /// What its cells are against what they cut, for the solve's note of it
    /// and the convergence check (µm): the narrowest conductor, how many
    /// cells were put across it and how wide they are there; the narrowest
    /// gap between two pieces of a layer facing each other (nought: none);
    /// and the smallest and the largest cell, each by its bounding box's
    /// shorter and longer side.
    double narrowest = 0;
    int across = 0;
    double acrossCell = 0;
    double narrowestGap = 0;
    /// Each layer's narrowest gap between two of its pieces facing each
    /// other across x or y, µm: what a metal's Solved as Automatic goes by
    /// (thickened()).
    QMap<QString, double> gapByLayer;
    double smallest = 0, largest = 0;
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
/// \a conductors with their mirrors across a plane of symmetry (1: x = \a at,
/// 2: y = it, in the conductors' units): each that reaches the plane united
/// with its own mirror - one outline, as the whole has it -, the rest beside
/// theirs. As they are where \a axis is nought.
QList<Conductor> withMirrors(const QList<Conductor>& conductors, int axis, double at);

/// What a mesh of that many unknowns would cost, and whether this build
/// will hold it: a dense matrix of complex doubles, both triangles - or,
/// \a compressed, held compressed, some 20 kB an unknown as measured
/// (rf25_fast.h).
MeshCost costOf(int unknowns, bool compressed = false);

/// What the charge solve of \a cells cells costs, said: one unknown a
/// cell, a dense real matrix of them, and whether it is more than this
/// build solves (kMostCharges). The mesh has two counts - its cells, the
/// charge solve's, and the edges between them, a full-wave solve's - and
/// they were said side by side with the second's matrix.
QString chargeCostOf(int cells, bool compressedPast = false);

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
    QHash<int, int> wallOn;        ///< each wall's cell, and the cell (its bottom face's) it stands on
    QStringList said;              ///< what was done, in words
    /// Whether \a layer of the stack is a metal's top face or its walls:
    /// what the solvers made and the caller did not.
    bool made(const QString& layer) const;
};
Thickened thickened(const Stack& stack, const QList<Conductor>& conductors, const Mesh& mesh);

/*!
 * The charge on the side walls of a metal solved as its two faces, which
 * neither face holds: across each narrow gap, a capacitance between the
 * edge cells of the two walls that face each other (WallGap), what a 2D
 * field solve of solid metal against its two faces alone leaves between
 * walls of that thickness that far apart (wallExcess()). Walls along x or
 * y, each with the nearest wall facing it across a gap no wider than
 * sixteen times the metal's thickness (past which it is under a per cent of
 * the gap's). \a micrometre is how many metres one unit of the mesh is.
 */
struct WallGap {
    int a = -1, b = -1;   ///< the two edge cells, of the thickened mesh
    double capacitance = 0;   ///< F
    double gap = 0, length = 0;   ///< m
    double epsr = 1;          ///< of what fills the gap
    bool alongX = false;      ///< its walls run along x (else along y)
    double lo = 0, hi = 0;    ///< where they face each other along them, the mesh's units
};
QList<WallGap> wallGapsOf(const Thickened& thick, double micrometre);
/// The capacitance per length a gap of \a tau = thickness / gap between two
/// metals' walls has beyond what their two faces alone hold, over the
/// permittivity of what fills it: a 2D finite-difference solve of two
/// conductors across a gap, solid against each as its two faces with no
/// walls, extrapolated in its grid. Nought below 1/32.
double wallExcess(double tau);

} // namespace qucs_s::rf25

#endif
