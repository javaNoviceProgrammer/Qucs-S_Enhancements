/*
 * fem3_geometry.h - a 3D multiphysics model's geometry: layered solids, each
 *                   a region of the plan from a bottom z up through a height
 *                   (boxes, cylinders, extruded sketches, a layer stack's
 *                   rows, a layout's layers), united, cut and intersected
 *                   slab by slab, then made one by Form Union - the domains
 *                   (volumes), faces, edges and points, numbered from below,
 *                   then from the left, then from the front; and the
 *                   selections of them
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#ifndef QUCS_FEM3_GEOMETRY_H
#define QUCS_FEM3_GEOMETRY_H

#include "fem_geometry.h"

#include <array>
#include <cmath>
#include <limits>
#include <memory>
#include <utility>
#include <vector>

namespace qucs_s::fem {

/// A point or a direction in 3D, in the geometry's unit.
struct Vec3 {
    double x = 0, y = 0, z = 0;
    Vec3 operator+(const Vec3& o) const { return {x + o.x, y + o.y, z + o.z}; }
    Vec3 operator-(const Vec3& o) const { return {x - o.x, y - o.y, z - o.z}; }
    Vec3 operator-() const { return {-x, -y, -z}; }
    Vec3 operator*(double k) const { return {x * k, y * k, z * k}; }
    Vec3 operator/(double k) const { return {x / k, y / k, z / k}; }
    double dot(const Vec3& o) const { return x * o.x + y * o.y + z * o.z; }
    Vec3 cross(const Vec3& o) const { return {y * o.z - z * o.y, z * o.x - x * o.z, x * o.y - y * o.x}; }
    double norm() const { return std::sqrt(x * x + y * y + z * z); }
    Vec3 normalized() const
    {
        const double n = norm();
        return n > 0 ? *this / n : Vec3{};
    }
};

/// A box along the axes, empty until something is added to it.
struct Box3 {
    double x0 = std::numeric_limits<double>::infinity(), y0 = x0, z0 = x0;
    double x1 = -std::numeric_limits<double>::infinity(), y1 = x1, z1 = x1;
    bool isEmpty() const { return !(x0 <= x1 && y0 <= y1 && z0 <= z1); }
    void add(const Vec3& p);
    void add(const Box3& b);
    /// \a b inside it, \a tolerance given.
    bool contains(const Box3& b, double tolerance = 0) const;
    Vec3 center() const { return {(x0 + x1) / 2, (y0 + y1) / 2, (z0 + z1) / 2}; }
    double diagonal() const { return isEmpty() ? 0 : std::sqrt((x1 - x0) * (x1 - x0) + (y1 - y0) * (y1 - y0) + (z1 - z0) * (z1 - z0)); }
};

/*!
 * A 3D geometry made one. Every solid is a region of the plan times a range
 * of z, so the whole is a plan - a 2D Form Union of every solid's outline -
 * and levels: each plan domain in each slab between two levels is a cell,
 * held by the solids whose region and range hold it. A domain is the cells
 * held by the same solids that touch (as a 2D domain is the part of the
 * plane held by the same objects); a face is where two domains meet, or a
 * domain the outside, on one surface (a plane, a cylinder); an edge is where
 * faces meet, a point where edges meet.
 *
 * Its parts, each a domain's, face's, edge's or point's, or none's (inside
 * one, or outside every solid):
 *   cell (d, k)          plan domain d in slab k: between levels k and k + 1
 *   wall (b, k)          plan boundary b through slab k
 *   floor (d, i)         plan domain d at level i
 *   rise (p, k)          plan point p through slab k: an edge up a wall
 *   rim (b, i)           plan boundary b at level i: an edge along a floor
 *   corner (p, i)        plan point p at level i
 * Numbers are indices here; those shown, and in a file, are one more.
 */
struct Topology3 {
    /// Every solid's outline made one; its objects the solids' regions.
    std::shared_ptr<const Topology> plan;
    /// Where solids begin and end, rising: slab k is from levels[k] to
    /// levels[k + 1].
    std::vector<double> levels;
    int slabs() const { return levels.size() > 1 ? int(levels.size()) - 1 : 0; }

    QStringList objectNames;   ///< the solids, in the order made (later booleans' inputs used up)
    QStringList objectGroups;  ///< each one's feature, by its name: an extrude's, a stack's (else the solid's own)
    std::vector<double> objectVolume;
    std::vector<char> objectLayers;   ///< each one a Layer Stack's row

    std::vector<int> cellDomain;   ///< [d * slabs() + k]: the domain, or -1
    std::vector<int> wallFace;     ///< [b * slabs() + k]: the face, or -1
    std::vector<int> floorFace;    ///< [d * levels.size() + i]
    std::vector<int> riseEdge;     ///< [p * slabs() + k]: the edge, or -1
    std::vector<int> rimEdge;      ///< [b * levels.size() + i]
    std::vector<int> cornerPoint;  ///< [p * levels.size() + i]: the point, or -1

