/*
 * fem_fe.h - the finite elements of the multiphysics solver: Lagrange
 *            triangles of order 1 and 2 (a quadratic one curved where its
 *            edge lies on an arc), Dunavant's rules of integration on them
 *            and Gauss's on their edges, the space of a mesh's degrees of
 *            freedom
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#ifndef QUCS_FEM_FE_H
#define QUCS_FEM_FE_H

#include "fem_mesh.h"

#include <array>
#include <memory>
#include <vector>

namespace qucs_s::fem {

/// A rule of integration on the reference triangle (0,0), (1,0), (0,1):
/// points (xi, eta) and weights (they add up to its area, 1/2).
struct TriangleRule {
    std::vector<std::array<double, 3>> points;   // xi, eta, weight
};
/// Dunavant's rule exact for polynomials of degree \a degree (1 to 5).
const TriangleRule& triangleRule(int degree);
/// Gauss's rule of \a n points on [0, 1]: (s, weight).
const std::vector<std::array<double, 2>>& lineRule(int n);

/// The shape functions of a Lagrange triangle of order 1 (3) or 2 (6:
/// the corners, then the middles of edges 0-1, 1-2, 2-0) at a point of the
/// reference triangle, and their derivatives there.
struct Shape {
    int count = 3;
    double n[6];
    double dxi[6], deta[6];
};
void shapeAt(int order, double xi, double eta, Shape& s);

/*!
 * The degrees of freedom of a mesh for Lagrange elements of an order: one
 * at each node, and (order 2) one at each edge's middle. Coordinates in
 * metres (SI): the mesh's are in the geometry's unit. An element's map
 * from the reference triangle is affine, or (order 2, an edge on an arc)
 * quadratic: its middle node on the arc.
 */
class Space
{
public:
    Space(std::shared_ptr<const Mesh> mesh, int order);

    const Mesh& mesh() const { return *a_mesh; }
    std::shared_ptr<const Mesh> meshPtr() const { return a_mesh; }
    int order() const { return a_order; }
    int size() const { return a_size; }
    int perElement() const { return a_order == 1 ? 3 : 6; }
    int elements() const { return int(a_mesh->triangles.size()); }
    const int* dofs(int t) const { return &a_dofs[std::size_t(t) * std::size_t(perElement())]; }
    /// Where a degree of freedom is (m).
    QPointF point(int dof) const { return a_points[std::size_t(dof)]; }
    bool curved(int t) const { return a_curved[std::size_t(t)] != 0; }
    /// The element's geometry nodes (m): its corners, and its edges'
    /// middles (order 2).
    const std::array<QPointF, 6>& nodes(int t) const { return a_nodes[std::size_t(t)]; }

    /// The map of element \a t at reference point (xi, eta): the point
    /// (m), the Jacobian's determinant, its inverse transposed (the
    /// gradient of a shape function is invT times its reference one).
    struct Map {
        QPointF x;
        double det = 0;
        double invT[2][2];
    };
    Map map(int t, double xi, double eta) const;
    /// A shape function's gradient (m^-1) from its reference one.
    static void gradient(const Map& m, double dxi, double deta, double& gx, double& gy)
    {
        gx = m.invT[0][0] * dxi + m.invT[0][1] * deta;
        gy = m.invT[1][0] * dxi + m.invT[1][1] * deta;
    }

    /// The degrees of freedom on mesh edge \a e (of the mesh's boundary
    /// edges): at a, at b, (order 2) its middle.
    std::array<int, 3> edgeDofs(int e) const;
    /// The point at s (0 at a, 1 at b) on mesh edge \a e, and the length
    /// of the edge there per unit of s (m).
    QPointF edgePoint(int e, double s, double* jacobian = nullptr) const;
    /// The local index (0, 1, 2) of a triangle's edge a -> b, or -1.
    static int localEdge(const std::array<int, 3>& tri, int a, int b);

private:
    std::shared_ptr<const Mesh> a_mesh;
    int a_order = 1;
    int a_size = 0;
    std::vector<int> a_dofs;
    std::vector<QPointF> a_points;
    std::vector<std::array<QPointF, 6>> a_nodes;
    std::vector<char> a_curved;
    struct Affine {
        double det;
        double invT[2][2];
    };
    std::vector<Affine> a_affine;
    std::vector<std::array<int, 3>> a_edgeDofs;   // by mesh boundary edge
    std::vector<QPointF> a_edgeMid;               // (m)
};

} // namespace qucs_s::fem

#endif // QUCS_FEM_FE_H
