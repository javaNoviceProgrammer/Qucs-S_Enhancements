/*
 * fem3_ams.cpp - an iterative solve of edge element systems: the auxiliary
 *                space preconditioner (AMS) and restarted GMRES
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#include "fem3_ams.h"

#include <QCoreApplication>

#include <cmath>
#include <vector>

namespace qucs_s::fem::detail3 {

namespace {

QString tr(const char* text)
{
    return QCoreApplication::translate("AmsPreconditioner", text);
}

} // namespace

namespace {

class MultigridCycle : public Cycle
{
public:
    AmgCycle amg;
    void apply(const Eigen::VectorXd& r, Eigen::VectorXd& x) const override { amg.apply(r, x); }
    std::size_t bytes() const override { return amg.bytes(); }
};

std::size_t matrixBytes(const Eigen::SparseMatrix<double>& m)
{
    return std::size_t(m.nonZeros()) * 12 + std::size_t(m.outerSize()) * 4;
}

} // namespace

CycleMaker multigrid(int blockSize)
{
    return [blockSize](const Eigen::SparseMatrix<double>& a, QString* error) -> std::unique_ptr<Cycle> {
        auto c = std::make_unique<MultigridCycle>();
        c->amg.setBlockSize(blockSize);
        if (!c->amg.compute(a, error)) return nullptr;
        return c;
    };
}

bool AuxiliaryCycle::compute(const Eigen::SparseMatrix<double>& a, std::vector<Space> spaces, std::vector<int> order, int sweeps, QString* error)
{
    a_a = a;
    a_a.makeCompressed();
    a_diag = a.diagonal();
    for (Eigen::Index i = 0; i < a_diag.size(); ++i)
        if (!(a_diag(i) > 0)) {
            if (error) *error = tr("a preconditioner's matrix is not positive on its diagonal (row %1)").arg(i + 1);
            return false;
        }
    a_sweeps = std::max(1, sweeps);
    a_order = std::move(order);
    a_p.clear();
    a_inner.clear();
    for (Space& s : spaces) {
        const Eigen::SparseMatrix<double> coarse = Eigen::SparseMatrix<double>(s.p.transpose()) * (a * s.p);
        std::unique_ptr<Cycle> c = s.make(coarse, error);
        if (!c) return false;
        a_p.push_back(std::move(s.p));
        a_inner.push_back(std::move(c));
    }
    return true;
}

void AuxiliaryCycle::smooth(const Eigen::VectorXd& r, Eigen::VectorXd& d, bool forward) const
{
    // (D + L) d = r forward, (D + U) d = r backward, from nought.
    const Eigen::Index n = a_a.rows();
    d = Eigen::VectorXd::Zero(n);
    const int* outer = a_a.outerIndexPtr();
    const int* inner = a_a.innerIndexPtr();
    const double* v = a_a.valuePtr();
    for (Eigen::Index k = 0; k < n; ++k) {
        const Eigen::Index i = forward ? k : n - 1 - k;
        double s = r(i);
        for (int p = outer[i]; p < outer[i + 1]; ++p) {
            const int j = inner[p];
            if (forward ? j < i : j > i) s -= v[p] * d(j);
        }
        d(i) = s / a_diag(i);
    }
}

void AuxiliaryCycle::apply(const Eigen::VectorXd& r, Eigen::VectorXd& x) const
{
    Eigen::VectorXd d, y;
    smooth(r, x, true);
    for (int k = 1; k < a_sweeps; ++k) {
        smooth(r - a_a * x, d, true);
        x += d;
    }
    for (int s : a_order) {
        const Eigen::VectorXd res = r - a_a * x;
        a_inner[std::size_t(s)]->apply(a_p[std::size_t(s)].transpose() * res, y);
        x += a_p[std::size_t(s)] * y;
    }
    for (int k = 0; k < a_sweeps; ++k) {
        smooth(r - a_a * x, d, false);
        x += d;
    }
}

std::size_t AuxiliaryCycle::bytes() const
{
    std::size_t b = std::size_t(a_a.nonZeros()) * 12 + std::size_t(a_a.outerSize()) * 4;
    for (const auto& p : a_p) b += matrixBytes(p);
    for (const auto& c : a_inner) b += c->bytes();
    return b;
}

IterativeReport gmres(const ComplexOperator& a, const ComplexOperator& m, const Eigen::VectorXcd& b, Eigen::VectorXcd& x, double tolerance,
                      int maxIterations, int restart)
{
    using cd = std::complex<double>;
    IterativeReport rep;
    const Eigen::Index n = b.size();
    if (x.size() != n) x = Eigen::VectorXcd::Zero(n);
    const double bn = b.norm();
    if (n == 0 || !(bn > 0)) {
        x.setZero();
        rep.converged = true;
        return rep;
    }
    if (!std::isfinite(bn)) return rep;
    const int mr = std::max(2, restart);
    Eigen::MatrixXcd V(n, mr + 1);
    Eigen::MatrixXcd H = Eigen::MatrixXcd::Zero(mr + 1, mr);
    std::vector<double> cs(std::size_t(mr), 0.0);
    std::vector<cd> sn(std::size_t(mr), cd(0));
    Eigen::VectorXcd g(mr + 1), w, z, r;
    const auto residual = [&] {
        a(x, w);
        r = b - w;
        return r.norm();
    };
    double beta = residual();
    rep.residual = beta / bn;
    while (rep.residual > tolerance && rep.iterations < maxIterations) {
        V.col(0) = r / beta;
        g.setZero();
        g(0) = beta;
        H.setZero();
        int j = 0;
        for (; j < mr && rep.iterations < maxIterations; ++j) {
            ++rep.iterations;
            m(V.col(j), z);
            a(z, w);
            // (Modified Gram-Schmidt, twice: the basis kept orthonormal.)
            for (int pass = 0; pass < 2; ++pass)
                for (int i = 0; i <= j; ++i) {
                    const cd h = V.col(i).dot(w);
                    H(i, j) += h;
                    w -= h * V.col(i);
                }
            const double hn = w.norm();
            H(j + 1, j) = hn;
            if (hn > 0) V.col(j + 1) = w / hn;
            for (int i = 0; i < j; ++i) {
                const cd t = cs[std::size_t(i)] * H(i, j) + sn[std::size_t(i)] * H(i + 1, j);
                H(i + 1, j) = -std::conj(sn[std::size_t(i)]) * H(i, j) + cs[std::size_t(i)] * H(i + 1, j);
                H(i, j) = t;
            }
            const cd hj = H(j, j);
            const double ha = std::abs(hj), t = std::hypot(ha, hn);
            if (ha == 0) {
                cs[std::size_t(j)] = 0;
                sn[std::size_t(j)] = 1;
            } else {
                cs[std::size_t(j)] = ha / t;
                sn[std::size_t(j)] = hj / ha * hn / t;
            }
            H(j, j) = cs[std::size_t(j)] * hj + sn[std::size_t(j)] * hn;
            H(j + 1, j) = 0;
            g(j + 1) = -std::conj(sn[std::size_t(j)]) * g(j);
            g(j) = cs[std::size_t(j)] * g(j);
            if (std::abs(g(j + 1)) / bn <= tolerance || !(hn > 0)) {
                ++j;
                break;
            }
        }
        // x += M (V y), H y = g on the rows solved.
        const Eigen::VectorXcd y = H.topLeftCorner(j, j).triangularView<Eigen::Upper>().solve(g.head(j));
        m(V.leftCols(j) * y, z);
        x += z;
        if (!x.allFinite()) break;
        beta = residual();
        rep.residual = beta / bn;
        if (!(beta > 0)) break;
    }
    rep.converged = rep.residual <= tolerance * 1.0000001 && x.allFinite();
    return rep;
}

} // namespace qucs_s::fem::detail3
