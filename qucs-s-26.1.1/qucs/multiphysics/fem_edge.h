/*
 * fem_edge.h - Nédélec's edge elements of the first kind on the mesh's
 *              triangles: of order 1 (Whitney's, one on each edge) and 2
 *              (hierarchical, as Webb's: two on each edge, two inside) -
 *              a field whose tangential part is continuous, beside a
 *              Lagrange field of the same order (a mode's Et and Ez); and
 *              a small eigensolver for what they give (Krylov-Schur, with
 *              a shift and invert about a guess)
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#ifndef QUCS_FEM_EDGE_H
#define QUCS_FEM_EDGE_H

#include "fem_fe.h"

#include <QString>

#include <Eigen/Core>

#include <array>
#include <complex>
#include <functional>
#include <memory>
#include <unordered_map>
#include <vector>

namespace qucs_s::fem {

/// An element's map as its corners make it (affine: a curved edge's
/// middle not minded), its barycentric coordinates' gradients (m^-1).
struct AffineMap {
    double det = 0;
    double grad[3][2];   // ∇λ0, ∇λ1, ∇λ2
    QPointF p0, p1, p2;  // its corners (m)
    QPointF at(double xi, double eta) const { return p0 + (p1 - p0) * xi + (p2 - p0) * eta; }
};
AffineMap affineMap(const Space& space, int t);

/*!
 * The edge elements of a mesh: the degrees of freedom on its edges (and,
 * of order 2, inside each triangle), each edge's from its lower node to
 * its higher, an element's basis functions and their curls where it is.
 */
class EdgeSpace
{
public:
    EdgeSpace(std::shared_ptr<const Mesh> mesh, int order);

    const Mesh& mesh() const { return *a_mesh; }
    int order() const { return a_order; }
    int size() const { return a_size; }
    int perElement() const { return a_order == 1 ? 3 : 8; }
    int elements() const { return int(a_mesh->triangles.size()); }
    const int* dofs(int t) const { return &a_dofs[std::size_t(t) * std::size_t(perElement())]; }
    /// Each local function's sign: +1 when the element's edge runs as the
    /// global edge does (Whitney's functions turn with it).
    const signed char* signs(int t) const { return &a_signs[std::size_t(t) * std::size_t(perElement())]; }
    /// The degrees of freedom on the mesh's edge from node \a a to \a b.
    std::vector<int> edgeDofs(int a, int b) const;

    /// The basis functions of element \a t at reference point (xi, eta):
    /// their values (x, y) and curls (z), signs applied.
    struct Basis {
        int count = 3;
        double vx[8], vy[8], curl[8];
    };
    void basis(int t, const AffineMap& m, double xi, double eta, Basis& b) const;

private:
    std::shared_ptr<const Mesh> a_mesh;
    int a_order = 1;
    int a_size = 0;
    std::vector<int> a_dofs;
    std::vector<signed char> a_signs;
    std::vector<std::array<int, 2>> a_edges;     // by global edge: its nodes, lower first
    std::vector<int> a_edgeIndex;                // by element and local edge: its global edge
    std::unordered_map<unsigned long long, int> a_byNodes;
};

/// What an eigensolver found: the eigenvalues (as many as converged, the
/// wanted first) and their vectors.
struct EigenResult {
    std::vector<std::complex<double>> values;
    std::vector<Eigen::VectorXcd> vectors;
    int restarts = 0;
    int products = 0;
    QString error;
};
/*!
 * The \a count eigenvalues of largest magnitude of the operator \a apply
 * (y = Op x, n by n), by the Krylov-Schur method (Stewart's): an Arnoldi
 * basis of \a basis vectors (0: chosen), restarted on its sorted Schur
 * form's wanted part until each wanted pair's residual is under \a
 * tolerance relative to its value. With Op = (A - σB)⁻¹B these are the
 * eigenvalues of A x = λ B x nearest σ: λ = σ + 1/θ.
 */
EigenResult largestEigenvalues(int n, int count, const std::function<void(const Eigen::VectorXcd&, Eigen::VectorXcd&)>& apply,
                               int basis = 0, double tolerance = 1e-10, int maxRestarts = 300);

} // namespace qucs_s::fem

#endif // QUCS_FEM_EDGE_H
