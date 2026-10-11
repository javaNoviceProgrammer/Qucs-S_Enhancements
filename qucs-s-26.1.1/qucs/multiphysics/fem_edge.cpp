/*
 * fem_edge.cpp - Nédélec's edge elements and the Krylov-Schur eigensolver
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#include "fem_edge.h"

#include <Eigen/Dense>

#include <QCoreApplication>

#include <algorithm>
#include <cmath>

namespace qucs_s::fem {

namespace {

QString tr(const char* text)
{
    return QCoreApplication::translate("qucs_s::fem::EdgeSpace", text);
}

unsigned long long nodePair(int a, int b)
{
    if (a > b) std::swap(a, b);
    return (static_cast<unsigned long long>(unsigned(a)) << 32) | unsigned(b);
}

} // namespace

AffineMap affineMap(const Space& space, int t)
{
    AffineMap m;
    const std::array<QPointF, 6>& g = space.nodes(t);
    m.p0 = g[0];
    m.p1 = g[1];
    m.p2 = g[2];
    const double xx = m.p1.x() - m.p0.x(), xe = m.p2.x() - m.p0.x(), yx = m.p1.y() - m.p0.y(), ye = m.p2.y() - m.p0.y();
    m.det = xx * ye - xe * yx;
    // ∇ = J⁻ᵀ ∇̂: the reference gradients (-1, -1), (1, 0), (0, 1).
    const double i00 = ye / m.det, i01 = -yx / m.det, i10 = -xe / m.det, i11 = xx / m.det;
    const double ref[3][2] = {{-1, -1}, {1, 0}, {0, 1}};
    for (int k = 0; k < 3; ++k) {
        m.grad[k][0] = i00 * ref[k][0] + i01 * ref[k][1];
        m.grad[k][1] = i10 * ref[k][0] + i11 * ref[k][1];
    }
    return m;
}

// ------------------------------------------------------------------ EdgeSpace

EdgeSpace::EdgeSpace(std::shared_ptr<const Mesh> mesh, int order) : a_mesh(std::move(mesh)), a_order(order == 2 ? 2 : 1)
{
    const Mesh& m = *a_mesh;
    const int nt = int(m.triangles.size());
    a_edgeIndex.assign(std::size_t(nt) * 3, -1);
    for (int t = 0; t < nt; ++t) {
        const auto& v = m.triangles[std::size_t(t)];
        for (int k = 0; k < 3; ++k) {
            const int a = v[std::size_t(k)], b = v[std::size_t((k + 1) % 3)];
            const unsigned long long key = nodePair(a, b);
            auto it = a_byNodes.find(key);
            if (it == a_byNodes.end()) {
                it = a_byNodes.emplace(key, int(a_edges.size())).first;
                a_edges.push_back({std::min(a, b), std::max(a, b)});
            }
            a_edgeIndex[std::size_t(t) * 3 + std::size_t(k)] = it->second;
        }
    }
    const int ne = int(a_edges.size());
    const int per = perElement();
    a_dofs.assign(std::size_t(nt) * std::size_t(per), -1);
    a_signs.assign(std::size_t(nt) * std::size_t(per), 1);
    // Order 1: an edge's Whitney function. Order 2: its gradient function
    // too (ne more), and two inside each element.
    a_size = a_order == 1 ? ne : 2 * ne + 2 * nt;
    for (int t = 0; t < nt; ++t) {
        const auto& v = m.triangles[std::size_t(t)];
        int* d = &a_dofs[std::size_t(t) * std::size_t(per)];
        signed char* s = &a_signs[std::size_t(t) * std::size_t(per)];
        for (int k = 0; k < 3; ++k) {
            const int e = a_edgeIndex[std::size_t(t) * 3 + std::size_t(k)];
            const int a = v[std::size_t(k)], b = v[std::size_t((k + 1) % 3)];
            d[k] = e;
            s[k] = a < b ? 1 : -1;
            if (a_order == 2) {
                d[3 + k] = ne + e;   // the gradient ∇(λa λb): the same either way
                s[3 + k] = 1;
            }
        }
        if (a_order == 2) {
            d[6] = 2 * ne + 2 * t;
            d[7] = 2 * ne + 2 * t + 1;
        }
    }
}

std::vector<int> EdgeSpace::edgeDofs(int a, int b) const
{
    auto it = a_byNodes.find(nodePair(a, b));
    if (it == a_byNodes.end()) return {};
    if (a_order == 1) return {it->second};
    return {it->second, int(a_edges.size()) + it->second};
}

void EdgeSpace::basis(int t, const AffineMap& m, double xi, double eta, Basis& b) const
{
    const double lambda[3] = {1 - xi - eta, xi, eta};
    const signed char* s = signs(t);
    auto cross = [](const double* u, const double* v) { return u[0] * v[1] - u[1] * v[0]; };
    b.count = perElement();
    for (int k = 0; k < 3; ++k) {
        const int a = k, c = (k + 1) % 3;
        const double* ga = m.grad[a];
        const double* gc = m.grad[c];
        // Whitney's: λa ∇λb - λb ∇λa, its curl 2 ∇λa × ∇λb.
        b.vx[k] = s[k] * (lambda[a] * gc[0] - lambda[c] * ga[0]);
        b.vy[k] = s[k] * (lambda[a] * gc[1] - lambda[c] * ga[1]);
        b.curl[k] = s[k] * 2 * cross(ga, gc);
        if (a_order == 2) {
            // ∇(λa λb): no curl.
            b.vx[3 + k] = lambda[a] * gc[0] + lambda[c] * ga[0];
            b.vy[3 + k] = lambda[a] * gc[1] + lambda[c] * ga[1];
            b.curl[3 + k] = 0;
        }
    }
    if (a_order == 2) {
        // Inside: λ2 W01 and λ0 W12 (λ1 W20 is minus their sum), their
        // tangential parts nought on every edge. curl(f W) = f curl W + ∇f × W.
        const int faces[2][3] = {{2, 0, 1}, {0, 1, 2}};   // c, then the edge a-b
        for (int f = 0; f < 2; ++f) {
            const int c = faces[f][0], a = faces[f][1], e = faces[f][2];
            const double* ga = m.grad[a];
            const double* ge = m.grad[e];
            const double* gc = m.grad[c];
            const double wx = lambda[a] * ge[0] - lambda[e] * ga[0], wy = lambda[a] * ge[1] - lambda[e] * ga[1];
            const double curlW = 2 * cross(ga, ge);
            b.vx[6 + f] = lambda[c] * wx;
            b.vy[6 + f] = lambda[c] * wy;
            const double w[2] = {wx, wy};
            b.curl[6 + f] = lambda[c] * curlW + cross(gc, w);
        }
    }
}

// ------------------------------------------------------------------ Eigensolver

namespace {

using cd = std::complex<double>;

/// Swaps diagonal entries i and i + 1 of upper triangular T (Q its Schur
/// vectors), T and Q kept a Schur decomposition.
void swapSchur(Eigen::MatrixXcd& T, Eigen::MatrixXcd& Q, int i)
{
    const int m = int(T.rows());
    const cd a = T(i, i), b = T(i + 1, i + 1), c = T(i, i + 1);
    Eigen::Vector2cd v(c, b - a);
    const double n = v.norm();
    if (n == 0) return;
    v /= n;
    // G = [v, w], unitary: its first column the eigenvector of b.
    Eigen::Matrix2cd G;
    G(0, 0) = v(0);
    G(1, 0) = v(1);
    G(0, 1) = -std::conj(v(1));
    G(1, 1) = std::conj(v(0));
    T.block(i, i, 2, m - i) = G.adjoint() * T.block(i, i, 2, m - i);
    T.block(0, i, i + 2, 2) = T.block(0, i, i + 2, 2) * G;
    Q.block(0, i, Q.rows(), 2) = Q.block(0, i, Q.rows(), 2) * G;
    T(i + 1, i) = 0;
}

} // namespace

EigenResult largestEigenvalues(int n, int count, const std::function<void(const Eigen::VectorXcd&, Eigen::VectorXcd&)>& apply, int basis,
                               double tolerance, int maxRestarts)
{
    EigenResult out;
    if (n <= 0 || count <= 0) return out;
    count = std::min(count, n);
    int m = basis > 0 ? basis : std::max(2 * count + 1, 20);
    m = std::min(m, n);
    if (m <= count) m = std::min(n, count + 1);
    const int keepTarget = std::min(m - 1, std::max(count, (m + count) / 2));
    Eigen::MatrixXcd V(n, m + 1);
    Eigen::MatrixXcd H = Eigen::MatrixXcd::Zero(m + 1, m);
    // A start that is the same each time (no random state): of every entry.
    Eigen::VectorXcd v0(n);
    for (int i = 0; i < n; ++i) v0(i) = cd(1.0 + 0.37 * std::sin(1.3 * i), 0.21 * std::cos(0.7 * i));
    V.col(0) = v0 / v0.norm();
    int k = 0;
    Eigen::VectorXcd w(n);
    std::vector<int> order;
    Eigen::MatrixXcd T, Q;
    for (int restart = 0; restart <= maxRestarts; ++restart) {
        // Arnoldi from k to m: orthogonalized twice.
        for (int j = k; j < m; ++j) {
            apply(V.col(j), w);
            ++out.products;
            // (The operator's own scale: (A - σB)⁻¹B may be 1e-13 at optical
            // frequencies, so a breakdown is relative to what it gave.)
            const double scale = w.norm();
            Eigen::VectorXcd h = V.leftCols(j + 1).adjoint() * w;
            w -= V.leftCols(j + 1) * h;
            const Eigen::VectorXcd h2 = V.leftCols(j + 1).adjoint() * w;
            w -= V.leftCols(j + 1) * h2;
            h += h2;
            H.block(0, j, j + 1, 1) = h;
            const double beta = w.norm();
            H(j + 1, j) = beta;
            if (beta <= 1e-12 * scale || scale == 0) {
                // An invariant subspace: a new direction, orthogonal.
                Eigen::VectorXcd r(n);
                for (int i = 0; i < n; ++i) r(i) = cd(std::cos(0.91 * i + j), std::sin(0.53 * i * (j + 1)));
                r -= V.leftCols(j + 1) * (V.leftCols(j + 1).adjoint() * r);
                r -= V.leftCols(j + 1) * (V.leftCols(j + 1).adjoint() * r);
                H(j + 1, j) = 0;
                V.col(j + 1) = r / r.norm();
            } else {
                V.col(j + 1) = w / beta;
            }
        }
        // H's m × m part in Schur form, its largest first.
        Eigen::ComplexSchur<Eigen::MatrixXcd> schur(H.topRows(m));
        T = schur.matrixT();
        Q = schur.matrixU();
        for (int i = 0; i < m; ++i)
            for (int j = m - 1; j > i; --j)
                if (std::abs(T(j, j)) > std::abs(T(j - 1, j - 1))) swapSchur(T, Q, j - 1);
        // The residuals of the wanted Ritz pairs: |β (Q y)_m|.
        const cd beta = H(m, m - 1);
        const Eigen::MatrixXcd Tc = T.topLeftCorner(count, count);
        Eigen::ComplexEigenSolver<Eigen::MatrixXcd> small(Tc);
        int converged = 0;
        for (int i = 0; i < count; ++i) {
            const Eigen::VectorXcd y = Q.leftCols(count) * small.eigenvectors().col(i);
            const double residual = std::abs(beta * y(m - 1));
            if (residual <= tolerance * std::max(std::abs(small.eigenvalues()(i)), 1e-300)) ++converged;
        }
        out.restarts = restart;
        if (converged == count || restart == maxRestarts) {
            // The eigenvectors: V Q y, largest first.
            Eigen::ComplexEigenSolver<Eigen::MatrixXcd> fin(T.topLeftCorner(count, count));
            std::vector<int> idx(static_cast<std::size_t>(count));
            for (int i = 0; i < count; ++i) idx[std::size_t(i)] = i;
            std::sort(idx.begin(), idx.end(), [&](int a, int b) { return std::abs(fin.eigenvalues()(a)) > std::abs(fin.eigenvalues()(b)); });
            for (int i : idx) {
                const Eigen::VectorXcd y = Q.leftCols(count) * fin.eigenvectors().col(i);
                const double residual = std::abs(beta * y(m - 1));
                if (residual > 1e3 * tolerance * std::max(std::abs(fin.eigenvalues()(i)), 1e-300)) continue;
                Eigen::VectorXcd x = V.leftCols(m) * y;
                x /= x.norm();
                out.values.push_back(fin.eigenvalues()(i));
                out.vectors.push_back(x);
            }
            if (converged < count) out.error = tr("%1 of %2 eigenvalues converged in %3 restarts").arg(converged).arg(count).arg(restart);
            return out;
        }
        // Restart: the wanted Schur vectors kept, the residual direction after.
        k = keepTarget;
        Eigen::MatrixXcd Vk = V.leftCols(m) * Q.leftCols(k);
        V.leftCols(k) = Vk;
        V.col(k) = V.col(m);
        H.setZero();
        H.topLeftCorner(k, k) = T.topLeftCorner(k, k);
        H.block(k, 0, 1, k) = beta * Q.block(m - 1, 0, 1, k);
    }
    return out;
}

} // namespace qucs_s::fem