    struct Domain {
        QSet<int> objects;           ///< the solids that hold it
        int owner = -1;              ///< the smallest of them ("only", "between"); of two alike the later
        Box3 bounds;
        double volume = 0;
        Vec3 inside;                 ///< a point in it
        std::vector<std::pair<int, int>> cells;   ///< (plan domain, slab)
    };
    struct Face {
        /// The domains behind it and in front of it (its normal points from
        /// back into front), or -1: outside.
        int back = -1, front = -1;
        bool floor = false;          ///< level (its normal +z), else a wall (its normal across z)
        /// A wall's (plan boundary, slab), a floor's (plan domain, level).
        std::vector<std::pair<int, int>> atoms;
        int curve = -1;              ///< a wall's plan curve (-1: a floor, or walls on several)
        bool planar = false;         ///< a floor, or a wall on a straight line
        Vec3 normal;                 ///< a planar one's, from back into front
        QSet<int> objects;           ///< the solids whose surface it is on
        Box3 bounds;
        double area = 0;
        Vec3 middle;                 ///< a point on it
        int middleAtom = 0;          ///< the atom it is on
        bool exterior() const { return (back < 0) != (front < 0); }
    };
    struct Edge {
        bool rise = false;           ///< up the plan point of its atoms, else along plan boundaries at a level
        /// In order from its start: rises (plan point, slab) or rims (plan
        /// boundary, level).
        std::vector<std::pair<int, int>> atoms;
        int start = -1, end = -1;    ///< its points (one: closed)
        std::vector<int> faces;      ///< the faces that meet on it
        QSet<int> objects;           ///< its faces' solids
        Box3 bounds;
        double length = 0;
        Vec3 middle;
    };
    struct Point {
        int planPoint = -1;
        int level = -1;
        Vec3 position;
        std::vector<int> edges;
        QSet<int> objects;
    };
    std::vector<Domain> domains;
    std::vector<Face> faces;
    std::vector<Edge> edges;
    std::vector<Point> points;
    Box3 bounds;
    double unitScale = 1e-3;     ///< metres in a unit of the geometry
    QString unitName;

    /// The domain of cell (\a planDomain, \a slab), or -1.
    int cell(int planDomain, int slab) const;
    /// The slab \a z is in (its lower level's), or -1 outside.
    int slabAt(double z) const;
    /// The domain at \a p, or -1.
    int domainAt(const Vec3& p) const;
    /// A face's triangles, for drawing and picking: each counter-clockwise
    /// seen from its front (its normal's side).
    std::vector<std::array<Vec3, 3>> faceTriangles(int face) const;
    /// An edge's points, in order from its start.
    std::vector<Vec3> edgePolyline(int edge) const;
    /// A face's normal out of \a domain (one of its two sides), at its
    /// middle - a curved wall's there.
    Vec3 normalAt(int face, int domain) const;
};

/// A 3D geometry built: its solids made one, and what each feature said.
struct GeometryBuild3 {
    std::shared_ptr<Topology3> topology;
    /// Errors by feature tag: the build stopped at the first.
    QHash<QString, QString> errors;
    QHash<QString, QString> warnings;
    /// The feature the build stopped at, or empty.
    QString failedAt;
    /// The drawings and layouts read (absolute paths).
    QStringList files;
    bool ok() const { return failedAt.isEmpty() && topology != nullptr; }
};

/// Builds 3D \a model's geometry (its parameters evaluated in \a parameters),
/// up to the feature \a upTo (inclusive; empty: all).
GeometryBuild3 buildGeometry3(const Model& model, const ParameterScope& parameters, const QString& upTo = {});

/// How many of \a level a 3D topology has.
int count3(const Topology3& topology, Level level);

/*!
 * A selection of a 3D geometry resolved: the domains, faces (Level::Boundary),
 * edges or points that \a rule picks. The 2D rules (resolveSelection) as they
 * are - "of" with "side" left or right (along x), front or back (y), bottom
 * or top (z); "box" of six numbers [x0, y0, z0, x1, y1, z1] - and:
 *   {"layer": ["M1"]}            a stack row's solids (their faces, edges...)
 *   {"normal": [0, 0, 1]}        planar faces facing that way (an exterior
 *                                face out of its domain, another either way)
 *   {"plane": {"z": 3}}          faces lying in the plane z = 3 (x, y alike)
 * Indices from 0, sorted; an error said in \a error.
 */
QVector<int> resolveSelection3(const Topology3& topology, const Model& model, Level level, const QJsonObject& rule,
                               QString* error = nullptr);
/// \a rule said for a reader, a 3D model's way: "faces 1, 3", "the faces of
/// trace (top)".
QString describeSelection3(const QJsonObject& rule, Level level);

} // namespace qucs_s::fem

#endif // QUCS_FEM3_GEOMETRY_H
