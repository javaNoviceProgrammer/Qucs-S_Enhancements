/***************************************************************************
                                rf25_export.h
                               ---------------
 *  An RF 2.5D model's shapes and stack written out, to be looked at or
 *  worked on elsewhere: a GDSII file, a DXF drawing, and a 3D model of
 *  Multiphysics 3D (.qfem3d) - its Layer Stack drawing the shapes up from a
 *  GDSII file beside it. None needs a solve.
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 ***************************************************************************/

#ifndef QUCS_RF25_EXPORT_H
#define QUCS_RF25_EXPORT_H

#include "rf25_layout.h"
#include "rf25_model.h"

#include <QByteArray>
#include <QColor>
#include <QList>
#include <QMap>
#include <QPolygonF>
#include <QRectF>
#include <QString>
#include <QStringList>

#include <cmath>
#include <limits>
#include <utility>

namespace qucs_s::rf25 {

/// One shape written out: its layer of the stack, the layer and datatype it
/// is written on, its outline (µm; a hole kept as GDSII keeps it, by a
/// keyhole).
struct ExportedShape {
    QString layer;
    int number = 0, datatype = 0;
    QPolygonF outline;
};

/// The shapes as processed, each with the numbers it is written on.
struct LayoutExport {
    QList<ExportedShape> shapes;
    /// Each layer of the stack that has shapes, to its layer and datatype.
    QMap<QString, std::pair<int, int>> numbers;
    /// Those layers in the stack's order, bottom up (any the stack does not
    /// have after them).
    QStringList layers;
    QStringList notes;      ///< what was done that is worth saying
    QStringList problems;   ///< why nothing can be written
    bool ok() const { return problems.isEmpty(); }
    /// Round every shape, µm (null: none).
    QRectF bounds() const;
};

/// The shapes of \a processed - the layout's and the model's own, cut by
/// the Cutout; on the metals, the vias and the dielectrics alike - each on
/// the layer and datatype the layer map of \a model gives its layer of the
/// stack: the lowest of the file's layers it sends there. A layer it gives
/// none (the model's own shapes alone) takes the next number free, datatype
/// 0, and is said.
LayoutExport layoutExportOf(const Model& model, const Processed& processed);

/// \a shapes as a GDSII file of one cell, \a cell (TOP when empty), in µm.
bool writeLayoutGds(const LayoutExport& shapes, const QString& path, const QString& cell, QString* error);

/// \a shapes as an ASCII DXF drawing, AutoCAD R12 (AC1009) - which every
/// reader takes -, in µm ($INSUNITS 13): each a closed POLYLINE on a layer of
/// the DXF named after its layer of the stack (letters, digits, '_', '-' and
/// '$'; others '_'), coloured by the nearest of AutoCAD's colours to the one
/// \a colours gives it (by name; white when none). \a madeBy in its comment.
QByteArray dxfOf(const LayoutExport& shapes, const QMap<QString, QColor>& colours = {}, const QString& madeBy = {});

/// The DXF layer \a name is written on (dxfOf()).
QString dxfLayerName(const QString& name);

/// A 3D model of Multiphysics 3D made of an RF 2.5D model.
struct Mp3dExport {
    QByteArray json;        ///< the .qfem3d
    QStringList notes;      ///< what it holds, and what it leaves out
    QStringList problems;   ///< why nothing can be written
    bool ok() const { return problems.isEmpty(); }
};

/*!
 * \a model as a 3D model (.qfem3d), its shapes \a shapes read from
 * \a gdsFile (relative to the model's folder: the GDSII file of the same
 * name, which writeLayoutGds() writes beside it).
 *
 * One Layer Stack across a window round the shapes - \a margin (m) beyond
 * them on every side (NaN: a tenth of their larger side), a lid's side walls
 * where it has them - with a row of each slab of the stack: the substrate
 * and the dielectrics; the room each metal takes, filled with the dielectric
 * over it as the 2.5D stack has it; each metal and via drawn up from its
 * shapes (a via through the room between its two metals); a ground plane
 * under it all, as thick as the thinnest metal; the air above. Each with a
 * material of its own: εr (a uniaxial layer's along z too), tan δ, σ, µr. A
 * perfect conductor is written with σ = 1e12 S/m.
 *
 * What is hidden in the RF 2.5D tab (layerVisibility()) is hidden there,
 * as the material's visibility; the air is drawn as its frame alone, unless
 * hidden.
 */
Mp3dExport mp3dOf(const Model& model, const LayoutExport& shapes, const QString& gdsFile, const QString& title,
                  double margin = std::numeric_limits<double>::quiet_NaN(), const QString& madeBy = {});

} // namespace qucs_s::rf25

#endif
