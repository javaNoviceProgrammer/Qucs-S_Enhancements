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
 * The steps are the table of docs/proposals/2026-10-23-rf-2p5d-moment-method.md:
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

namespace qucs_s::rf25 {

/// One polygon that will become a conductor: the layer of the stack it
/// belongs to, and its outline in the layout's micrometres.
struct Conductor {
    QString layer;          ///< a stack layer's name: "M1", "V1"
    QPolygonF outline;
    bool fill = false;      ///< dummy fill, kept as floating metal
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
    QString summary() const;   ///< the lines joined, for a status bar or a tooltip
};

/// The rf_processing node read: its flags, and its lengths in metres.
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
};
/// \a node (an rf_processing) read in the model's parameters; what cannot
/// be read is said in \a problems and left at its default.
Processing processingOf(const Node& node, const QList<fem::Model::Parameter>& parameters, QStringList* problems);

/// The shapes after processing, with what each step did.
struct Processed {
    QList<Conductor> conductors;
    LayoutReport report;
    QStringList problems;
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
};
LayerMap layerMapOf(const Model& model);

/*!
 * The model's layout read from its file and processed. \a folder is where
 * the model is, for a file named relative to it. Null and why in \a error
 * when the file cannot be read; a model that names no file gives an empty
 * result with a problem said, not an error.
 */
Processed processLayout(const Model& model, const QString& folder, QString* error = nullptr);

} // namespace qucs_s::rf25

#endif
