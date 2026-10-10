/*
 * fem_geometry.h - a multiphysics model's geometry: its features built in
 *                  order into objects (rectangles, circles, polygons; moved,
 *                  rotated, united, cut), then made one by Form Union - the
 *                  domains its edges close, its boundaries and points,
 *                  numbered as COMSOL numbers them; and the selections of
 *                  them, by rule ("the boundaries of trace") or by number
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#ifndef QUCS_FEM_GEOMETRY_H
#define QUCS_FEM_GEOMETRY_H

#include "fem_model.h"

#include <QHash>
#include <QPointF>
#include <QRectF>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QVector>

#include <array>
#include <memory>
#include <vector>

namespace qucs_s::fem {

/// A curve of the geometry: a straight segment from c to c + a (t from 0
/// to 1), an arc of an ellipse - a circle's too - c + a cos t + b sin t,
/// t from t0 to t1, or a polyline through pts (a spline's or a Bézier's
/// points, as imported): t from 0 at the first to pts.size() - 1 at the
/// last, straight between. Affine maps keep it what it is.
struct Curve {
    enum Kind { Line, Arc, Polyline };
    Kind kind = Line;
    QPointF c, a, b;
    double t0 = 0, t1 = 1;
    std::vector<QPointF> pts;

    QPointF at(double t) const;
    /// The parameter of the curve's point nearest \a p.
    double paramOf(QPointF p) const;
    /// Its radius of curvature at \a t (a line: infinite).
    double radiusAt(double t) const;
    /// Its length from t0 to t1.
    double length() const;
};

/// A straight piece of a curve, from a to b: curve's parameters ta to tb.
struct GEdge {
    QPointF a, b;
    int curve = -1;
    double ta = 0, tb = 0;
};

/// An object made by the features: closed loops (counter-clockwise round
/// what it holds, clockwise round its holes), open curves, points.
struct GObject {
    QString name;
    std::vector<std::vector<GEdge>> loops;
    std::vector<GEdge> open;
    std::vector<QPointF> points;
    bool solid() const { return !loops.empty(); }
    QRectF bounds() const;
    /// Inside it: its loops wind round \a p.
    bool contains(QPointF p) const;
};

/*!
 * A geometry made one: the vertices of its planar graph, its segments
 * (each a piece of a curve, between two vertices), the domains they close,
 * the boundaries (chains of segments between points, on one curve) and the
 * points. Coordinates in the geometry's unit. Numbers are indices here;
 * COMSOL's numbers (shown, and in the file) are one more.
 */
struct Topology {
    std::vector<QPointF> vertices;
    struct Segment {
        int v0 = -1, v1 = -1;
        int curve = -1;
        double t0 = 0, t1 = 0;
        int boundary = -1;
        int left = -1, right = -1;   ///< domains on either side of v0 -> v1, or -1
        QSet<int> objects;           ///< the objects it is an edge of
    };
    std::vector<Segment> segments;
    struct Domain {
        QSet<int> objects;            ///< the objects that hold it
        int owner = -1;               ///< the smallest of them ("only", "between")
        QRectF bounds;
        double area = 0;
        QPointF inside;               ///< a point in it
        std::vector<std::array<int, 3>> triangles;   ///< of vertices: for drawing and picking
    };
    std::vector<Domain> domains;
    struct Boundary {
        std::vector<int> segments;    ///< in order, each from the last one's end
        std::vector<bool> reversed;   ///< a segment walked from v1 to v0
        int start = -1, end = -1;     ///< vertices at its two ends (equal: closed)
        int up = -1, down = -1;       ///< domains on its left and right (as walked)
        int curve = -1;               ///< the curve it lies on, when it is one
        double t0 = 0, t1 = 0;        ///< that curve's parameters at its ends
        QSet<int> objects;
        double length = 0;
        bool exterior() const { return (up < 0) != (down < 0); }
        /// Its points in order (its segments' ends).
        std::vector<QPointF> polyline(const Topology& t) const;
    };
    std::vector<Boundary> boundaries;
    struct Point {
        int vertex = -1;
        QSet<int> objects;
        std::vector<int> boundaries;
    };
    std::vector<Point> points;
    std::vector<Curve> curves;
    QStringList objectNames;          ///< the objects, in the order made (later overrides)
    std::vector<GObject> objects;
    QRectF bounds;
    double unitScale = 1e-3;          ///< metres in a unit of the geometry
    QString unitName;

    /// The domain at \a p, or -1.
    int domainAt(QPointF p) const;
    /// The boundary nearest \a p within \a tolerance, or -1.
    int boundaryAt(QPointF p, double tolerance) const;
    int pointAt(QPointF p, double tolerance) const;
    /// A boundary's outward normal from domain \a domain, its middle's.
    QPointF normalAt(int boundary, int domain) const;
};

/// A geometry built: its objects, made one, and what each feature said.
struct GeometryBuild {
    std::shared_ptr<Topology> topology;
    /// Errors by feature tag: the build stopped at the first.
    QHash<QString, QString> errors;
    QHash<QString, QString> warnings;
    /// The feature the build stopped at, or empty.
    QString failedAt;
    /// The drawings its Import features read (absolute paths).
    QStringList files;
    bool ok() const { return failedAt.isEmpty() && topology != nullptr; }
};

/// Builds \a model's geometry (its parameters evaluated in \a parameters).
/// Up to the feature \a upTo (inclusive; empty: all): the objects then,
/// made one.
GeometryBuild buildGeometry(const Model& model, const ParameterScope& parameters, const QString& upTo = {});

/*!
 * A selection resolved: the domains, boundaries or points of \a level that
 * \a rule picks in \a topology. A rule:
 *   {"all": true}                every one
 *   {"numbers": [1, 3]}          those (COMSOL's numbers, from 1)
 *   {"objects": ["air"]}         domains in any of these objects
 *   {"only": ["air"]}            domains whose smallest object is one of these
 *   {"of": ["trace"]}            boundaries (points) on the objects' edges;
 *                                with "side": "bottom" (top, left, right)
 *                                those facing it
 *   {"between": ["a", "b"]}      boundaries between a's domains and b's
 *   {"exterior": true}           boundaries with a domain on one side only
 *   {"interior": true}           boundaries between two domains
 *   {"adjacent": {...}}          boundaries (points) of the domains a rule picks
 *   {"box": [x0, y0, x1, y1]}    those wholly inside the box (the geometry's unit)
 *   {"named": "sel1"}            a named selection (Definitions)
 * and "except": {...} - those another rule picks taken out. Indices from 0,
 * sorted; an error said in \a error (an object or a selection unknown).
 */
QVector<int> resolveSelection(const Topology& topology, const Model& model, Level level, const QJsonObject& rule,
                              QString* error = nullptr);
/// \a rule said for a reader: "domains 1, 3", "all boundaries", "the
/// boundaries of trace (bottom)".
QString describeSelection(const QJsonObject& rule, Level level);

} // namespace qucs_s::fem

#endif // QUCS_FEM_GEOMETRY_H
