/*
 * router_gds.h - routes committed to a layout file as a cell of their
 *                own, placed once in the cell routed: a later commit
 *                replaces that cell's contents and keeps its placement,
 *                so a run can be redone cleanly. Read and written with
 *                gdstk (GDSII, or OASIS by the target's suffix)
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#ifndef QUCS_ROUTER_GDS_H
#define QUCS_ROUTER_GDS_H

#include "router.h"

namespace gdstk {
struct Library;
}

namespace qucs_s::router {

/// A layout file gdstk writes: .gds, .gds2, .gdsii, or (isOasisFile) .oas,
/// .oasis.
bool isWritableLayout(const QString& path);
bool isOasisFile(const QString& path);
/// \a path read whole with gdstk - GDSII, or OASIS by its suffix - into
/// \a lib (its units set when the file has none); false, and why, when it
/// cannot be (\a lib then freed).
bool readLibrary(const QString& path, gdstk::Library* lib, QString* error);
/// \a lib written whole to \a target - GDSII, or OASIS by its suffix -: a
/// temporary file beside it, renamed over it when complete. \a lib is
/// left as it was (the caller frees it).
bool writeLibrary(gdstk::Library& lib, const QString& target, QString* error);

struct Commit {
    QString source;             ///< the layout read
    QString target;             ///< the file written (may be the source)
    QString topCell;            ///< the cell the route cell is placed in (empty: the largest top cell)
    QString cell = QStringLiteral("ROUTES");
    QStringList nets;           ///< the nets written (empty: every routed one)
    bool remove = false;        ///< take the route cell and its placements away instead
};

struct CommitReport {
    bool ok = false;
    QString error;
    int polygons = 0;           ///< written in the route cell
    int nets = 0;
    bool replaced = false;      ///< the cell was there already
    bool placed = false;        ///< a placement of it was added to the top cell
};

/// Writes \a result's routes as \a commit says. The target is written
/// whole: a temporary file beside it, renamed over it when complete.
CommitReport commitRoutes(const Commit& commit, const Problem& problem, const Result& result);

/// The polygons a net's route draws, by layer (for the tests and the
/// preview): runs and landings on its metals, cuts on the via layer; µm.
struct DrawnShape {
    LayerId layer;
    QVector<QPointF> points;
};
QVector<DrawnShape> drawnShapes(const Problem& problem, const NetResult& route, int net);

} // namespace qucs_s::router

#endif // QUCS_ROUTER_GDS_H
