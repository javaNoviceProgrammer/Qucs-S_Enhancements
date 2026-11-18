/*
 * rf25_layout.h - the shapes of an RF 2.5D model: the layer map applied,
 *                 and the processing steps that run before meshing
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * The steps are the table of an earlier note:
 * merge, merge vias, drop slivers, snap, heal, remove fill. Each says what
 * it changed, each can be turned off, and the shapes that come out are what
 * the mesher will see - so the view can draw them over the originals and
 * the user can check what the solver is given.
 *
 * Coordinates are the layout's own micrometres throughout, because that is
 * what layout.h hands over and what the view draws; the settings are
 * lengths in metres, as everything else in a model is, and are converted
 * here once.
 */
#ifndef QUCS_RF25_LAYOUT_H
#define QUCS_RF25_LAYOUT_H

#include "rf25_model.h"

#include <QMap>
#include <QPolygonF>
#include <QSet>
#include <QString>
#include <QStringList>

namespace qucs_s::layout {
struct Layout;
}

namespace qucs_s::rf25 {

/// \a polygon's area, positive whichever way it is wound.
double areaOf(const QPolygonF& polygon);
/// The length of \a polygon's boundary.
double perimeterOf(const QPolygonF& polygon);
/// How wide a conductor drawn as \a polygon is: the short side of the
/// rectangle of its area and its perimeter, no wider than its bounding box -
/// a strip's or a wire's own width, a square pad's side; a shape rounder
/// than any rectangle a quarter of its perimeter. (Twice the area over the
/// perimeter is half a square pad's side.)
double conductorWidthOf(const QPolygonF& polygon);
/// How thick against its narrowest conductor (conductorWidthOf()) a metal is
/// before its thickness is the inductance's: the full wave's said to read
/// high at a low frequency, and the loop inductance's runs taken as bars
/// Thinner, a strip's is within a
/// per cent or so of a bar's.
constexpr double kThickForInductance = 0.2;
/// \a shapes joined where they touch or overlap - along part of an edge too,
/// which gdstk's union alone does not always join. \a joined, given, counts
/// the pieces it had to join after that union.
QList<QPolygonF> unionOf(const QList<QPolygonF>& shapes, int* joined = nullptr);
/// Whether \a a and \a b share a stretch of an edge longer than \a tol (a
/// side of one on a side of the other, within \a tol of its line).
bool touchAlongAnEdge(const QPolygonF& a, const QPolygonF& b, double tol);
/// \a shapes grown by \a distance (negative: shrunk), in their own units.
QList<QPolygonF> offsetOf(const QList<QPolygonF>& shapes, double distance);
/// \a shapes united (gdstk's boolean "or"), µm: those that touch or overlap
/// one outline. \a shapes as they are where it fails.
QList<QPolygonF> unitedOf(const QList<QPolygonF>& shapes);
/// \a polygon without its jogs: each side shorter than \a under between two
/// sides along the other axis - a step of a few grid units in an outline
/// otherwise along x and y - taken out by moving the shorter of those two
/// sides across by it, so that every side stays along x or y (its two
/// points merged would slant them). How many were taken out added to
/// \a removed.
QPolygonF withoutJogs(const QPolygonF& polygon, double under, int* removed = nullptr);
/// \a polygon (a closed outline) thinned by Douglas and Peucker: every
/// corner within \a tolerance of the line between the corners kept either
/// side of it dropped, so that no point of the outline moves further than
/// that. How far the furthest dropped corner was from its line in \a moved.
/// As it was where \a tolerance is not above nought or it has four corners
/// or fewer.
QPolygonF simplifiedOutline(const QPolygonF& polygon, double tolerance, double* moved = nullptr);

/*!
 * Whether \a outline is a strip: a path's outline, or a polygon drawn as
 * one - two polylines \a width apart either side of a centreline, mitred
 * at its corners and flush at its ends -, and its centreline (\a spine)
 * and width where it is. Its corners must come in pairs across it, each
 * segment's two sides parallel and \a width apart, each end square to its
 * segment and \a width across, every side of either polyline running the
 * centreline's way; within \a tolerance (the outline's units). One whose
 * every side runs along x or y is none: the mesh cuts those into their
 * rectangles. A rectangle turned is one of a single segment, along its
 * longer way.
 */
bool stripOf(const QPolygonF& outline, double tolerance, QPolygonF* spine = nullptr, double* width = nullptr);
/// \a spine (an open polyline) thinned by Douglas and Peucker: every corner
/// within \a tolerance of the line between the corners kept either side of
/// it dropped, its ends and its sharper corners kept. How far the furthest
/// dropped corner was from its line in \a moved.
QPolygonF simplifiedSpine(const QPolygonF& spine, double tolerance, double* moved = nullptr);
/// The outline of a strip \a width wide along \a spine: mitred at its
/// corners, flush at its ends - stripOf()'s inverse. Its left side first,
/// from the spine's start; then its right, back.
QPolygonF stripOutline(const QPolygonF& spine, double width);
/// The part of \a shapes that is inside \a within: a cut, which is how a
/// cell of the mesh is taken out of a conductor.
QList<QPolygonF> clipTo(const QList<QPolygonF>& shapes, const QPolygonF& within);
/// \a shapes less \a less: what of the first is outside the second (an
/// ended dielectric's region beyond its shapes, rf25_dielectric.h).
QList<QPolygonF> differenceOf(const QList<QPolygonF>& shapes, const QList<QPolygonF>& less);

/// One polygon that will become a conductor: the layer of the stack it
/// belongs to, and its outline in the layout's micrometres.
struct Conductor {
    QString layer;          ///< a stack layer's name: "M1", "V1"
    QPolygonF outline;
    bool fill = false;      ///< dummy fill, kept as floating metal
    /// A strip's centreline and width, µm, where the processing found the
    /// shape to be one (stripOf()): its outline is then stripOutline() of
    /// them, and the mesh cuts it along its centreline in cells of its own
    /// frame. Empty, and nought, for any other shape.
    QPolygonF spine;
    double spineWidth = 0;
    bool isStrip() const { return spine.size() >= 2 && spineWidth > 0; }
    double area() const;    ///< µm², positive whichever way it is wound
};

/// What the processing did, in the order it did it.
struct LayoutReport {
    QStringList lines;      ///< "merged 412 vias into 7 shapes"
    int read = 0;           ///< shapes the map sent to a layer of the stack
    int ignored = 0;        ///< shapes on layers the map does not name
    int mergedFrom = 0;     ///< shapes a union replaced...
    int mergedTo = 0;       ///< ...and how many it left
    int viasFrom = 0;       ///< vias a merge replaced...
    int viasTo = 0;         ///< ...and how many it left
    double areaKept = 0;    ///< how much of the vias' area the merge kept (1: all)
    int slivers = 0;        ///< shapes dropped as too thin or too small
    int snapped = 0;        ///< shapes whose points moved to the grid
    int healed = 0;         ///< shapes a heal changed
    int fill = 0;           ///< dummy fill shapes dropped
    int cut = 0;            ///< shapes the Cutout cut along a region's edge...
    int masked = 0;         ///< ...and those it dropped, wholly outside what it keeps
    int simplified = 0;     ///< shapes whose curves were thinned (Processing::simplify)...
    int cornersDropped = 0; ///< ...the corners that went...
    double simplifiedMost = 0; ///< ...and the furthest the outline moved, µm
    QString summary() const;   ///< the lines joined, for a status bar or a tooltip
};

/// One region of the model's Cutout, read: its outline in the layout's
/// micrometres, whether what is inside it is kept or taken out, and the
/// layers it cuts.
struct CutRegion {
    QString name;           ///< its label, for what is said of it
    QPolygonF outline;      ///< µm; a circle as a polygon of many sides
    bool keep = true;       ///< kept, the rest masked (false: taken out)
    QStringList layers;     ///< the stack layers it cuts, by name (empty: every one)
    QString tag;            ///< its node's, for the tree to mark it read (last: the regions are written as aggregates)
    bool cuts(const QString& layer) const { return layers.isEmpty() || layers.contains(layer); }
};

/*!
 * The Cutout read: the regions that are enabled, in order. On each layer,
 * what the regions that keep and cut it hold is kept - all of it where none
 * keeps -, and then what those that take out and cut it hold is taken out.
 */
struct Cutout {
    QList<CutRegion> regions;
    /// Each problem of a region left out, with that region's tag: found by
    /// its tag, not by matching a translated text - "Cutout: R1" also began
    /// R10's (the review of 26ff2805).
    QList<std::pair<QString, QString>> problems;
    bool isEmpty() const { return regions.isEmpty(); }
};
/// \a model's Cutout read, its lengths in its parameters, in micrometres.
/// Empty where it has no region or its regions are not applied. A region
/// that cannot be read - a length that is no number, a polygon of fewer than
/// three corners, no area, a layer no layer of the stack - is said in
/// \a problems and left out.
Cutout cutoutOf(const Model& model, QStringList* problems);

/// A shape of the model's own (Shapes, rf_shape) as its parameters make it:
/// its outline, µm, on a layer of the stack.
struct OwnShape {
    QString tag, name, layer;
    QPolygonF outline;
};
/// The model's own shapes, each evaluated in its parameters - a path
/// outlined as gdstk outlines one, its joins and ends as a path drawn on the
/// tab -; one that cannot be read, or is on no layer a shape may be on, left
/// out and said in \a problems.
QList<OwnShape> ownShapesOf(const Model& model, QStringList* problems = nullptr);

/// A Dimension (rf_dimension) as its parameters make it: the corners of its
/// layer's shapes of the layout file inside \a box moved by \a move, µm.
struct Dimension {
    QString tag, name, layer;
    QRectF box;
    QPointF move;
    QString offset;   ///< its By as written (w - 50[um])
};
/// The model's Dimensions, in order, each evaluated in its parameters; one
/// that cannot be read left out and said in \a problems.
QList<Dimension> dimensionsOf(const Model& model, QStringList* problems = nullptr);
/// \a outline, a shape of \a layer, its corners moved by \a dimensions in
/// order: each corner inside one's box, its edges too, by its move; \a moved,
/// given (one per dimension), counts them. False, \a outline as it was and why
/// in \a why, where it would cross itself or have no area.
bool moveCorners(QPolygonF& outline, const QString& layer, const QList<Dimension>& dimensions, QString* why = nullptr,
                 QVector<int>* moved = nullptr);
/// \a shapes of \a layer cut as \a cutout says: a shape wholly outside what
/// it keeps dropped, one across a region's edge cut along it, one it leaves
/// whole returned as it was. \a cut and \a masked, given, count the shapes
/// cut and those dropped.
QList<QPolygonF> cutOf(const QList<QPolygonF>& shapes, const Cutout& cutout, const QString& layer, int* cut = nullptr,
                       int* masked = nullptr);

/// The rf_processing node read: its flags, and its lengths in metres - and
/// the Cutout beside it, which the processing applies first.
struct Processing {
    bool merge = true;
    bool mergeVias = true;
    bool keepArea = true;
    bool dropSlivers = true;
    bool snap = false;
    bool heal = true;
    bool removeFill = false;
    double viaDistance = 1e-6;
    double sliverWidth = 1e-7;
    double grid = 5e-9;
    /// Curves thinned (Douglas-Peucker): corners within \a curveTolerance
    /// of the line between their neighbours dropped - a circle of 180
    /// segments a turn as one of some 80. \a curveTolerance 0: a hundredth
    /// of each shape's width (twice its area over its perimeter), at most
    /// 0.1 µm. m.
    bool simplify = true;
    double curveTolerance = 0;
    /// The part of the layout to solve, in the polygons' own units
    /// (processRead() reads it with cutoutOf(); processingOf() leaves it
    /// empty).
    Cutout cutout;
    /// A plane of symmetry (its "Symmetry"): 0 none, 1 a plane x = \a
    /// symmetryAt, 2 y = it, m; the half beyond it (x or y above it) kept,
    /// the rest taken out after the cutout, and solved with its mirror
    /// (FullWaveControl::Symmetry) - a magnetic wall, or an electric one.
    int symmetryAxis = 0;
    double symmetryAt = 0;
    bool symmetryMagnetic = true;
};
/// \a node (an rf_processing) read in the model's parameters; what cannot
/// be read is said in \a problems and left at its default.
Processing processingOf(const Node& node, const QList<fem::Model::Parameter>& parameters, QStringList* problems);

/// The shapes after processing, with what each step did.
struct Processed {
    QList<Conductor> conductors;
    /// The shapes the map sends to a dielectric or the substrate, by layer:
    /// where that layer is, beyond which it is its "beyond" (µm).
    QMap<QString, QList<QPolygonF>> extents;
    LayoutReport report;
    QStringList problems;
    /// Those of problems that are the Cutout's: a region left out, which
    /// the solve warns of (C1 of the open items).
    QStringList cutoutProblems;
    bool ok() const { return problems.isEmpty(); }
    /// How many conductors each layer has.
    QMap<QString, int> byLayer() const;
};

/*!
 * \a byLayer (each layer of the stack to its polygons, in µm) processed as
 * \a how says, on the stack \a stack. \a fillLayers names the layers whose
 * shapes are dummy fill. \a micrometre is how many metres one unit of the
 * polygons is - 1e-6 for a layout read by layout.h.
 *
 * This is the whole of the processing and knows nothing of files, so a
 * test can hand it polygons it wrote itself.
 */
Processed processShapes(const QMap<QString, QList<QPolygonF>>& byLayer, const QSet<QString>& fillLayers,
                        const Processing& how, const Stack& stack, double micrometre = 1e-6);

/// The layer map of \a model read: each layer and datatype of the file to
/// the stack layer it belongs to ("31/0" to "M1"), and which of those are
/// dummy fill. A row that names no stack layer is left out.
struct LayerMap {
    QMap<QString, QString> toLayer;   ///< "31/0" to "M1"
    QSet<QString> fill;               ///< the stack layers whose shapes are fill
    QStringList problems;
    /// What it does that is worth saying but stands in no solve's way: a
    /// layer sent to two layers of the stack, the later row taking it.
    QStringList notes;
};
LayerMap layerMapOf(const Model& model);

/*!
 * \a read - a layout already in memory - processed as \a model says: the
 * cell the model names, its layer map applied, its processing steps run.
 * \a called is the file's name, for what the problems say about it.
 *
 * This is what a tab uses: its file is read on a thread of its own long
 * before anything is meshed, and reading a large GDSII again to process it
 * would hold the window for as long as the first read took.
 */
Processed processRead(const Model& model, const layout::Layout& read, const QString& called = {});

/*!
 * The model's layout read from its file and processed - processRead() with
 * the read before it. \a folder is where the model is, for a file named
 * relative to it. Null and why in \a error when the file cannot be read; a
 * model that names no file gives an empty result with a problem said, not
 * an error.
 */
Processed processLayout(const Model& model, const QString& folder, QString* error = nullptr);

} // namespace qucs_s::rf25

#endif
