/*
 * layoutnet.h - a net of a layout traced from one of its shapes: the
 *               metal joined to it, on its layer and through the cuts
 *               between layers
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#ifndef QUCS_LAYOUTNET_H
#define QUCS_LAYOUTNET_H

#include "layout.h"

#include <QMap>
#include <QPolygonF>
#include <QRectF>
#include <QString>
#include <QVector>

#include <atomic>

namespace qucs_s::layout {

/// A metal joined to the metal above it by a cut layer: Layout::layers'
/// indices (-1: a layer the layout has nothing on).
struct NetStep {
    int lower = -1;
    int cut = -1;
    int upper = -1;
};

/// How a layout's layers join: steps that chain (M1-V1-M2, M2-V2-M3), and
/// where they came from, to say.
struct NetConnectivity {
    QVector<NetStep> steps;
    QString source;
    /// Every layer a step names.
    QVector<int> layers() const;
    bool isEmpty() const { return steps.isEmpty(); }
};

/// The router's technology beside the layout (<layout>.route.json, its
/// technology there or the file it names): its metals and vias, a step
/// for each pair. False, with why, when there is none or it cannot be
/// read.
bool connectivityBeside(const Layout& layout, NetConnectivity* out, QString* why);

/// A shape of a net: where it is, its outline in the traced cell's µm.
struct NetShape {
    Found found;
    QPolygonF outline;
    int layer = -1;
    bool cut = false;           ///< on a step's cut layer
};

/// A text on a net: on a layer traced, its origin on one of the net's
/// shapes there.
struct NetLabel {
    QString text;
    int layer = -1;
    QPointF at;
    int cell = -1;              ///< where it is defined
};

/// A via of a net: its cuts that join the same two shapes (an array's
/// cuts, one via).
struct NetVia {
    int layer = -1;             ///< the cut layer
    QPointF at;                 ///< the middle of its cuts
    int cuts = 0;
};

struct TracedNet {
    QVector<NetShape> shapes;
    QVector<NetLabel> labels;
    QVector<NetVia> vias;
    QRectF bounds;
    /// The centre-line length of its runs, µm: each path's spine, each
    /// rectangle's long side less its short (a run drawn half its width
    /// past each end); other polygons and the cuts not counted.
    double length = 0;
    /// Its pieces on each layer it is on (Layout::layers' index): its shapes
    /// there joined where they touch, as a merge would leave them.
    QMap<int, int> pieces;
    bool stopped = false;       ///< maxShapes reached: more of it not followed
    bool cancelled = false;
    /// The cells its shapes are defined in, by Layout::cells' index.
    QVector<int> cells() const;
};

struct NetTrace {
    int cell = -1;              ///< the cell traced in, through what it places
    QVector<Found> seeds;       ///< where it starts (in that cell's coordinates)
    NetConnectivity connectivity;
    QVector<bool> stopAt;       ///< by layer index: not followed into
    int maxShapes = 20000;
    const std::atomic_bool* cancel = nullptr;
};

/// The net the seeds are on: everything that touches or overlaps them on
/// their layer, and across layers through the steps' cuts, by the shapes'
/// own geometry in the traced cell's coordinates - a net in a placed cell
/// is the shapes it touches there, never a net of another cell that
/// happens to share its name or number. A seed's layer that no step names
/// is followed on that layer alone.
TracedNet traceNet(const Layout& layout, const NetTrace& trace);

/// The shapes a trace starts from at \a point: on \a layer, or (-1) on
/// the lowest layer of \a connectivity that has one there - as the router
/// reads a terminal.
QVector<Found> seedsAt(const Layout& layout, int cell, QPointF point, int layer, const NetConnectivity& connectivity);

} // namespace qucs_s::layout

#endif // QUCS_LAYOUTNET_H
