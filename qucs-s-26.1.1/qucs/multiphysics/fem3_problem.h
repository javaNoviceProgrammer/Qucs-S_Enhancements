/*
 * fem3_problem.h - what the 3D solver's physics have in common (not
 *                  installed: the solver's own): a physics as a problem on a
 *                  3D solution, and a symmetric system solved by multigrid or
 *                  a Cholesky
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#ifndef QUCS_FEM3_PROBLEM_H
#define QUCS_FEM3_PROBLEM_H

#include "fem3_solver.h"
#include "fem_amg.h"
#include "fem_sparse.h"

#include <QSet>

#include <functional>
#include <memory>

namespace qucs_s::fem::detail3 {

QString tr(const char* text);

/// How a 3D physics is solved, once.
struct Options3 {
    QString solver = QStringLiteral("auto");   ///< auto, direct or iterative
    double tolerance = 1e-6;                   ///< its nonlinear iterations', and (to 1e-9 at most) the iterative solve's
    int maxIterations = 50;                    ///< its nonlinear iterations
    bool sweep = true;                         ///< the terminals' matrix
    /// A step in time's: what multiplies the capacity's ∂u/∂t of u (1/h,
    /// BDF2's (1 + 2ω)/((1 + ω)h)), and the history it is taken from by dof
    /// - C (mass u - history) + K u = F. 0: at rest.
    double mass = 0;
    const std::vector<double>* history = nullptr;
    /// A physics that reads its own field: by Newton's method (auto, newton)
    /// or Picard's iterations (picard).
    QString nonlinear = QStringLiteral("auto");
    std::function<bool()> cancelled;
};

/*!
 * A symmetric positive definite system solved for as many right-hand sides
 * as wanted: by conjugate gradients preconditioned by algebraic multigrid
 * (AmgSolver) - or, asked for, or (auto) on macOS where more right-hand sides
 * than about 6 + n/20,000 share a matrix of no more than 300,000 unknowns, by
 * Accelerate's Cholesky in METIS's order (an iterative solve is quicker for
 * one right-hand side at every size measured). Off macOS, the direct solve
 * asked for is Eigen's. What it did is said in the log.
 */
class System3
{
public:
    /// \a a factorized, or set up for its iterations; \a symmetric false (a
    /// Newton's matrix): by an LU (direct) or BiCGStab with multigrid.
    bool factorize(const Eigen::SparseMatrix<double>& a, const QString& solver, int rhs, double tolerance, const QString& name,
                   QStringList* log, QString* error, bool symmetric = true);
    /// Solves for \a b, from \a x as it is (an iterative solve's guess).
    bool solve(const Eigen::VectorXd& b, Eigen::VectorXd& x, const QString& name, QStringList* log, QString* error);
    bool direct() const { return a_chol != nullptr || a_lu != nullptr; }
    int size() const { return a_n; }

private:
    std::unique_ptr<AmgSolver> a_amg;
    std::unique_ptr<SparseCholesky> a_chol;
    std::unique_ptr<SparseLu<double>> a_lu;
    bool a_symmetric = true;
    int a_n = 0;
    double a_tolerance = 1e-9;
};

class Problem3
{
public:
    Problem3(Solution3& solution, int field);
    virtual ~Problem3() = default;
    /// Reads the model; false and why (every problem found) in \a error.
    virtual bool setup(QString* error) = 0;
    /// Its field's initial values.
    virtual void initialize() {}
    /// Assembles and solves (one that reads its own field: Picard's
    /// iterations); false and why.
    virtual bool solve(const Options3& options, QString* error) = 0;
    /// Its globals from the field as solved.
    virtual void setGlobals() = 0;
    /// Whether its field changes in time (heat: ρ Cp ∂T/∂t), or is at rest
    /// at each step (electrostatics, currents).
    virtual bool hasMass() const { return false; }
    /// Whether what it reads reads field \a f.
    bool reads(int f) const { return a_reads.contains(f); }
    int index() const { return a_fi; }
    Field3& field() { return a_s.fields()[std::size_t(a_fi)]; }
    const Node* node() const { return a_node; }

protected:
    void problem(const QString& what)
    {
        if (!a_problems.contains(what)) a_problems << what;
    }
    /// \a text of property \a key of \a feature compiled in the point
    /// scope, the fields it reads noted; one read where \a spatial is not,
    /// an error; its unit against the property's (a warning, once).
    Expression compile(const QString& text, const Node& feature, const QString& key, bool spatial = true);
    /// The material of each domain: the last that has it (null: none).
    std::vector<const Node*> materials();

    Solution3& a_s;
    int a_fi;
    const Node* a_node = nullptr;
    QSet<int> a_reads;
    QStringList a_problems;
};

/// The problem of field \a field of \a solution (heat, electrostatics,
/// currents).
std::unique_ptr<Problem3> makeScalarProblem3(Solution3& solution, int field);

} // namespace qucs_s::fem::detail3

#endif // QUCS_FEM3_PROBLEM_H
