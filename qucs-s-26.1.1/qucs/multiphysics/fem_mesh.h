/*
 * fem_mesh.h - a multiphysics model's mesh: triangles, Delaunay, of the
 *              sizes the model asks - a size for all, for some domains,
 *              boundaries or points, finer on curves and across narrow
 *              gaps, growing no faster than a rate - their angles made good
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#ifndef QUCS_FEM_MESH_H
#define QUCS_FEM_MESH_H

#include "fem_geometry.h"

#include <array>
#include <functional>
#include <memory>
#include <vector>

namespace qucs_s::fem {

/*!
 * A mesh of triangles over a geometry's domains: its nodes (in the
 * geometry's unit), triangles (counter-clockwise), each one's domain; the
 * edges on the geometry's boundaries, each with its boundary and the curve
 * it lies on (a quadratic element's middle node goes on that curve); the
 * node at each point of the geometry.
 */
struct Mesh {
    std::vector<QPointF> nodes;
    std::vector<std::array<int, 3>> triangles;
    std::vector<int> domain;
    struct Edge {
        int a = -1, b = -1;      ///< nodes, in the boundary's direction
        int boundary = -1;
        int curve = -1;          ///< a curve of the topology (an arc's middle on it), or -1
        double ta = 0, tb = 0;   ///< its parameters at a and b
        int left = -1;           ///< the triangle on its left (the boundary's up side), or -1
        int right = -1;          ///< and on its right
    };
    std::vector<Edge> edges;
    std::vector<int> pointNode;  ///< each point of the geometry's node
    std::shared_ptr<const Topology> topology;
    double unitScale = 1e-3;

    // Its quality: the smallest angle (degrees); a triangle's quality, 1 for
    // an equilateral one, 0 a flat one - the worst, the mean, how many of
    // each tenth.
    double minAngle = 0;
    double minQuality = 0;
    double meanQuality = 0;
    std::array<int, 10> histogram{};
    /// Triangles whose smallest angle comes from the geometry (a sharp
    /// corner), not made better.
    int sharpCorners = 0;

    QRectF bounds() const;
    /// A triangle's quality: 4√3 A / (sum of its edges squared).
    double quality(int triangle) const;
    void computeStatistics();
};

/// The sizes a mesh is built to, in the geometry's unit.
struct MeshSizes {
    double hmax = 0, hmin = 0;
    double growth = 1.3;
    double curvature = 0.3;
    double narrow = 1;
    std::vector<double> domainMax;     // by domain (infinite: none)
    std::vector<double> boundaryMax;   // by boundary
    std::vector<double> pointMax;      // by point
    std::vector<int> boundaryCount;    // elements on a boundary (0: by size)
};

/// The sizes \a model's mesh node asks for on \a topology: its preset,
/// customized, and its Size and Distribution features. Errors in \a error.
MeshSizes meshSizes(const Model& model, const ParameterScope& parameters, const Topology& topology, QString* error);

/// A mesh built, or why not.
struct MeshBuild {
    std::shared_ptr<Mesh> mesh;
    QString error;
    QStringList warnings;
};

/// Called as the mesh is built: how far (0 to 1); false cancels.
using Progress = std::function<bool(double fraction, const QString& what)>;

/// Meshes \a topology to \a sizes.
MeshBuild buildMesh(std::shared_ptr<const Topology> topology, const MeshSizes& sizes, const Progress& progress = {});
/// Meshes \a model's geometry as its mesh node says.
MeshBuild buildMesh(const Model& model, const ParameterScope& parameters, std::shared_ptr<const Topology> topology,
                    const Progress& progress = {});

} // namespace qucs_s::fem

#endif // QUCS_FEM_MESH_H
