/*
 * fem3_fe.h - the 3D finite elements of the multiphysics solver: Lagrange
 *             prisms of order 1 (6 nodes) and 2 (18: the quadratic
 *             triangle's six at the bottom, the middle and the top) - a swept
 *             mesh's, each its plan triangle times its height, curved where
 *             its triangle is -, their rules of integration (a triangle's
 *             times Gauss's along z), and the space of a swept mesh's degrees
 *             of freedom with its boundary faces' points
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#ifndef QUCS_FEM3_FE_H
#define QUCS_FEM3_FE_H

#include "fem3_mesh.h"
#include "fem_fe.h"

#include <array>
#include <memory>
#include <unordered_map>
#include <vector>

namespace qucs_s::fem {

/// The shape functions of a Lagrange prism of order 1 (6: the triangle's
/// corners at the bottom, then at the top) or 2 (18: the quadratic
/// triangle's six - its corners, then the middles of edges 0-1, 1-2, 2-0 -
/// at ζ = 0, then at ζ = 1/2, then at ζ = 1), and their derivatives, at a
/// point of the reference prism: (ξ, η) in the triangle (0,0), (1,0), (0,1),
/// ζ from 0 to 1.
struct PrismShape {
    int count = 6;
    double n[18], dxi[18], deta[18], dzeta[18];
};
void prismShapeAt(int order, double xi, double eta, double zeta, PrismShape& s);

/// A rule of integration on the reference prism: (ξ, η, ζ, weight), the
/// weights adding up to its volume, 1/2.
struct PrismRule {
    std::vector<std::array<double, 4>> points;
};
/// Dunavant's rule of degree \a degree (1 to 5) on the triangle times
/// Gauss's of \a n points along ζ.
const PrismRule& prismRule(int degree, int n);

/*!
 * The degrees of freedom of a swept mesh (Mesh3) for Lagrange prisms of an
 * order: its plan's 2D space of that order (fem_fe.h - one at each plan node
 * and, order 2, at each edge's middle, on an arc where the edge is) at each
 * height, and (order 2) at each layer's middle - those its prisms touch.
 * Order 1's are the mesh's nodes, numbered as they are; order 2's the nodes
 * first, then the rest. Coordinates in metres (SI): the mesh's are in the
 * geometry's unit. A prism's map is its triangle's (affine, or quadratic on
 * an arc) times the line between its heights, so its Jacobian is the
 * triangle's and dz/dζ.
 */
class Space3
{
public:
    Space3(std::shared_ptr<const Mesh3> mesh, int order);

    const Mesh3& mesh() const { return *a_mesh; }
    std::shared_ptr<const Mesh3> meshPtr() const { return a_mesh; }
    /// The plan's space, of the same order.
    const Space& plan() const { return *a_plan; }
    int order() const { return a_order; }
    int size() const { return a_size; }
    int perElement() const { return a_order == 1 ? 6 : 18; }
    int elements() const { return int(a_mesh->prisms.size()); }
    const int* dofs(int e) const { return &a_dofs[std::size_t(e) * std::size_t(perElement())]; }
    /// Where a degree of freedom is (m).
    Vec3 point(int dof) const;
    /// A degree of freedom's plan degree of freedom and its level: its height
    /// (order 1), or twice its height and one more at a layer's middle (order
    /// 2).
    std::pair<int, int> origin(int dof) const { return a_origin[std::size_t(dof)]; }
    /// The z (m) of a level (as origin() gives it).
    double levelZ(int level) const;
    /// The degree of freedom of plan degree of freedom \a planDof at \a
    /// level, or -1.
    int dofAt(int planDof, int level) const;

    /// The map of prism \a e at reference point (ξ, η, ζ): the point (m),
    /// the Jacobian's determinant, its inverse transposed (block diagonal:
    /// the triangle's, then 1/(dz/dζ)).
    struct Map {
        Vec3 x;
        double det = 0;
        double invT[2][2];
        double invDz = 0;
    };
    Map map(int e, double xi, double eta, double zeta) const;
    /// A shape function's gradient (m^-1) from its reference one.
    static void gradient(const Map& m, double dxi, double deta, double dzeta, double g[3])
    {
        g[0] = m.invT[0][0] * dxi + m.invT[0][1] * deta;
        g[1] = m.invT[1][0] * dxi + m.invT[1][1] * deta;
        g[2] = m.invDz * dzeta;
    }

    /// A point of a rule on one of the mesh's boundary faces (Mesh3::faces):
    /// its shape functions' values there - over the face's own degrees of
    /// freedom, faceDofs() -, the point (m) and its weight (the area it
    /// stands for, m²).
    struct FacePoint {
        double n[9];
        Vec3 x;
        double weight = 0;
        /// The same point as a point of a prism beside the face (one of \a
        /// domain's where it has one): what reads a field there reads it.
        int prism = -1;
        double xi = 0, eta = 0, zeta = 0;
    };
    /// The degrees of freedom on boundary face \a f: a floor's its triangle's
    /// at its height (3, or 6), a wall's its edge's at its two heights (4, or
    /// at three levels 9) - as facePoints() orders them.
    std::vector<int> faceDofs(int f) const;
    /// The points of a rule of degree \a degree on boundary face \a f, each
    /// with a prism beside it - one of domain \a domain's where there is one.
    std::vector<FacePoint> facePoints(int f, int degree, int domain = -1) const;
    /// The prism of plan triangle \a t at height \a j, or -1.
    int prismAt(int t, int j) const;

private:
    std::shared_ptr<const Mesh3> a_mesh;
    std::shared_ptr<const Space> a_plan;
    int a_order = 1;
    int a_size = 0;
    std::vector<int> a_dofs;
    std::vector<std::pair<int, int>> a_origin;   // by dof: plan dof, level
    std::vector<int> a_prismAt;                  // [t * heights + j]
    std::unordered_map<long long, int> a_key;    // order 2: plan dof * levels + level
    int a_levels = 0;
    // By plan edge (Mesh::edges): the triangles on it and their edge's
    // local index (-1: none).
    std::vector<std::array<std::pair<int, int>, 2>> a_edgeTriangles;
};

} // namespace qucs_s::fem

#endif // QUCS_FEM3_FE_H
